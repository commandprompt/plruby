/**********************************************************************
 * plruby_spi.c - the database API PL/Ruby exposes to Ruby code, plus
 * interpreter-level setup ($_SHARED, output redirection).
 *
 * This file grows feature by feature; the SPI query functions, prepared
 * statements, transaction control and subtransactions are added on top of
 * the setup and messaging helpers established here.
 *
 * Copyright (c) 2026 ChronicallyJD.  MIT License; see LICENSE.
 **********************************************************************/

#include "postgres.h"
#include "plruby.h"
#include "plruby_spi.h"
#include "plruby_io.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

/* SRF support (shared with the SRF handler in plruby.c) */
FunctionCallInfo current_fcinfo = NULL;
TupleDesc	current_tupledesc = NULL;
AttInMetadata *current_attinmeta = NULL;
MemoryContext current_memcxt = NULL;
Tuplestorestate *current_tuplestore = NULL;
VALUE		current_srf_binding = Qnil;

/*
 * Raise PLRuby::Error for a PostgreSQL error captured via CopyErrorData(),
 * attaching the five-character SQLSTATE as @sqlstate.  Does not return.
 */
static void
plruby_raise_pg_error(ErrorData *edata)
{
	VALUE		exc;

	exc = rb_exc_new_str(rb_ePLRubyError,
						 plruby_str_from_pg(edata->message,
											strlen(edata->message)));
	rb_ivar_set(exc, rb_intern("@sqlstate"),
				rb_str_new_cstr(unpack_sql_state(edata->sqlerrcode)));
	FreeErrorData(edata);
	rb_exc_raise(exc);
}

/* ---------------------------------------------------------------------
 * Output redirection: Ruby's $stdout/$stderr forwarded to the PG log.
 * ------------------------------------------------------------------- */

static StringInfo plruby_msgbuf = NULL;

static VALUE
plruby_do_log(VALUE self, VALUE str)
{
	const char *p;
	long		len;

	str = rb_obj_as_string(str);
	p = RSTRING_PTR(str);
	len = RSTRING_LEN(str);

	if (plruby_msgbuf == NULL)
		plruby_msgbuf = makeStringInfo();

	appendBinaryStringInfo(plruby_msgbuf, p, len);

	/* Flush once a complete line has accumulated (matches PL/php). */
	if (plruby_msgbuf->len > 0 &&
		plruby_msgbuf->data[plruby_msgbuf->len - 1] == '\n')
	{
		plruby_msgbuf->data[plruby_msgbuf->len - 1] = '\0';
		elog(LOG, "%s", plruby_msgbuf->data);
		resetStringInfo(plruby_msgbuf);
	}

	return LONG2NUM(len);
}

static const char *plruby_output_ruby =
"module PLRuby\n"
"  class Output\n"
"    def write(*a); n = 0; a.each { |x| s = x.to_s; PLRuby.__log(s); n += s.bytesize }; n; end\n"
"    def print(*a); a.each { |x| write(x.to_s) }; nil; end\n"
"    def puts(*a)\n"
"      if a.empty? then write(\"\\n\")\n"
"      else a.each { |x|\n"
"        if x.is_a?(Array) then puts(*x)\n"
"        else s = x.to_s; write(s); write(\"\\n\") unless s.end_with?(\"\\n\") end }\n"
"      end; nil\n"
"    end\n"
"    def printf(f, *a); write(sprintf(f, *a)); nil; end\n"
"    def <<(x); write(x.to_s); self; end\n"
"    def flush; self; end\n"
"    def sync; true; end\n"
"    def sync=(v); v; end\n"
"    def fileno; -1; end\n"
"    def tty?; false; end\n"
"    def isatty; false; end\n"
"  end\n"
"  OUT = Output.new\n"
"end\n";

/*
 * The block form of spi_query and Cursor#each are layered in Ruby on top of the
 * C primitives (__spi_query / spi_fetchrow / spi_cursor_close): the per-row loop
 * runs at the Ruby level, and the cursor is always closed via ensure.
 */
