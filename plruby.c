/**********************************************************************
 * plruby.c - Ruby as a procedural language for PostgreSQL
 *
 * PL/Ruby embeds a Ruby (MRI) interpreter in the PostgreSQL backend and
 * runs function/trigger/procedure bodies written in Ruby.  Its structure
 * follows the sibling PL/php handler; the Zend/PHP embed API is replaced
 * with Ruby's C API.
 *
 * Copyright (c) 2026 ChronicallyJD.  Distributed under the MIT License;
 * see the LICENSE file at the root of the source tree.
 **********************************************************************/

#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"
#include "parser/parse_coerce.h"
#if PG_VERSION_NUM >= 130000
#include "tcop/cmdtag.h"
#endif
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "plruby.h"
#include "plruby_io.h"
#include "plruby_spi.h"

PG_MODULE_MAGIC;

/*
 * FunctionCallInfo argument access; PG 12 replaced arg[]/argnull[] with a
 * NullableDatum args[] array.
 */
#if PG_VERSION_NUM >= 120000
#define PLRUBY_ARG_ISNULL(fcinfo, n)	((fcinfo)->args[n].isnull)
#define PLRUBY_ARG_VALUE(fcinfo, n)		((fcinfo)->args[n].value)
#else
#define PLRUBY_ARG_ISNULL(fcinfo, n)	((fcinfo)->argnull[n])
#define PLRUBY_ARG_VALUE(fcinfo, n)		((fcinfo)->arg[n])
#endif

#define IS_ARGMODE_OUT(mode) ((mode) == PROARGMODE_OUT || \
							  (mode) == PROARGMODE_TABLE)

typedef enum pl_type
{
	PL_TUPLE = 1 << 0,
	PL_ARRAY = 1 << 1,
	PL_PSEUDO = 1 << 2
} pl_type;

typedef struct plruby_proc_desc
{
	char	   *proname;		/* internal (OID-derived) method name */
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	pl_type		ret_type;
	Oid			ret_oid;
	bool		retset;
	FmgrInfo	result_in_func;
	Oid			result_typioparam;
	int			n_total_args;
	int			n_out_args;
	int			n_mixed_args;
	Oid			arg_type[FUNC_MAX_ARGS];
	char		arg_typtype[FUNC_MAX_ARGS];
	char		arg_argmode[FUNC_MAX_ARGS];
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];

	/*
	 * TRANSFORM FOR TYPE support: the FromSQL function for each argument and
	 * the ToSQL function for the return value (InvalidOid when the function
	 * declares no transform for that type).  A FromSQL function returns the
	 * Ruby VALUE as a Datum; a ToSQL function accepts one.
	 */
	Oid			arg_transform[FUNC_MAX_ARGS];
	Oid			ret_transform;
} plruby_proc_desc;

/* Interpreter-wide Ruby objects (see plruby.h) */
VALUE		rb_mPLRuby;
VALUE		rb_ePLRubyError;
VALUE		plruby_recv;
VALUE		plruby_proc_cache;

/* Global state */
static bool plruby_first_call = true;
static bool plruby_vm_inited = false;
static char *plruby_start_proc = NULL;
static bool plruby_session_inited = false;

/* Forward declarations */
void		_PG_init(void);
void		plruby_init(void);
static void plruby_init_all(void);
static void plruby_session_init(void);
static void plruby_load_modules(void);

PG_FUNCTION_INFO_V1(plruby_call_handler);
PG_FUNCTION_INFO_V1(plruby_validator);
PG_FUNCTION_INFO_V1(plruby_inline_handler);

static Datum plruby_func_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc);
static Datum plruby_srf_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc);
static Datum plruby_trigger_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc);
static Datum plruby_event_trigger_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc);

static plruby_proc_desc *plruby_compile_function(Oid fnoid, bool is_trigger,
												 bool is_event_trigger);
static VALUE plruby_func_build_args(plruby_proc_desc *desc, FunctionCallInfo fcinfo);
static bool is_valid_ruby_identifier(const char *name);

/* ---------------------------------------------------------------------
 * Error bridge: Ruby exceptions <-> PostgreSQL errors
 * ------------------------------------------------------------------- */

static VALUE
msg_to_s_tramp(VALUE err)
{
	return rb_funcall(err, rb_intern("message"), 0);
}

char *
plruby_pop_exception_message(void)
{
	VALUE		err = rb_errinfo();
	char	   *msg;
	int			state = 0;
	VALUE		m;

	if (NIL_P(err))
		return NULL;

	m = rb_protect(msg_to_s_tramp, err, &state);
	if (!state && RB_TYPE_P(m, T_STRING))
		msg = pnstrdup(RSTRING_PTR(m), RSTRING_LEN(m));
	else
		msg = pstrdup("unknown Ruby error");

	rb_set_errinfo(Qnil);
	return msg;
}

/*
 * A string-valued instance variable of the pending exception, as a palloc'd
 * C string, or NULL.  rb_ivar_get cannot raise for a missing ivar.
 */
static char *
plruby_exception_field(VALUE err, const char *ivar)
{
	VALUE		v = rb_ivar_get(err, rb_intern(ivar));

	if (!RB_TYPE_P(v, T_STRING))
		return NULL;
	return pnstrdup(RSTRING_PTR(v), RSTRING_LEN(v));
}

