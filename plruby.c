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
	 * Ruby VALUE as a Datum; a ToSQL function accepts one.  The full
	 * resolved list (transforms/n_transforms, malloc'd) also backs nested
	 * conversions: composite fields, array elements, and SRF rows.
	 */
	Oid			arg_transform[FUNC_MAX_ARGS];
	Oid			ret_transform;
	plruby_transform_info *transforms;
	int			n_transforms;
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
static char *plruby_on_init = NULL;
static bool plruby_session_inited = false;

/*
 * Per-function static storage.  Maps a function's internal proc name (see
 * plruby_compile_function) to a Hash that is exposed to the body as $_SD and
 * persists across calls to that function within the session.  Counterpart of
 * PL/Python's SD; $_SHARED is the session-global GD.  Entries are dropped when
 * a function is recompiled, so $_SD resets on CREATE OR REPLACE.
 */
static VALUE plruby_sd_table;

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
	char		buf[192];
	int			state = 0;

	snprintf(buf, sizeof(buf),
			 "PLRuby::PROCS.delete('%s'); "
			 "Object.send(:remove_method, :%s) rescue nil", name, name);
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
	DefineCustomStringVariable("plruby.on_init",
							   "Ruby source to evaluate when the interpreter "
							   "is first initialized in a session.",
							   "Runs before plruby_modules and plruby.start_proc, "
							   "at the top level.  The counterpart of "
							   "plperl.on_init.",
							   &plruby_on_init,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);

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
		 * ruby_run_node).
		 *
		 * RubyGems stays enabled: on Ruby 3.4+ much of the traditional
		 * standard library (csv, bigdecimal, base64, ...) ships as bundled
		 * gems that plain require cannot see without it.  did_you_mean and
		 * error_highlight are disabled so error messages stay deterministic
		 * (no suggestion lines appended to NameError/NoMethodError).
		 */
		static char *ruby_argv[] = {
			(char *) "plruby",
			(char *) "--disable=did_you_mean,error_highlight",
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

	plruby_eval_string("module PLRuby; PROCS = {};"
					   " class Error < StandardError;"
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

	plruby_sd_table = rb_hash_new();
	rb_gc_register_address(&plruby_sd_table);
	rb_gv_set("$_SD", rb_hash_new());	/* placeholder until a body binds one */

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

	/*
	 * plruby.on_init runs first: arbitrary top-level Ruby (requires, helper
	 * definitions, ...) evaluated once, before modules and start_proc.  The
	 * counterpart of plperl.on_init.
	 */
	if (plruby_on_init != NULL && plruby_on_init[0] != '\0')
	{
		int			state = 0;

		rb_eval_string_protect(plruby_on_init, &state);
		if (state)
		{
			char	   *msg = plruby_pop_exception_message();

			if (msg != NULL)
				elog(ERROR, "plruby.on_init failed: %s", msg);
			else
				elog(ERROR, "plruby.on_init failed");
		}
	}

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
 * Error context: name the running code in the server's CONTEXT line, like
 * every other procedural language.
 * ------------------------------------------------------------------- */

typedef struct plruby_error_ctx
{
	const char *proname;		/* real function name, or NULL */
	bool		inline_block;	/* true for a DO block */
} plruby_error_ctx;

static void
plruby_error_callback(void *arg)
{
	plruby_error_ctx *ctx = (plruby_error_ctx *) arg;

	if (ctx->inline_block)
		errcontext("PL/Ruby anonymous code block");
	else if (ctx->proname != NULL)
		errcontext("PL/Ruby function \"%s\"", ctx->proname);
}

/* ---------------------------------------------------------------------
 * Call handler
 * ------------------------------------------------------------------- */

/*
 * Bind $_SD to this function's private, session-persistent Hash and return the
 * previous $_SD so the caller can restore it (needed when a body re-enters
 * PL/Ruby via SPI).  The hash is keyed by the internal proc name, so it is
 * distinct for every function -- and for a function's trigger vs plain form --
 * and is dropped when the function is recompiled.
 */
static VALUE
plruby_bind_sd(plruby_proc_desc *desc)
{
	VALUE		prev = rb_gv_get("$_SD");
	VALUE		key = rb_str_new_cstr(desc->proname);
	VALUE		sd = rb_hash_aref(plruby_sd_table, key);

	if (NIL_P(sd))
	{
		sd = rb_hash_new();
		rb_hash_aset(plruby_sd_table, key, sd);
	}
	rb_gv_set("$_SD", sd);
	return prev;
}

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

	{
		/*
		 * Scope the function's TRANSFORM FOR TYPE list to this call,
		 * saving/restoring for nested calls -- including when a nested
		 * call's error is caught and rescued by the outer body, hence the
		 * PG_CATCH.  Triggers and event triggers do not use transforms.
		 */
		plruby_transform_info *save_call_trf = plruby_call_transforms;
		int			save_call_ntrf = plruby_call_ntransforms;
		plruby_transform_info *save_arg_trf = plruby_arg_transforms;
		int			save_arg_ntrf = plruby_arg_ntransforms;
		VALUE		save_sd = rb_gv_get("$_SD");
		ErrorContextCallback ecb;
		plruby_error_ctx errctx;

		/*
		 * Add a "CONTEXT: PL/Ruby function ..." line to any error raised while
		 * this function compiles or runs.  The name is resolved now (cheap
		 * syscache lookup) so it is stable across nested calls.
		 */
		errctx.proname = get_func_name(fcinfo->flinfo->fn_oid);
		errctx.inline_block = false;
		ecb.callback = plruby_error_callback;
		ecb.arg = (void *) &errctx;
		ecb.previous = error_context_stack;
		error_context_stack = &ecb;

		PG_TRY();
		{
			if (CALLED_AS_TRIGGER(fcinfo))
			{
				/*
				 * A trigger function that declares TRANSFORM FOR TYPE gets
				 * transformed $_TD rows and 'MODIFY' conversion; without the
				 * clause the list is empty and nothing changes.
				 */
				desc = plruby_compile_function(fcinfo->flinfo->fn_oid, true, false);
				plruby_call_transforms = desc->transforms;
				plruby_call_ntransforms = desc->n_transforms;
				plruby_bind_sd(desc);
				retval = plruby_trigger_handler(fcinfo, desc);
			}
			else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
			{
				desc = plruby_compile_function(fcinfo->flinfo->fn_oid, false, true);
				plruby_call_transforms = NULL;
				plruby_call_ntransforms = 0;
				plruby_bind_sd(desc);
				retval = plruby_event_trigger_handler(fcinfo, desc);
			}
			else
			{
				desc = plruby_compile_function(fcinfo->flinfo->fn_oid, false, false);
				plruby_call_transforms = desc->transforms;
				plruby_call_ntransforms = desc->n_transforms;
				plruby_bind_sd(desc);
				if (desc->retset)
					retval = plruby_srf_handler(fcinfo, desc);
				else
					retval = plruby_func_handler(fcinfo, desc);
			}
		}
		PG_CATCH();
		{
			error_context_stack = ecb.previous;
			plruby_call_transforms = save_call_trf;
			plruby_call_ntransforms = save_call_ntrf;
			plruby_arg_transforms = save_arg_trf;
			plruby_arg_ntransforms = save_arg_ntrf;
			rb_gv_set("$_SD", save_sd);
			PG_RE_THROW();
		}
		PG_END_TRY();

		error_context_stack = ecb.previous;
		plruby_call_transforms = save_call_trf;
		plruby_call_ntransforms = save_call_ntrf;
		plruby_arg_transforms = save_arg_trf;
		plruby_arg_ntransforms = save_arg_ntrf;
		rb_gv_set("$_SD", save_sd);
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
				 * Array of composite (or of a transformed type, e.g.
				 * jsonb[]): build the array datum directly so each element
				 * is assembled recursively/through its ToSQL function.
				 * Scalar arrays keep the (multidimensional-capable) text
				 * path below.
				 */
				if (OidIsValid(elemtype) &&
					(type_is_rowtype(elemtype) ||
					 OidIsValid(plruby_transform_tosql(elemtype))))
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

	/*
	 * FROM-SQL transforms apply only while $_TD is built (so SPI reads in
	 * the body are unaffected); the call handler's PG_CATCH restores this
	 * on error.
	 */
	plruby_arg_transforms = desc->transforms;
	plruby_arg_ntransforms = desc->n_transforms;
	td = plruby_trig_build_args(fcinfo);
	plruby_arg_transforms = NULL;
	plruby_arg_ntransforms = 0;

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

	/*
	 * FROM-SQL transforms apply only while the argument array is built (so
	 * SPI/cursor/trigger row reads are unaffected); the call handler's
	 * PG_CATCH restores this on error.
	 */
	plruby_arg_transforms = desc->transforms;
	plruby_arg_ntransforms = desc->n_transforms;

	for (i = 0, j = 0; i < desc->n_total_args;
		 (j = IS_ARGMODE_OUT(desc->arg_argmode[i]) ? j : j + 1), i++)
	{
		/*
		 * The type used to shape the Ruby value.  For a polymorphic pseudo
		 * argument (anyelement/anyarray/anycompatible/anycompatiblearray) it is
		 * the concrete type resolved at call time, so the value arrives as its
		 * native Ruby type (an Array for array types) rather than as text.
		 */
		Oid			argtype = desc->arg_type[i];

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

			/* Resolve the polymorphic type to its concrete call-time type. */
			argtype = get_fn_expr_argtype(fcinfo->flinfo, j);
			typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(argtype));
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
				Oid			elemtype = get_element_type(argtype);
				Oid			etrf = plruby_transform_fromsql(elemtype);

				if (OidIsValid(etrf))
				{
					/* array of a transformed type: per-element FromSQL */
					rb_ary_push(args,
								plruby_array_from_datum(PLRUBY_ARG_VALUE(fcinfo, j),
														elemtype, etrf));
				}
				else
				{
					char	   *tmp = OutputFunctionCall(&(desc->arg_out_func[i]),
														 PLRUBY_ARG_VALUE(fcinfo, j));

					if (OidIsValid(elemtype))
						rb_ary_push(args, plruby_array_from_pg(tmp, elemtype));
					else
						rb_ary_push(args,
									plruby_scalar_from_cstring(tmp, argtype));
					pfree(tmp);
				}
			}
		}
	}

	plruby_arg_transforms = NULL;
	plruby_arg_ntransforms = 0;

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
				free(prodesc->transforms);
				free(prodesc);
				prodesc = NULL;

				/*
				 * Drop the stale cache entry now that its descriptor is freed.
				 * The rebuild below re-adds it on success; if the rebuild
				 * instead errors out (an unsupported return type, a compile
				 * failure, ...), the cache no longer points at freed memory,
				 * so the next call recompiles cleanly instead of dereferencing
				 * a dangling pointer.
				 */
				rb_hash_delete(plruby_proc_cache,
							   rb_str_new_cstr(internal_proname));

				/* Reset $_SD too, so a recompile starts with empty storage. */
				rb_hash_delete(plruby_sd_table,
							   rb_str_new_cstr(internal_proname));
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
					procStruct->prorettype == ANYARRAYOID