static const char *plruby_spi_prelude =
"def spi_query(sql, &blk)\n"
"  cur = __spi_query(sql)\n"
"  return cur unless blk\n"
"  begin\n"
"    while (row = spi_fetchrow(cur)); blk.call(row); end\n"
"  ensure\n"
"    spi_cursor_close(cur)\n"
"  end\n"
"  nil\n"
"end\n"
"class PLRuby::Cursor\n"
"  include Enumerable\n"
"  def each\n"
"    return enum_for(:each) unless block_given?\n"
"    while (row = spi_fetchrow(self)); yield row; end\n"
"    self\n"
"  end\n"
"end\n";

/* ---------------------------------------------------------------------
 * Messaging: elog / pg_raise
 * ------------------------------------------------------------------- */

static int
plruby_parse_elevel(const char *level, bool allow_full)
{
	if (allow_full && pg_strcasecmp(level, "DEBUG") == 0)
		return DEBUG1;
	if (allow_full && pg_strcasecmp(level, "LOG") == 0)
		return LOG;
	if (allow_full && pg_strcasecmp(level, "INFO") == 0)
		return INFO;
	if (pg_strcasecmp(level, "NOTICE") == 0)
		return NOTICE;
	if (pg_strcasecmp(level, "WARNING") == 0)
		return WARNING;
	if (pg_strcasecmp(level, "ERROR") == 0)
		return ERROR;
	return -1;
}

/*
 * A Ruby string argument as a null-terminated palloc'd C string.  Any embedded
 * NUL truncates, which is fine for log messages and SQL text.
 */
static char *
plruby_str_arg(VALUE v)
{
	VALUE		s = rb_obj_as_string(v);

	return pnstrdup(RSTRING_PTR(s), RSTRING_LEN(s));
}

static VALUE
plruby_elog(VALUE self, VALUE level, VALUE message)
{
	char	   *lvl = plruby_str_arg(level);
	char	   *msg = plruby_str_arg(message);
	int			elevel = plruby_parse_elevel(lvl, true);

	if (elevel < 0)
		rb_raise(rb_ePLRubyError, "elog: unrecognized level \"%s\"", lvl);

	if (elevel == ERROR)
		/* Unwind through Ruby so the top-level handler reports it cleanly */
		rb_raise(rb_ePLRubyError, "%s", msg);

	ereport(elevel, (errmsg("%s", msg)));
	return Qnil;
}

static VALUE
plruby_pg_raise(VALUE self, VALUE level, VALUE message)
{
	char	   *lvl = plruby_str_arg(level);
	char	   *msg = plruby_str_arg(message);
	int			elevel = plruby_parse_elevel(lvl, false);

	if (elevel < 0)
		rb_raise(rb_ePLRubyError, "pg_raise: incorrect log level \"%s\"", lvl);

	if (elevel == ERROR)
		rb_raise(rb_ePLRubyError, "%s", msg);

	ereport(elevel, (errmsg("%s", msg)));
	return Qnil;
}

/* ---------------------------------------------------------------------
 * Quoting helpers
 * ------------------------------------------------------------------- */

static VALUE
plruby_quote_literal(VALUE self, VALUE arg)
{
	char	   *in = plruby_str_arg(arg);
	char	   *q = quote_literal_cstr(in);
	VALUE		r = rb_str_new_cstr(q);

	pfree(q);
	pfree(in);
	return r;
}

static VALUE
plruby_quote_nullable(VALUE self, VALUE arg)
{
	char	   *in;
	char	   *q;
	VALUE		r;

	if (NIL_P(arg))
		return rb_str_new_cstr("NULL");

	in = plruby_str_arg(arg);
	q = quote_literal_cstr(in);
	r = rb_str_new_cstr(q);
	pfree(q);
	pfree(in);
	return r;
}

static VALUE
plruby_quote_ident(VALUE self, VALUE arg)
{
	char	   *in = plruby_str_arg(arg);
	/* quote_identifier may return its argument unchanged; do not free it */
	const char *q = quote_identifier(in);
	VALUE		r = rb_str_new_cstr(q);

	pfree(in);
	return r;
}