void
plruby_report_exception(void)
{
	VALUE		err = rb_errinfo();
	char	   *detail = NULL;
	char	   *hint = NULL;
	char	   *sqlstate = NULL;
	char	   *msg;

	if (!NIL_P(err))
	{
		detail = plruby_exception_field(err, "@detail");
		hint = plruby_exception_field(err, "@hint");
		sqlstate = plruby_exception_field(err, "@sqlstate");
		if (sqlstate != NULL && strlen(sqlstate) != 5)
			sqlstate = NULL;
	}

	msg = plruby_pop_exception_message();
	if (msg == NULL)
		msg = pstrdup("plruby: unknown error");

	ereport(ERROR,
			(sqlstate ? errcode(MAKE_SQLSTATE(sqlstate[0], sqlstate[1],
											  sqlstate[2], sqlstate[3],
											  sqlstate[4])) : 0,
			 errmsg("%s", msg),
			 detail ? errdetail("%s", detail) : 0,
			 hint ? errhint("%s", hint) : 0));
}

void
plruby_eval_string(const char *source, const char *errctx)
{
	int			state = 0;

	rb_eval_string_protect(source, &state);
	if (state)
	{
		char	   *msg = plruby_pop_exception_message();

		if (msg != NULL)
			elog(ERROR, "%s: %s", errctx, msg);
		else
			elog(ERROR, "%s", errctx);
	}
}

struct pcall
{
	VALUE		recv;
	ID			mid;
	int			argc;
	VALUE	   *argv;
};

static VALUE
pcall_tramp(VALUE p)
{
	struct pcall *c = (struct pcall *) p;

	return rb_funcallv(c->recv, c->mid, c->argc, c->argv);
}

VALUE
plruby_protected_call(VALUE recv, ID mid, int argc, VALUE *argv)
{
	int			state = 0;
	struct pcall c;
	VALUE		r;

	c.recv = recv;
	c.mid = mid;
	c.argc = argc;
	c.argv = argv;

	r = rb_protect(pcall_tramp, (VALUE) &c, &state);
	if (state)
		plruby_report_exception();	/* does not return */
	return r;
}

/* Remove a top-level method previously defined for a temp/inline body. */
static void
plruby_remove_method(const char *name)
{
	char		buf[128];
	int			state = 0;

	snprintf(buf, sizeof(buf),
			 "Object.send(:remove_method, :%s) rescue nil", name);
	rb_eval_string_protect(buf, &state);
	if (state)
		rb_set_errinfo(Qnil);
}

/* ---------------------------------------------------------------------
 * Module init and interpreter startup
 * ------------------------------------------------------------------- */

void
_PG_init(void)
{
	DefineCustomStringVariable("plruby.start_proc",
							   "PL/Ruby function to call when the interpreter "
							   "is first initialized in a session.",
							   NULL,
							   &plruby_start_proc,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("plruby");
#else
	EmitWarningsOnPlaceholders("plruby");
#endif
}

static void
plruby_init_all(void)
{
	if (plruby_first_call)
		plruby_init();
}

void
plruby_init(void)
{
	if (!plruby_first_call)
		return;

	if (!plruby_vm_inited)
	{
		/*
		 * ruby_init() alone leaves the interpreter only partially set up: the
		 * builtin prelude (RUBY_DESCRIPTION and Ruby-level core methods such as
		 * Integer#odd?, Kernel#class, require, ...) is loaded by the normal
		 * startup path, ruby_options().  We run it with a no-op "-e ;" program
		 * (which is parsed but never executed, since we do not call
		 * ruby_run_node) and --disable-gems to avoid pulling in RubyGems.
		 */
		static char *ruby_argv[] = {
			(char *) "plruby", (char *) "--disable-gems",
			(char *) "-e", (char *) ";"
		};
		int			ruby_argc = lengthof(ruby_argv);
		char	  **argv_p = ruby_argv;

		ruby_sysinit(&ruby_argc, &argv_p);
		{
			RUBY_INIT_STACK;
			ruby_init();
			ruby_options(ruby_argc, argv_p);
		}
		plruby_vm_inited = true;
	}

	plruby_eval_string("module PLRuby; class Error < StandardError;"
					   " attr_reader :sqlstate, :detail, :hint; end; end",
					   "plruby: interpreter init");

	rb_mPLRuby = rb_const_get(rb_cObject, rb_intern("PLRuby"));
	rb_ePLRubyError = rb_const_get(rb_mPLRuby, rb_intern("Error"));
	rb_gc_register_address(&rb_mPLRuby);
	rb_gc_register_address(&rb_ePLRubyError);

	/*
	 * Use the top-level "main" object as the receiver for compiled methods
	 * (which are private methods on Object).  Besides being the natural self
	 * for top-level code, its #inspect is the stable string "main", so
	 * NoMethodError/NameError messages are deterministic rather than embedding
	 * a per-backend object address.
	 */
	plruby_recv = rb_eval_string("self");
	rb_gc_register_address(&plruby_recv);

	plruby_proc_cache = rb_hash_new();
	rb_gc_register_address(&plruby_proc_cache);

	rb_gc_register_address(&current_srf_binding);

	plruby_spi_init();

	plruby_first_call = false;
}

/* ---------------------------------------------------------------------
 * Session initialization: modules + start_proc
 * ------------------------------------------------------------------- */

/*
 * plruby_load_modules
 *		If a visible table named plruby_modules(modname, modseq, modsrc) exists,
 *		evaluate each row's Ruby source at the top level (ordered by
 *		modname, modseq) so any methods/classes it defines become available to
 *		every PL/Ruby function in the session.  Must run with SPI connected.
 */
static void
plruby_load_modules(void)
{
	uint64		i;
	int			ret;

	ret = SPI_execute("SELECT 1 FROM pg_class "
					  "WHERE relname = 'plruby_modules' "
					  "AND relkind IN ('r', 'p') "
					  "AND pg_table_is_visible(oid)", true, 1);
	if (ret != SPI_OK_SELECT || SPI_processed == 0)
		return;

	ret = SPI_execute("SELECT modsrc FROM plruby_modules "
					  "ORDER BY modname, modseq", true, 0);
	if (ret != SPI_OK_SELECT)
		return;

	for (i = 0; i < SPI_processed; i++)
	{
		char	   *modsrc = SPI_getvalue(SPI_tuptable->vals[i],
										 SPI_tuptable->tupdesc, 1);
		int			state = 0;

		if (modsrc == NULL)
			continue;

		rb_eval_string_protect(modsrc, &state);
		pfree(modsrc);
		if (state)
		{
			char	   *msg = plruby_pop_exception_message();

			if (msg != NULL)
				elog(ERROR, "plruby: failed to load module: %s", msg);
			else
				elog(ERROR, "plruby: failed to load module");
		}
	}
}

static void
plruby_session_init(void)
{
	if (plruby_session_inited)
		return;
	plruby_session_inited = true;

	plruby_load_modules();

	if (plruby_start_proc != NULL && plruby_start_proc[0] != '\0')
	{
		StringInfoData buf;
		int			ret;

		initStringInfo(&buf);
		appendStringInfo(&buf, "SELECT %s()", plruby_start_proc);
		ret = SPI_execute(buf.data, false, 0);
		if (ret < 0)
			elog(ERROR, "plruby: start_proc \"%s\" failed: %s",
				 plruby_start_proc, SPI_result_code_string(ret));
		pfree(buf.data);
	}
}

/* ---------------------------------------------------------------------
 * Call handler
 * ------------------------------------------------------------------- */

Datum
plruby_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval = (Datum) 0;
	bool		nonatomic = false;
	plruby_proc_desc *desc;

	plruby_init_all();

	if (fcinfo->context && IsA(fcinfo->context, CallContext))
		nonatomic = !((CallContext *) fcinfo->context)->atomic;

	if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to SPI manager")));

	/* Reset SRF state for this call */
	current_fcinfo = NULL;

	plruby_session_init();

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		desc = plruby_compile_function(fcinfo->flinfo->fn_oid, true, false);
		retval = plruby_trigger_handler(fcinfo, desc);
	}
	else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
	{
		desc = plruby_compile_function(fcinfo->flinfo->fn_oid, false, true);
		retval = plruby_event_trigger_handler(fcinfo, desc);
	}
	else
	{
		desc = plruby_compile_function(fcinfo->flinfo->fn_oid, false, false);
		if (desc->retset)
			retval = plruby_srf_handler(fcinfo, desc);
		else
			retval = plruby_func_handler(fcinfo, desc);
	}

	return retval;
}