#if PG_VERSION_NUM >= 130000
					|| procStruct->prorettype == ANYCOMPATIBLEOID
					|| procStruct->prorettype == ANYCOMPATIBLEARRAYOID
#endif
					)
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

			if (procStruct->prorettype == ANYARRAYOID
#if PG_VERSION_NUM >= 130000
				|| procStruct->prorettype == ANYCOMPATIBLEARRAYOID
#endif
				)
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
		prodesc->transforms = NULL;
		prodesc->n_transforms = 0;
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

				/* The full list, for nested conversions during the call. */
				prodesc->transforms = (plruby_transform_info *)
					malloc(nelems * sizeof(plruby_transform_info));
				if (prodesc->transforms == NULL)
					elog(ERROR, "out of memory");
				for (i = 0; i < nelems; i++)
				{
					Oid			typid = DatumGetObjectId(elems[i]);

					prodesc->transforms[i].typid = typid;
					prodesc->transforms[i].fromsql =
						get_transform_fromsql(typid, procStruct->prolang,
											  trftypes);
					prodesc->transforms[i].tosql =
						get_transform_tosql(typid, procStruct->prolang,
											trftypes);
				}
				prodesc->n_transforms = nelems;
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

		/*
		 * Every variant stores the user body as a TOP-LEVEL lambda in
		 * PLRuby::PROCS and defines the internal method as a pure delegator.
		 * `class`/`module` keywords are a SyntaxError anywhere lexically
		 * inside a `def` (blocks included), but are legal in a block at the
		 * top level -- definitions land globally, like plruby_modules.  An
		 * explicit `return` exits the lambda with the value the delegator
		 * method then returns.
		 */
		if (is_trigger)
			appendStringInfo(&src,
							 "PLRuby::PROCS['%s'] = lambda {\n%s\n}\n"
							 "def %s()\nPLRuby::PROCS['%s'].call\nend\n",
							 internal_proname, proc_source,
							 internal_proname, internal_proname);
		else if (is_event_trigger)
			appendStringInfo(&src,
							 "PLRuby::PROCS['%s'] = lambda {\n%s\n}\n"
							 "def %s()\nPLRuby::PROCS['%s'].call\nend\n",
							 internal_proname, proc_source,
							 internal_proname, internal_proname);
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

			appendStringInfo(&src, "PLRuby::PROCS['%s'] = lambda { |args, argc|\n",
							 internal_proname);
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
			appendStringInfoString(&src, "}\n");
			appendStringInfo(&src,
							 "def %s(args, argc)\nPLRuby::PROCS['%s'].call(args, argc)\nend\n",
							 internal_proname, internal_proname);
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
	/* Same top-level-lambda shape as the real compilation, so `class`/
	 * `module` in the body validate (they are illegal inside a def). */
	appendStringInfo(&src, "PLRuby::PROCS['%s'] = lambda { |args, argc|\n%s\n}\n",
					 tmpname, prosrc);
	pfree(prosrc);

	rb_eval_string_protect(src.data, &state);
	pfree(src.data);

	if (state)
	{
		char	   *msg = plruby_pop_exception_message();

		if (msg != NULL)
		{
			/*
			 * Ruby 3.4's parser (Prism) reports syntax errors over several
			 * lines, quoting the generated wrapper source, including the
			 * OID-derived internal method name.  Keep the first line only.
			 */
			char	   *nl = strchr(msg, '\n');

			if (nl != NULL)
				*nl = '\0';
			elog(ERROR, "function \"%s\" does not validate: %s", funcname, msg);
		}
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
	VALUE		save_sd;
	ErrorContextCallback ecb;
	plruby_error_ctx errctx;

	plruby_init_all();

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to SPI manager")));

	current_fcinfo = NULL;
	plruby_session_init();

	/* "CONTEXT: PL/Ruby anonymous code block" on any error below. */
	errctx.proname = NULL;
	errctx.inline_block = true;
	ecb.callback = plruby_error_callback;
	ecb.arg = (void *) &errctx;
	ecb.previous = error_context_stack;
	error_context_stack = &ecb;

	snprintf(funcname, sizeof(funcname), "plruby_inline_%ld", ++inline_counter);

	initStringInfo(&src);
	appendStringInfo(&src,
					 "PLRuby::PROCS['%s'] = lambda { |args, argc|\n%s\n}\n"
					 "def %s(args, argc)\nPLRuby::PROCS['%s'].call(args, argc)\nend\n",
					 funcname, codeblock->source_text,
					 funcname, funcname);

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

	/*
	 * An anonymous block has no persistent identity, so $_SD is a fresh empty
	 * Hash for the duration of the block (matching PL/Python, where a DO block
	 * gets its own SD that does not survive).  $_SHARED still persists.
	 */
	save_sd = rb_gv_get("$_SD");
	rb_gv_set("$_SD", rb_hash_new());
	plruby_protected_call(plruby_recv, rb_intern(funcname), 2, params);
	rb_gv_set("$_SD", save_sd);

	plruby_remove_method(funcname);

	error_context_stack = ecb.previous;

	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	PG_RETURN_VOID();
}