/* ---------------------------------------------------------------------
 * SPI query results, exposed to Ruby as PLRuby::SPIResult objects
 * ------------------------------------------------------------------- */

typedef struct
{
	SPITupleTable *tuptable;
	uint64		processed;
	uint64		current_row;
	int			status;
} plruby_spi_result;

/*
 * The tuple table lives in the SPI memory context and is reclaimed when the
 * SPI connection is finished (or the transaction ends), so we only free the
 * wrapper struct here -- freeing the tuptable ourselves could double-free.
 */
static void
spi_result_free(void *p)
{
	ruby_xfree(p);
}

static const rb_data_type_t plruby_spi_result_type = {
	"PLRuby::SPIResult",
	{NULL, spi_result_free, NULL},
	NULL, NULL,
	RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE cSPIResult;

static plruby_spi_result *
plruby_get_spi_result(VALUE v)
{
	plruby_spi_result *r;

	if (!rb_typeddata_is_kind_of(v, &plruby_spi_result_type))
		rb_raise(rb_ePLRubyError, "expected an SPI result");
	TypedData_Get_Struct(v, plruby_spi_result, &plruby_spi_result_type, r);
	return r;
}

static VALUE
plruby_make_spi_result(int status)
{
	plruby_spi_result *r;
	VALUE		obj;

	obj = TypedData_Make_Struct(cSPIResult, plruby_spi_result,
								&plruby_spi_result_type, r);
	r->processed = SPI_processed;
	r->tuptable = (status == SPI_OK_SELECT) ? SPI_tuptable : NULL;
	r->current_row = 0;
	r->status = status;
	return obj;
}

static VALUE
plruby_spi_exec(int argc, VALUE *argv, VALUE self)
{
	VALUE		q,
				lim;
	char	   *query;
	long		limit;
	long		status;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (argc < 1 || argc > 2)
		rb_raise(rb_ePLRubyError,
				 "spi_exec: expected 1 or 2 arguments, got %d", argc);
	q = argv[0];
	lim = (argc == 2) ? argv[1] : Qnil;
	query = plruby_str_arg(q);
	limit = NIL_P(lim) ? 0 : NUM2LONG(lim);

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		status = SPI_exec(query, limit);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();

	pfree(query);
	return plruby_make_spi_result(status);
}

static VALUE
plruby_spi_fetch_row(VALUE self, VALUE res)
{
	plruby_spi_result *r = plruby_get_spi_result(res);
	VALUE		row;

	if (r->status != SPI_OK_SELECT || r->tuptable == NULL)
		return Qnil;

	if (r->current_row >= r->processed)
		return Qnil;

	row = plruby_hash_from_tuple(r->tuptable->vals[r->current_row],
								 r->tuptable->tupdesc);
	r->current_row++;
	return row;
}

static VALUE
plruby_spi_processed(VALUE self, VALUE res)
{
	plruby_spi_result *r = plruby_get_spi_result(res);

	return ULL2NUM(r->processed);
}

static VALUE
plruby_spi_status(VALUE self, VALUE res)
{
	plruby_spi_result *r = plruby_get_spi_result(res);

	return rb_str_new_cstr(SPI_result_code_string(r->status));
}

static VALUE
plruby_spi_rewind(VALUE self, VALUE res)
{
	plruby_spi_result *r = plruby_get_spi_result(res);

	r->current_row = 0;
	return Qnil;
}

/* ---------------------------------------------------------------------
 * Prepared statements, exposed as PLRuby::SPIPlan objects
 * ------------------------------------------------------------------- */

typedef struct
{
	SPIPlanPtr	plan;
	int			nargs;
	Oid		   *argtypes;		/* malloc'd, length nargs */
	Oid		   *typinput;
	Oid		   *typioparam;
} plruby_spi_plan;

static void
spi_plan_free(void *p)
{
	plruby_spi_plan *pl = p;

	if (pl == NULL)
		return;
	if (pl->plan != NULL)
		SPI_freeplan(pl->plan);
	free(pl->argtypes);
	free(pl->typinput);
	free(pl->typioparam);
	ruby_xfree(pl);
}

static const rb_data_type_t plruby_spi_plan_type = {
	"PLRuby::SPIPlan",
	{NULL, spi_plan_free, NULL},
	NULL, NULL,
	RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE cSPIPlan;

static plruby_spi_plan *
plruby_get_spi_plan(VALUE v)
{
	plruby_spi_plan *pl;

	if (!rb_typeddata_is_kind_of(v, &plruby_spi_plan_type))
		rb_raise(rb_ePLRubyError, "expected a prepared SPI plan");
	TypedData_Get_Struct(v, plruby_spi_plan, &plruby_spi_plan_type, pl);
	return pl;
}

static VALUE
plruby_spi_prepare(int argc, VALUE *argv, VALUE self)
{
	char	   *query;
	int			ntypes;
	Oid		   *argtypes = NULL;
	Oid		   *typinput = NULL;
	Oid		   *typioparam = NULL;
	SPIPlanPtr	spiplan = NULL;
	plruby_spi_plan *pl;
	VALUE		obj;
	int			i;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (argc < 1)
		rb_raise(rb_ePLRubyError, "spi_prepare: missing query text");

	query = plruby_str_arg(argv[0]);
	ntypes = argc - 1;

	if (ntypes > 0)
	{
		argtypes = (Oid *) malloc(ntypes * sizeof(Oid));
		typinput = (Oid *) malloc(ntypes * sizeof(Oid));
		typioparam = (Oid *) malloc(ntypes * sizeof(Oid));
	}

	PG_TRY();
	{
		for (i = 0; i < ntypes; i++)
		{
			char	   *typ = plruby_str_arg(argv[i + 1]);
			Oid			typid;
			int32		typmod;

			parseTypeString(typ, &typid, &typmod, NULL);
			argtypes[i] = typid;
			getTypeInputInfo(typid, &typinput[i], &typioparam[i]);
			pfree(typ);
		}

		spiplan = SPI_prepare(query, ntypes, argtypes);
		if (spiplan == NULL)
			elog(ERROR, "spi_prepare: SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));
		if (SPI_keepplan(spiplan) != 0)
			elog(ERROR, "spi_prepare: SPI_keepplan failed");
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		CurrentResourceOwner = oldowner;
		free(argtypes);
		free(typinput);
		free(typioparam);
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();

	pfree(query);

	obj = TypedData_Make_Struct(cSPIPlan, plruby_spi_plan,
								&plruby_spi_plan_type, pl);
	pl->plan = spiplan;
	pl->nargs = ntypes;
	pl->argtypes = argtypes;
	pl->typinput = typinput;
	pl->typioparam = typioparam;
	return obj;
}

static VALUE
plruby_spi_exec_prepared(int argc, VALUE *argv, VALUE self)
{
	plruby_spi_plan *pl;
	long		status;
	Datum	   *values = NULL;
	char	   *nulls = NULL;
	int			i,
				nargs;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (argc < 1)
		rb_raise(rb_ePLRubyError, "spi_exec_prepared: missing plan");

	pl = plruby_get_spi_plan(argv[0]);
	if (pl->plan == NULL)
		rb_raise(rb_ePLRubyError,
				 "spi_exec_prepared: plan has already been freed");
	nargs = argc - 1;
	if (nargs != pl->nargs)
		rb_raise(rb_ePLRubyError,
				 "spi_exec_prepared: plan expects %d argument(s), got %d",
				 pl->nargs, nargs);

	if (pl->nargs > 0)
	{
		values = (Datum *) palloc(pl->nargs * sizeof(Datum));
		nulls = (char *) palloc(pl->nargs * sizeof(char));
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		for (i = 0; i < pl->nargs; i++)
		{
			char	   *val = plruby_value_to_cstring(argv[i + 1], true, true);

			if (val == NULL)
			{
				nulls[i] = 'n';
				values[i] = (Datum) 0;
			}
			else
			{
				nulls[i] = ' ';
				values[i] = OidInputFunctionCall(pl->typinput[i], val,
												 pl->typioparam[i], -1);
				pfree(val);
			}
		}

		status = SPI_execute_plan(pl->plan, values, nulls, false, 0);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();

	return plruby_make_spi_result(status);
}

static VALUE
plruby_spi_freeplan(VALUE self, VALUE planobj)
{
	plruby_spi_plan *pl = plruby_get_spi_plan(planobj);

	if (pl->plan != NULL)
	{
		SPI_freeplan(pl->plan);
		pl->plan = NULL;
	}
	return Qtrue;
}

/* ---------------------------------------------------------------------
 * Cursor streaming: spi_query opens a portal, spi_fetchrow reads one row at a
 * time, so large result sets are consumed without materializing them all.
 * ------------------------------------------------------------------- */

/*
 * Rows are fetched from the portal a batch at a time and buffered as a Ruby
 * Array, so streaming a large result never materializes it all at once.
 */
#define PLRUBY_CURSOR_BATCH 256

typedef struct
{
	Portal		portal;			/* NULL once closed or exhausted */
	SPIPlanPtr	plan;			/* saved plan backing the portal, freed on close */
	VALUE		buffer;			/* Ruby Array of buffered row Hashes, or Qnil */
	long		pos;			/* next index into buffer */
} plruby_cursor;

static void
cursor_mark(void *p)
{
	plruby_cursor *c = p;

	if (c != NULL)
		rb_gc_mark(c->buffer);
}

/*
 * Portals are closed by SPI_finish / transaction end, and by GC time SPI may be
 * gone, so the finalizer only frees the wrapper struct.
 */
static void
cursor_free(void *p)
{
	ruby_xfree(p);
}

static const rb_data_type_t plruby_cursor_type = {
	"PLRuby::Cursor",
	{cursor_mark, cursor_free, NULL},
	NULL, NULL,
	RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE cCursor;

static plruby_cursor *
plruby_get_cursor(VALUE v)
{
	plruby_cursor *c;

	if (!rb_typeddata_is_kind_of(v, &plruby_cursor_type))
		rb_raise(rb_ePLRubyError, "expected a cursor from spi_query");
	TypedData_Get_Struct(v, plruby_cursor, &plruby_cursor_type, c);
	return c;
}

static void
plruby_cursor_do_close(plruby_cursor *c)
{
	if (c->portal != NULL)
	{
		SPI_cursor_close(c->portal);
		c->portal = NULL;
	}
	if (c->plan != NULL)
	{
		SPI_freeplan(c->plan);
		c->plan = NULL;
	}
	/* Drop any buffered rows: a closed cursor must not keep yielding. */
	c->buffer = Qnil;
	c->pos = 0;
}

/*
 * Refill the cursor's row buffer with the next batch of up to
 * PLRUBY_CURSOR_BATCH rows, freeing the tuple table afterward so streaming
 * memory stays bounded.  Returns false at end of result (closing the cursor).
 */
static bool
plruby_cursor_refill(plruby_cursor *c)
{
	VALUE		batch = Qnil;
	MemoryContext oldcontext = CurrentMemoryContext;

	if (c->portal == NULL)
		return false;

	/*
	 * Fetch the next batch directly on the portal (no per-fetch subtransaction:
	 * running a continuing portal under a subtransaction that is then released
	 * corrupts the portal).  A query error therefore propagates as a Ruby
	 * exception but is terminal for the surrounding statement, like a PL/pgSQL
	 * cursor loop.
	 */
	PG_TRY();
	{
		SPI_cursor_fetch(c->portal, true, PLRUBY_CURSOR_BATCH);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		plruby_cursor_do_close(c);
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();

	if (SPI_processed > 0 && SPI_tuptable != NULL)
	{
		uint64		i;

		/* batch is a C-stack local, protected from GC while we build it */
		batch = rb_ary_new_capa(SPI_processed);
		for (i = 0; i < SPI_processed; i++)
			rb_ary_push(batch,
						plruby_hash_from_tuple(SPI_tuptable->vals[i],
											   SPI_tuptable->tupdesc));
	}

	/* Free the batch's tuple table so a long stream does not accumulate them. */
	if (SPI_tuptable != NULL)
		SPI_freetuptable(SPI_tuptable);

	if (NIL_P(batch))
	{
		plruby_cursor_do_close(c);
		c->buffer = Qnil;
		c->pos = 0;
		return false;
	}

	c->buffer = batch;
	c->pos = 0;
	return true;
}

/*
 * Fetch the next row as a Hash, or nil at end of result.  Serves rows from the
 * buffer, refilling from the portal a batch at a time.
 */
static VALUE
plruby_cursor_fetch_one(plruby_cursor *c)
{
	if (NIL_P(c->buffer) || c->pos >= RARRAY_LEN(c->buffer))
	{
		if (!plruby_cursor_refill(c))
			return Qnil;
	}
	return rb_ary_entry(c->buffer, c->pos++);
}

/*
 * __spi_query(sql) -> a cursor over the query.  The block form of the public
 * spi_query is layered on top of this in Ruby (see plruby_spi_prelude), so the
 * per-row iteration runs at the Ruby level rather than through a C rb_yield
 * loop across fetches.
 */
static VALUE
plruby_spi_query(int argc, VALUE *argv, VALUE self)
{
	char	   *query;
	SPIPlanPtr	plan;
	Portal		portal;
	plruby_cursor *c;
	VALUE		obj;
	MemoryContext oldcontext = CurrentMemoryContext;

	if (argc != 1)
		rb_raise(rb_ePLRubyError, "spi_query: expected 1 argument (query text)");
	query = plruby_str_arg(argv[0]);

	/*
	 * Open the portal at the function's transaction level (no subtransaction):
	 * a portal created inside a subtransaction that is then released loses its
	 * ownership and crashes on a continuation fetch.  Rows are fetched in
	 * per-batch subtransactions (see plruby_cursor_refill).
	 */
	PG_TRY();
	{
		plan = SPI_prepare(query, 0, NULL);
		if (plan == NULL)
			elog(ERROR, "spi_query: SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));
		/*
		 * Save the plan: an unsaved SPI_prepare plan can be released after the
		 * first fetch, leaving the portal referencing freed memory on the next
		 * one.  It is freed when the cursor closes.
		 */
		if (SPI_keepplan(plan) != 0)
			elog(ERROR, "spi_query: SPI_keepplan failed");
		portal = SPI_cursor_open(NULL, plan, NULL, NULL, true);
		if (portal == NULL)
			elog(ERROR, "spi_query: could not open cursor");
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		pfree(query);
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();
	pfree(query);

	obj = TypedData_Make_Struct(cCursor, plruby_cursor,
								&plruby_cursor_type, c);
	c->portal = portal;
	c->plan = plan;
	c->buffer = Qnil;
	c->pos = 0;

	return obj;
}

static VALUE
plruby_spi_fetchrow(VALUE self, VALUE cur)
{
	return plruby_cursor_fetch_one(plruby_get_cursor(cur));
}

static VALUE
plruby_spi_cursor_close(VALUE self, VALUE cur)
{
	plruby_cursor_do_close(plruby_get_cursor(cur));
	return Qnil;
}

/* ---------------------------------------------------------------------
 * Transaction control (procedures called with CALL, non-atomic context)
 * ------------------------------------------------------------------- */

static VALUE
plruby_spi_commit(VALUE self)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		SPI_commit();
#if PG_VERSION_NUM < 150000
		SPI_start_transaction();
#endif
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();
	return Qnil;
}

static VALUE
plruby_spi_rollback(VALUE self)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		SPI_rollback();
#if PG_VERSION_NUM < 150000
		SPI_start_transaction();
#endif
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		plruby_raise_pg_error(edata);
	}
	PG_END_TRY();
	return Qnil;
}

/* ---------------------------------------------------------------------
 * subtransaction { ... }  /  subtransaction(callable, *args)
 * ------------------------------------------------------------------- */

struct subxact_call
{
	VALUE		callable;
	int			argc;
	VALUE	   *argv;
};

static VALUE
subxact_invoke(VALUE p)
{
	struct subxact_call *c = (struct subxact_call *) p;

	return rb_funcallv(c->callable, rb_intern("call"), c->argc, c->argv);
}

static VALUE
plruby_subtransaction(int argc, VALUE *argv, VALUE self)
{
	struct subxact_call c;
	VALUE		result;
	int			state = 0;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (rb_block_given_p())
	{
		c.callable = rb_block_proc();
		c.argc = argc;
		c.argv = argv;
	}
	else
	{
		if (argc < 1)
			rb_raise(rb_ePLRubyError,
					 "subtransaction requires a block or a callable argument");
		c.callable = argv[0];
		c.argc = argc - 1;
		c.argv = argv + 1;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	result = rb_protect(subxact_invoke, (VALUE) &c, &state);

	if (state)
	{
		/* Body raised (a Ruby error or a re-raised DB error): roll back */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		rb_jump_tag(state);		/* re-raise the pending exception */
	}

	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	return result;
}

/* ---------------------------------------------------------------------
 * return_next: append a row to a set-returning function's result
 * ------------------------------------------------------------------- */

/* Stash the running SRF body's binding, for a no-argument return_next. */
static VALUE
plruby_set_binding(VALUE self, VALUE b)
{
	current_srf_binding = b;
	return Qnil;
}

/*
 * Build the row for a no-argument return_next() by reading the TABLE/OUT
 * column locals out of the body's stashed binding.
 */
static VALUE
get_table_arguments(void)
{
	VALUE		arr = rb_ary_new();
	int			natts = current_attinmeta->tupdesc->natts;
	int			i;
	ID			defined_p = rb_intern("local_variable_defined?");
	ID			get = rb_intern("local_variable_get");

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(current_attinmeta->tupdesc, i);
		char	   *attname = NameStr(att->attname);
		VALUE		v = Qnil;

		if (!NIL_P(current_srf_binding))
		{
			VALUE		sym = ID2SYM(rb_intern(attname));

			if (RTEST(rb_funcall(current_srf_binding, defined_p, 1, sym)))
				v = rb_funcall(current_srf_binding, get, 1, sym);
		}
		rb_ary_push(arr, v);
	}
	return arr;
}

static VALUE
plruby_return_next(int argc, VALUE *argv, VALUE self)
{
	MemoryContext oldcxt;
	VALUE		param;
	HeapTuple	tup;
	ReturnSetInfo *rsi;

	if (current_fcinfo == NULL || current_fcinfo->flinfo == NULL ||
		!current_fcinfo->flinfo->fn_retset)
		rb_raise(rb_ePLRubyError,
				 "cannot use return_next in functions not declared to return a set");

	if (argc > 1)
		rb_raise(rb_ePLRubyError, "return_next: expected 0 or 1 argument, got %d",
				 argc);

	rsi = (ReturnSetInfo *) current_fcinfo->resultinfo;

	if (argc == 0)
		param = get_table_arguments();
	else
		param = argv[0];

	oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	tup = plruby_srf_htup_from_value(param, current_attinmeta, current_memcxt);

	if (!current_tuplestore)
		current_tuplestore = tuplestore_begin_heap(true, false, work_mem);

	tuplestore_puttuple(current_tuplestore, tup);
	heap_freetuple(tup);

	MemoryContextSwitchTo(oldcxt);
	return Qnil;
}

/* ---------------------------------------------------------------------
 * Interpreter setup
 * ------------------------------------------------------------------- */

void
plruby_spi_init(void)
{
	/* SPI result and plan objects */
	cSPIResult = rb_define_class_under(rb_mPLRuby, "SPIResult", rb_cObject);
	rb_undef_alloc_func(cSPIResult);
	rb_gc_register_address(&cSPIResult);
	cSPIPlan = rb_define_class_under(rb_mPLRuby, "SPIPlan", rb_cObject);
	rb_undef_alloc_func(cSPIPlan);
	rb_gc_register_address(&cSPIPlan);
	cCursor = rb_define_class_under(rb_mPLRuby, "Cursor", rb_cObject);
	rb_undef_alloc_func(cCursor);
	rb_gc_register_address(&cCursor);

	/* $_SHARED: a Hash that persists across calls within a session */
	rb_gv_set("$_SHARED", rb_hash_new());

	/* Output redirection */
	rb_define_module_function(rb_mPLRuby, "__log",
							  RUBY_METHOD_FUNC(plruby_do_log), 1);
	plruby_eval_string(plruby_output_ruby, "plruby output setup");
	{
		VALUE		out = rb_const_get(rb_mPLRuby, rb_intern("OUT"));

		rb_gv_set("$stdout", out);
		rb_gv_set("$stderr", out);
	}

	/* Messaging */
	rb_define_global_function("elog", RUBY_METHOD_FUNC(plruby_elog), 2);
	rb_define_global_function("pg_raise", RUBY_METHOD_FUNC(plruby_pg_raise), 2);

	/* Quoting */
	rb_define_global_function("quote_literal",
							  RUBY_METHOD_FUNC(plruby_quote_literal), 1);
	rb_define_global_function("quote_nullable",
							  RUBY_METHOD_FUNC(plruby_quote_nullable), 1);
	rb_define_global_function("quote_ident",
							  RUBY_METHOD_FUNC(plruby_quote_ident), 1);

	/* SRF support */
	rb_define_module_function(rb_mPLRuby, "__set_binding",
							  RUBY_METHOD_FUNC(plruby_set_binding), 1);
	rb_define_global_function("return_next",
							  RUBY_METHOD_FUNC(plruby_return_next), -1);

	/* SPI query API */
	rb_define_global_function("spi_exec", RUBY_METHOD_FUNC(plruby_spi_exec), -1);
	rb_define_global_function("spi_fetch_row",
							  RUBY_METHOD_FUNC(plruby_spi_fetch_row), 1);
	rb_define_global_function("spi_processed",
							  RUBY_METHOD_FUNC(plruby_spi_processed), 1);
	rb_define_global_function("spi_status",
							  RUBY_METHOD_FUNC(plruby_spi_status), 1);
	rb_define_global_function("spi_rewind",
							  RUBY_METHOD_FUNC(plruby_spi_rewind), 1);

	/* Cursor streaming (block form + Cursor#each layered in the prelude below) */
	rb_define_global_function("__spi_query",
							  RUBY_METHOD_FUNC(plruby_spi_query), -1);
	rb_define_global_function("spi_fetchrow",
							  RUBY_METHOD_FUNC(plruby_spi_fetchrow), 1);
	rb_define_global_function("spi_cursor_close",
							  RUBY_METHOD_FUNC(plruby_spi_cursor_close), 1);

	/* Prepared statements */
	rb_define_global_function("spi_prepare",
							  RUBY_METHOD_FUNC(plruby_spi_prepare), -1);
	rb_define_global_function("spi_exec_prepared",
							  RUBY_METHOD_FUNC(plruby_spi_exec_prepared), -1);
	rb_define_global_function("spi_query_prepared",
							  RUBY_METHOD_FUNC(plruby_spi_exec_prepared), -1);
	rb_define_global_function("spi_freeplan",
							  RUBY_METHOD_FUNC(plruby_spi_freeplan), 1);

	/* Transaction control + subtransactions */
	rb_define_global_function("spi_commit",
							  RUBY_METHOD_FUNC(plruby_spi_commit), 0);
	rb_define_global_function("spi_rollback",
							  RUBY_METHOD_FUNC(plruby_spi_rollback), 0);
	rb_define_global_function("subtransaction",
							  RUBY_METHOD_FUNC(plruby_subtransaction), -1);

	/* Ruby-level layer for the cursor block form and Cursor#each */
	plruby_eval_string(plruby_spi_prelude, "plruby spi prelude");
}