/* ---------------------------------------------------------------------
 * Regular function handler
 * ------------------------------------------------------------------- */

static Datum
plruby_func_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc)
{
	VALUE		args;
	VALUE		result;
	VALUE		params[2];
	Datum		retval = (Datum) 0;
	char	   *retvalbuffer = NULL;

	Assert(!desc->retset);

	args = plruby_func_build_args(desc, fcinfo);
	params[0] = args;
	params[1] = INT2NUM(desc->n_total_args);

	result = plruby_protected_call(plruby_recv, rb_intern(desc->proname),
								   2, params);

	/* Basic datatype checks */
	if ((desc->ret_type & PL_ARRAY) && !RB_TYPE_P(result, T_ARRAY))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return array must return an array")));
	if ((desc->ret_type & PL_TUPLE) &&
		!RB_TYPE_P(result, T_ARRAY) && !RB_TYPE_P(result, T_HASH))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return tuple must return an array or hash")));

	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	/* Refresh I/O info for pseudo return types resolved at call time */
	if (desc->ret_type & PL_PSEUDO)
	{
		HeapTuple	retTypeTup;
		Form_pg_type retTypeStruct;

		retTypeTup = SearchSysCache1(TYPEOID,
									 ObjectIdGetDatum(get_fn_expr_rettype(fcinfo->flinfo)));
		retTypeStruct = (Form_pg_type) GETSTRUCT(retTypeTup);
		fmgr_info_cxt(retTypeStruct->typinput, &(desc->result_in_func),
					  TopMemoryContext);
		desc->result_typioparam = retTypeStruct->typelem;
		ReleaseSysCache(retTypeTup);
	}

	/* VOID (and procedures called via CALL) ignore the body's value */
	if (desc->ret_oid == VOIDOID)
	{
		fcinfo->isnull = true;
		return (Datum) 0;
	}

	/* A TRANSFORM FOR TYPE return value converts through its ToSQL function */
	if (OidIsValid(desc->ret_transform) && !NIL_P(result))
	{
		fcinfo->isnull = false;
		return OidFunctionCall1(desc->ret_transform, (Datum) result);
	}

	switch (TYPE(result))
	{
		case T_NIL:
			fcinfo->isnull = true;
			break;
		case T_TRUE:
		case T_FALSE:
		case T_FIXNUM:
		case T_BIGNUM:
		case T_FLOAT:
		case T_STRING:
			retvalbuffer = plruby_value_to_cstring(result, false, false);
			break;
		case T_ARRAY:
			if (desc->ret_type & PL_ARRAY)
			{
				Oid			elemtype = get_element_type(desc->ret_oid);

				/*
				 * Array of composite: build the array datum directly so each
				 * element record is assembled recursively.  Scalar arrays keep
				 * the (multidimensional-capable) text path below.
				 */
				if (OidIsValid(elemtype) && type_is_rowtype(elemtype))
				{
					bool		isnull;
					Datum		d = plruby_datum_from_value(result, desc->ret_oid,
															(int32) -1, &isnull);

					if (isnull)
					{
						fcinfo->isnull = true;
						return (Datum) 0;
					}
					return d;
				}
				retvalbuffer = plruby_array_to_pg(result);
			}
			else if (desc->ret_type & PL_TUPLE)
			{
				TupleDesc	td;
				HeapTuple	tup;

				if (desc->ret_oid == RECORDOID)
				{
					ReturnSetInfo *rs = (ReturnSetInfo *) fcinfo->resultinfo;

					if (!rs || !IsA(rs, ReturnSetInfo) || rs->expectedDesc == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("function returning record called in context "
										"that cannot accept type record")));
					td = rs->expectedDesc;
					tup = plruby_htup_from_value(result, td);
				}
				else
				{
					td = lookup_rowtype_tupdesc(desc->ret_oid, (int32) -1);
					tup = plruby_htup_from_value(result, td);
					ReleaseTupleDesc(td);
				}
				return HeapTupleGetDatum(tup);
			}
			else
				elog(ERROR, "this plruby function cannot return arrays");
			break;
		case T_HASH:
			if (desc->ret_type & PL_TUPLE)
			{
				TupleDesc	td;
				HeapTuple	tup;

				if (desc->ret_oid == RECORDOID)
				{
					ReturnSetInfo *rs = (ReturnSetInfo *) fcinfo->resultinfo;

					if (!rs || !IsA(rs, ReturnSetInfo) || rs->expectedDesc == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("function returning record called in context "
										"that cannot accept type record")));
					td = rs->expectedDesc;
					tup = plruby_htup_from_value(result, td);
				}
				else
				{
					td = lookup_rowtype_tupdesc(desc->ret_oid, (int32) -1);
					tup = plruby_htup_from_value(result, td);
					ReleaseTupleDesc(td);
				}
				return HeapTupleGetDatum(tup);
			}
			else
				elog(ERROR, "this plruby function cannot return a hash");
			break;
		default:
			elog(WARNING, "plruby functions cannot return this Ruby type");
			fcinfo->isnull = true;
			break;
	}

	if (!fcinfo->isnull && retvalbuffer != NULL)
	{
		retval = InputFunctionCall(&desc->result_in_func, retvalbuffer,
								   desc->result_typioparam, -1);
		pfree(retvalbuffer);
	}

	return retval;
}

/* ---------------------------------------------------------------------
 * Handlers to be implemented in later tasks
 * ------------------------------------------------------------------- */

static Datum
plruby_srf_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	MemoryContext oldcxt;
	VALUE		args;
	VALUE		params[2];

	/* Save SRF state so nested set-returning calls compose correctly */
	FunctionCallInfo save_fcinfo = current_fcinfo;
	TupleDesc	save_tupledesc = current_tupledesc;
	AttInMetadata *save_attinmeta = current_attinmeta;
	MemoryContext save_memcxt = current_memcxt;
	Tuplestorestate *save_tuplestore = current_tuplestore;
	VALUE		save_binding = current_srf_binding;

	Assert(desc->retset);

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	get_call_result_type(fcinfo, NULL, &tupdesc);
	if (tupdesc == NULL)
		tupdesc = rsi->expectedDesc;
	if (tupdesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot determine result tuple type for PL/Ruby set function")));

	current_fcinfo = fcinfo;
	current_tuplestore = NULL;
	current_srf_binding = Qnil;

	oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	current_memcxt = AllocSetContextCreate(CurTransactionContext,
										   "PL/Ruby SRF context",
										   ALLOCSET_DEFAULT_SIZES);
	current_tupledesc = CreateTupleDescCopy(tupdesc);
	current_attinmeta = TupleDescGetAttInMetadata(current_tupledesc);

	/* The body must call return_next to populate the tuplestore. */
	args = plruby_func_build_args(desc, fcinfo);
	params[0] = args;
	params[1] = INT2NUM(desc->n_total_args);
	plruby_protected_call(plruby_recv, rb_intern(desc->proname), 2, params);

	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	rsi->returnMode = SFRM_Materialize;
	if (current_tuplestore)
	{
		rsi->setResult = current_tuplestore;
		rsi->setDesc = current_tupledesc;
	}

	MemoryContextDelete(current_memcxt);
	MemoryContextSwitchTo(oldcxt);

	/* Restore prior SRF state */
	current_fcinfo = save_fcinfo;
	current_tupledesc = save_tupledesc;
	current_attinmeta = save_attinmeta;
	current_memcxt = save_memcxt;
	current_tuplestore = save_tuplestore;
	current_srf_binding = save_binding;

	return (Datum) 0;
}

/*
 * Build the $_TD Hash for a data-modification trigger.
 */
static VALUE
plruby_trig_build_args(FunctionCallInfo fcinfo)
{
	TriggerData *tdata = (TriggerData *) fcinfo->context;
	TupleDesc	tupdesc = tdata->tg_relation->rd_att;
	VALUE		td = rb_hash_new();
	char	   *relname = SPI_getrelname(tdata->tg_relation);
	char	   *nspname = SPI_getnspname(tdata->tg_relation);
	int			i;

	rb_hash_aset(td, rb_str_new_cstr("name"),
				 rb_str_new_cstr(tdata->tg_trigger->tgname));
	rb_hash_aset(td, rb_str_new_cstr("relid"),
				 ULL2NUM(tdata->tg_relation->rd_id));
	rb_hash_aset(td, rb_str_new_cstr("relname"), rb_str_new_cstr(relname));
	rb_hash_aset(td, rb_str_new_cstr("schemaname"), rb_str_new_cstr(nspname));

	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("event"), rb_str_new_cstr("INSERT"));
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("event"), rb_str_new_cstr("DELETE"));
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("event"), rb_str_new_cstr("UPDATE"));
	else if (TRIGGER_FIRED_BY_TRUNCATE(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("event"), rb_str_new_cstr("TRUNCATE"));
	else
		elog(ERROR, "unknown firing event for trigger function");

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
	{
		if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
			rb_hash_aset(td, rb_str_new_cstr("new"),
						 plruby_hash_from_tuple(tdata->tg_trigtuple, tupdesc));
		else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
			rb_hash_aset(td, rb_str_new_cstr("old"),
						 plruby_hash_from_tuple(tdata->tg_trigtuple, tupdesc));
		else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
		{
			rb_hash_aset(td, rb_str_new_cstr("new"),
						 plruby_hash_from_tuple(tdata->tg_newtuple, tupdesc));
			rb_hash_aset(td, rb_str_new_cstr("old"),
						 plruby_hash_from_tuple(tdata->tg_trigtuple, tupdesc));
		}
	}

	rb_hash_aset(td, rb_str_new_cstr("argc"),
				 INT2NUM(tdata->tg_trigger->tgnargs));
	if (tdata->tg_trigger->tgnargs > 0)
	{
		VALUE		tgargs = rb_ary_new();

		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			rb_ary_push(tgargs, rb_str_new_cstr(tdata->tg_trigger->tgargs[i]));
		rb_hash_aset(td, rb_str_new_cstr("args"), tgargs);
	}

	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("when"), rb_str_new_cstr("BEFORE"));
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("when"), rb_str_new_cstr("AFTER"));
	else if (TRIGGER_FIRED_INSTEAD(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("when"), rb_str_new_cstr("INSTEAD OF"));
	else
		elog(ERROR, "unknown firing time for trigger function");

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("level"), rb_str_new_cstr("ROW"));
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		rb_hash_aset(td, rb_str_new_cstr("level"), rb_str_new_cstr("STATEMENT"));
	else
		elog(ERROR, "unknown firing level for trigger function");

	return td;
}

static Datum
plruby_trigger_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	VALUE		td;
	VALUE		save_td;
	VALUE		result;
	Datum		retval = (Datum) 0;

	td = plruby_trig_build_args(fcinfo);

	/* Expose $_TD to the body; save/restore for nested-trigger re-entrancy */
	save_td = rb_gv_get("$_TD");
	rb_gv_set("$_TD", td);

	result = plruby_protected_call(plruby_recv, rb_intern(desc->proname), 0, NULL);

	/* The body may have reassigned $_TD or mutated $_TD['new'] */
	td = rb_gv_get("$_TD");
	rb_gv_set("$_TD", save_td);

	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	if ((!TRIGGER_FIRED_BEFORE(trigdata->tg_event) &&
		 !TRIGGER_FIRED_INSTEAD(trigdata->tg_event)) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		return (Datum) 0;		/* AFTER, or statement-level (incl. TRUNCATE):
								 * the return value is ignored */

	if (RB_TYPE_P(result, T_STRING))
	{
		char	   *srv = pnstrdup(RSTRING_PTR(result), RSTRING_LEN(result));

		if (pg_strcasecmp(srv, "SKIP") == 0)
			retval = (Datum) 0;
		else if (pg_strcasecmp(srv, "MODIFY") == 0)
		{
			if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
				TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				retval = PointerGetDatum(plruby_modify_tuple(td, trigdata));
			else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot modify the return tuple of an on-delete trigger")));
			else
				elog(ERROR, "unknown event in trigger function");
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("expected trigger function to return nil, 'SKIP' or 'MODIFY'")));
		pfree(srv);
	}
	else if (NIL_P(result))
	{
		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
			TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
			retval = PointerGetDatum(trigdata->tg_trigtuple);
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			retval = PointerGetDatum(trigdata->tg_newtuple);
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("expected trigger function to return nil, 'SKIP' or 'MODIFY'")));

	return retval;
}

static Datum
plruby_event_trigger_handler(FunctionCallInfo fcinfo, plruby_proc_desc *desc)
{
	EventTriggerData *tdata = (EventTriggerData *) fcinfo->context;
	VALUE		td = rb_hash_new();
	VALUE		save_td;

	rb_hash_aset(td, rb_str_new_cstr("event"), rb_str_new_cstr(tdata->event));
#if PG_VERSION_NUM >= 130000
	rb_hash_aset(td, rb_str_new_cstr("tag"),
				 rb_str_new_cstr(GetCommandTagName(tdata->tag)));
#else
	rb_hash_aset(td, rb_str_new_cstr("tag"), rb_str_new_cstr(tdata->tag));
#endif

	save_td = rb_gv_get("$_TD");
	rb_gv_set("$_TD", td);

	plruby_protected_call(plruby_recv, rb_intern(desc->proname), 0, NULL);

	rb_gv_set("$_TD", save_td);

	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	return (Datum) 0;
}

/* ---------------------------------------------------------------------
 * Argument array construction
 * ------------------------------------------------------------------- */

static VALUE
plruby_func_build_args(plruby_proc_desc *desc, FunctionCallInfo fcinfo)
{
	VALUE		args = rb_ary_new();
	int			i,
				j;

	for (i = 0, j = 0; i < desc->n_total_args;
		 (j = IS_ARGMODE_OUT(desc->arg_argmode[i]) ? j : j + 1), i++)
	{
		if (IS_ARGMODE_OUT(desc->arg_argmode[i]))
		{
			rb_ary_push(args, Qnil);
			continue;
		}

		/* A TRANSFORM FOR TYPE argument converts through its FromSQL function */
		if (OidIsValid(desc->arg_transform[i]))
		{
			if (PLRUBY_ARG_ISNULL(fcinfo, j))
				rb_ary_push(args, Qnil);
			else
				rb_ary_push(args,
							(VALUE) OidFunctionCall1(desc->arg_transform[i],
													 PLRUBY_ARG_VALUE(fcinfo, j)));
			continue;
		}

		if (desc->arg_typtype[i] == TYPTYPE_PSEUDO)
		{
			HeapTuple	typeTup;
			Form_pg_type typeStruct;

			typeTup = SearchSysCache1(TYPEOID,
									  ObjectIdGetDatum(get_fn_expr_argtype(fcinfo->flinfo, j)));
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
			fmgr_info_cxt(typeStruct->typoutput, &(desc->arg_out_func[i]),
						  TopMemoryContext);
			ReleaseSysCache(typeTup);
		}

		if (desc->arg_typtype[i] == TYPTYPE_COMPOSITE)
		{
			if (PLRUBY_ARG_ISNULL(fcinfo, j))
				rb_ary_push(args, Qnil);
			else
			{
				HeapTupleHeader td;
				Oid			tupType;
				int32		tupTypmod;
				TupleDesc	tupdesc;
				HeapTupleData tmptup;

				td = DatumGetHeapTupleHeader(PLRUBY_ARG_VALUE(fcinfo, j));
				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = td;
				tupType = HeapTupleHeaderGetTypeId(td);
				tupTypmod = HeapTupleHeaderGetTypMod(td);
				tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

				rb_ary_push(args, plruby_hash_from_tuple(&tmptup, tupdesc));
				ReleaseTupleDesc(tupdesc);
			}
		}
		else
		{
			if (PLRUBY_ARG_ISNULL(fcinfo, j))
				rb_ary_push(args, Qnil);
			else
			{
				char	   *tmp = OutputFunctionCall(&(desc->arg_out_func[i]),
													PLRUBY_ARG_VALUE(fcinfo, j));
				Oid			elemtype = get_element_type(desc->arg_type[i]);

				if (OidIsValid(elemtype))
					rb_ary_push(args, plruby_array_from_pg(tmp, elemtype));
				else
					rb_ary_push(args,
								plruby_scalar_from_cstring(tmp, desc->arg_type[i]));
				pfree(tmp);
			}
		}
	}

	return args;
}

/* ---------------------------------------------------------------------
 * Function compilation
 * ------------------------------------------------------------------- */

static bool
is_valid_ruby_identifier(const char *name)
{
	int			i;

	if (name == NULL || name[0] == '\0')
		return false;
	/* local variables start with a lowercase letter or underscore */
	if (!(islower((unsigned char) name[0]) || name[0] == '_'))
		return false;
	for (i = 1; name[i] != '\0'; i++)
		if (!isalnum((unsigned char) name[i]) && name[i] != '_')
			return false;
	return true;
}

static plruby_proc_desc *
plruby_compile_function(Oid fnoid, bool is_trigger, bool is_event_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	plruby_proc_desc *prodesc = NULL;
	VALUE		cached;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *proc_source;

	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fnoid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fnoid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	if (is_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "plruby_proc_%u_trigger", fnoid);
	else if (is_event_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "plruby_proc_%u_evttrigger", fnoid);
	else
		snprintf(internal_proname, sizeof(internal_proname),
				 "plruby_proc_%u", fnoid);

	/* Look up a cached descriptor */
	cached = rb_hash_aref(plruby_proc_cache, rb_str_new_cstr(internal_proname));
	if (RB_TYPE_P(cached, T_STRING))
	{
		sscanf(RSTRING_PTR(cached), "%p", (void **) &prodesc);
		if (prodesc != NULL)
		{
			bool		uptodate =
				(prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
				 prodesc->fn_cmin == HeapTupleHeaderGetRawCommandId(procTup->t_data));

			if (!uptodate)
			{
				free(prodesc->proname);
				free(prodesc);
				prodesc = NULL;
			}
		}
	}

	if (prodesc == NULL)
	{
		HeapTuple	langTup;
		Form_pg_language langStruct;
		char	  **argnames = NULL;
		char	   *argmodes = NULL;
		Oid		   *argtypes = NULL;
		StringInfoData src;
		StringInfoData aliases;
		StringInfoData outvars;
		int			n_out_total;
		int			i;
		char		ptrbuf[64];

		prodesc = (plruby_proc_desc *) malloc(sizeof(plruby_proc_desc));
		if (!prodesc)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
		MemSet(prodesc, 0, sizeof(plruby_proc_desc));
		prodesc->proname = strdup(internal_proname);

		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_cmin = HeapTupleHeaderGetRawCommandId(procTup->t_data);

		/* language (unused beyond validation, but fetch to mirror plphp) */
		langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(procStruct->prolang));
		if (!HeapTupleIsValid(langTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "cache lookup failed for language %u", procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		(void) langStruct;
		ReleaseSysCache(langTup);

		/* Return-type information (regular functions only) */
		if (!is_trigger && !is_event_trigger)
		{
			int16		typlen;
			bool		typbyval;
			char		typalign,
						typdelim,
						typtype;
			Oid			typioparam,
						typinput;

			typtype = get_typtype(procStruct->prorettype);
			get_type_io_data(procStruct->prorettype, IOFunc_input,
							 &typlen, &typbyval, &typalign, &typdelim,
							 &typioparam, &typinput);

			if (typtype == TYPTYPE_PSEUDO)
			{
				if (procStruct->prorettype == VOIDOID ||
					procStruct->prorettype == RECORDOID ||
					procStruct->prorettype == ANYELEMENTOID ||
					procStruct->prorettype == ANYARRAYOID)
					prodesc->ret_type |= PL_PSEUDO;
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions may only be called as triggers")));
				}
				else
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plruby functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}

			prodesc->ret_oid = procStruct->prorettype;
			prodesc->retset = procStruct->proretset;

			if (typtype == TYPTYPE_COMPOSITE || procStruct->prorettype == RECORDOID)
				prodesc->ret_type |= PL_TUPLE;

			if (procStruct->prorettype == ANYARRAYOID)
				prodesc->ret_type |= PL_ARRAY;
			else if (typlen == -1 && get_element_type(procStruct->prorettype))
				prodesc->ret_type |= PL_ARRAY;

			fmgr_info_cxt(typinput, &(prodesc->result_in_func), TopMemoryContext);
			prodesc->result_typioparam = typioparam;
		}

		/* Argument info */
		prodesc->n_total_args = get_func_arg_info(procTup, &argtypes,
												  &argnames, &argmodes);
		prodesc->n_out_args = 0;
		prodesc->n_mixed_args = 0;

		for (i = 0; i < prodesc->n_total_args; i++)
		{
			char		mode = argmodes ? argmodes[i] : PROARGMODE_IN;

			switch (mode)
			{
				case PROARGMODE_OUT:
					prodesc->n_out_args++;
					break;
				case PROARGMODE_INOUT:
					prodesc->n_mixed_args++;
					break;
				case PROARGMODE_IN:
				case PROARGMODE_TABLE:
					break;
				case PROARGMODE_VARIADIC:
					/*
					 * The caller folds a non-"any" variadic tail into a
					 * single array argument, so it behaves exactly like an
					 * IN argument of the array type.  VARIADIC "any" would
					 * need per-call argument type discovery; keep rejecting
					 * it.
					 */
					if (argtypes && argtypes[i] == ANYOID)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("VARIADIC \"any\" arguments are not supported")));
					mode = PROARGMODE_IN;
					break;
				default:
					elog(ERROR, "unsupported argument mode %c", mode);
			}
			prodesc->arg_argmode[i] = mode;

			/*
			 * Resolve domains to their base type so a domain argument gets
			 * the base type's Ruby mapping (a domain over int arrives as an
			 * Integer, over int[] as an Array, ...).  The domain's checks
			 * were already applied when the value was created.
			 */
			prodesc->arg_type[i] = argtypes ? getBaseType(argtypes[i]) : InvalidOid;
			prodesc->arg_typtype[i] = argtypes ?
				get_typtype(prodesc->arg_type[i]) : TYPTYPE_BASE;

			if (argtypes && prodesc->arg_typtype[i] != TYPTYPE_COMPOSITE)
			{
				Oid			typoutput;
				bool		typisvarlena;

				getTypeOutputInfo(prodesc->arg_type[i], &typoutput, &typisvarlena);
				fmgr_info_cxt(typoutput, &(prodesc->arg_out_func[i]),
							  TopMemoryContext);
			}

			prodesc->arg_transform[i] = InvalidOid;
		}

		/*
		 * TRANSFORM FOR TYPE list: for each transformed type, look up the
		 * FromSQL function for matching arguments and the ToSQL function for
		 * the return type.
		 */
		prodesc->ret_transform = InvalidOid;
		{
			Datum		protrftypes;

			protrftypes = SysCacheGetAttr(PROCOID, procTup,
										  Anum_pg_proc_protrftypes, &isnull);
			if (!isnull)
			{
				ArrayType  *arr = DatumGetArrayTypeP(protrftypes);
				Datum	   *elems;
				int			nelems;
				List	   *trftypes = NIL;

				deconstruct_array(arr, OIDOID, sizeof(Oid), true, 'i',
								  &elems, NULL, &nelems);
				for (i = 0; i < nelems; i++)
					trftypes = lappend_oid(trftypes,
										   DatumGetObjectId(elems[i]));

				prodesc->ret_transform =
					get_transform_tosql(procStruct->prorettype,
										procStruct->prolang, trftypes);
				for (i = 0; i < prodesc->n_total_args; i++)
					if (!IS_ARGMODE_OUT(prodesc->arg_argmode[i]) &&
						OidIsValid(prodesc->arg_type[i]))
						prodesc->arg_transform[i] =
							get_transform_fromsql(prodesc->arg_type[i],
												  procStruct->prolang,
												  trftypes);
				list_free(trftypes);
			}
		}

		n_out_total = prodesc->n_out_args + prodesc->n_mixed_args;

		/* Function source */
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "cache lookup yielded NULL prosrc");
		proc_source = TextDatumGetCString(prosrcdatum);

		initStringInfo(&src);
		initStringInfo(&aliases);
		initStringInfo(&outvars);

		if (is_trigger)
			appendStringInfo(&src, "def %s()\n%s\nend\n",
							 internal_proname, proc_source);
		else if (is_event_trigger)
			appendStringInfo(&src, "def %s()\n%s\nend\n",
							 internal_proname, proc_source);
		else
		{
			bool		first_out = true;
			StringInfoData tablecols;

			initStringInfo(&tablecols);

			/*
			 * Walk the arguments once, building three pieces of source:
			 *  - aliases: "name = args[i]" for IN/INOUT arguments;
			 *  - outvars: the comma list of OUT/INOUT names to return (non-SRF);
			 *  - tablecols: "name = nil" predeclarations for OUT/TABLE columns
			 *    of a set-returning function, so a no-argument return_next can
			 *    read them from the body's binding.
			 */
			for (i = 0; i < prodesc->n_total_args; i++)
			{
				char		mode = prodesc->arg_argmode[i];
				bool		valid = (argnames && argnames[i] &&
									 is_valid_ruby_identifier(argnames[i]));

				if ((mode == PROARGMODE_IN || mode == PROARGMODE_INOUT) && valid)
					appendStringInfo(&aliases, "%s = args[%d]\n", argnames[i], i);

				/*
				 * Pre-declare pure OUT locals in the method scope (before the
				 * lambda) so the body's assignments mutate the captured outer
				 * locals rather than creating lambda-scoped ones.
				 */
				if (!prodesc->retset && mode == PROARGMODE_OUT && valid)
					appendStringInfo(&aliases, "%s = nil\n", argnames[i]);

				if (!prodesc->retset &&
					(mode == PROARGMODE_OUT || mode == PROARGMODE_INOUT) && valid)
				{
					if (!first_out)
						appendStringInfoString(&outvars, ", ");
					appendStringInfoString(&outvars, argnames[i]);
					first_out = false;
				}

				if (prodesc->retset &&
					(mode == PROARGMODE_OUT || mode == PROARGMODE_TABLE) && valid)
					appendStringInfo(&tablecols, "%s = nil\n", argnames[i]);
			}

			appendStringInfo(&src, "def %s(args, argc)\n", internal_proname);
			appendStringInfoString(&src, aliases.data);

			if (prodesc->retset)
			{
				appendStringInfoString(&src, tablecols.data);
				appendStringInfoString(&src, "PLRuby.__set_binding(binding)\n");
				appendStringInfoString(&src, "lambda {\n");
				appendStringInfoString(&src, proc_source);
				appendStringInfoString(&src, "\n}.call\n");
				appendStringInfoString(&src, "nil\n");
			}
			else if (n_out_total >= 1)
			{
				appendStringInfoString(&src, "lambda {\n");
				appendStringInfoString(&src, proc_source);
				appendStringInfoString(&src, "\n}.call\n");
				if (n_out_total == 1)
					appendStringInfo(&src, "return %s\n", outvars.data);
				else
					appendStringInfo(&src, "return [%s]\n", outvars.data);
			}
			else
			{
				appendStringInfoString(&src, proc_source);
				appendStringInfoChar(&src, '\n');
			}
			appendStringInfoString(&src, "end\n");
			pfree(tablecols.data);
		}

		/* Register the descriptor in the cache before compiling */
		snprintf(ptrbuf, sizeof(ptrbuf), "%p", (void *) prodesc);
		rb_hash_aset(plruby_proc_cache,
					 rb_str_new_cstr(internal_proname),
					 rb_str_new_cstr(ptrbuf));

		{
			int			state = 0;

			rb_eval_string_protect(src.data, &state);
			if (state)
			{
				char	   *msg = plruby_pop_exception_message();

				prodesc->fn_xmin = InvalidTransactionId;	/* force recompile */
				if (msg != NULL)
					elog(ERROR, "unable to compile function \"%s\": %s",
						 prodesc->proname, msg);
				else
					elog(ERROR, "unable to compile function \"%s\"",
						 prodesc->proname);
			}
		}

		pfree(src.data);
		pfree(aliases.data);
		pfree(outvars.data);
		pfree(proc_source);
	}

	ReleaseSysCache(procTup);
	return prodesc;
}

/* ---------------------------------------------------------------------
 * Validator
 * ------------------------------------------------------------------- */

Datum
plruby_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	procTup;
	Form_pg_proc procForm;
	char		funcname[NAMEDATALEN];
	char		tmpname[64];
	char	   *prosrc;
	Datum		prosrcdatum;
	bool		isnull;
	StringInfoData src;
	int			state = 0;

	plruby_init_all();

	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procForm = (Form_pg_proc) GETSTRUCT(procTup);

	prosrcdatum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "cache lookup yielded NULL prosrc");
	prosrc = TextDatumGetCString(prosrcdatum);
	strlcpy(funcname, NameStr(procForm->proname), NAMEDATALEN);
	ReleaseSysCache(procTup);

	snprintf(tmpname, sizeof(tmpname), "plruby_temp_%u", funcoid);
	initStringInfo(&src);
	appendStringInfo(&src, "def %s(args, argc)\n%s\nend\n", tmpname, prosrc);
	pfree(prosrc);

	rb_eval_string_protect(src.data, &state);
	pfree(src.data);

	if (state)
	{
		char	   *msg = plruby_pop_exception_message();

		if (msg != NULL)
			elog(ERROR, "function \"%s\" does not validate: %s", funcname, msg);
		else
			elog(ERROR, "function \"%s\" does not validate", funcname);
	}

	plruby_remove_method(tmpname);

	PG_RETURN_VOID();
}

/* ---------------------------------------------------------------------
 * Inline handler (DO blocks)
 * ------------------------------------------------------------------- */

Datum
plruby_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock =
		(InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	static long inline_counter = 0;
	char		funcname[64];
	StringInfoData src;
	int			state = 0;
	VALUE		params[2];

	plruby_init_all();

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to SPI manager")));

	current_fcinfo = NULL;
	plruby_session_init();

	snprintf(funcname, sizeof(funcname), "plruby_inline_%ld", ++inline_counter);

	initStringInfo(&src);
	appendStringInfo(&src, "def %s(args, argc)\n%s\nend\n",
					 funcname, codeblock->source_text);

	rb_eval_string_protect(src.data, &state);
	pfree(src.data);
	if (state)
	{
		char	   *msg = plruby_pop_exception_message();

		if (msg != NULL)
			elog(ERROR, "unable to compile inline code block: %s", msg);
		else
			elog(ERROR, "unable to compile inline code block");
	}

	params[0] = rb_ary_new();
	params[1] = INT2NUM(0);
	plruby_protected_call(plruby_recv, rb_intern(funcname), 2, params);

	plruby_remove_method(funcname);

	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	PG_RETURN_VOID();
}
