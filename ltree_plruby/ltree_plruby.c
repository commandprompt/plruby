/**********************************************************************
 * ltree_plruby.c - TRANSFORM between ltree and Ruby Arrays for PL/Ruby.
 *
 * ltree_to_plruby (FROM SQL) converts an ltree datum into a Ruby Array of
 * its label Strings ('Top.Science.Astronomy' -> ['Top','Science',
 * 'Astronomy']).  plruby_to_ltree (TO SQL) converts a Ruby Array back:
 * elements are stringified and joined with '.', then parsed as an ltree.
 * This mirrors the in-core ltree_plpython transform (ltree <-> list).
 *
 * Like hstore_plruby, this module never touches ltree's internal
 * representation: it converts through the ltree extension's own
 * SQL-callable functions, ltree2text(ltree) and text2ltree(text), whose
 * OIDs are resolved at run time.  The ltree type is found through the
 * pg_transform row that points at this very function, so the lookup is
 * independent of schemas and search_path.  The resolved OID is cached in
 * fn_extra.
 *
 * The Ruby VM is owned and initialized by plruby; these functions only run
 * inside a PL/Ruby function call, so it is always up.
 *
 * Copyright (c) 2026 ChronicallyJD.  MIT License; see LICENSE.
 **********************************************************************/

#include "postgres.h"

#include <ruby.h>
#include <ruby/encoding.h>

#include "access/genam.h"
#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#else
#include "access/heapam.h"
#define table_open(rel, lock) heap_open(rel, lock)
#define table_close(rel, lock) heap_close(rel, lock)
#endif
#include "catalog/pg_proc.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(ltree_to_plruby);
PG_FUNCTION_INFO_V1(plruby_to_ltree);

/*
 * The ltree type OID, found via the pg_transform row whose FromSQL (or
 * ToSQL) function is this transform function itself.
 */
static Oid
ltree_type_from_transform(Oid trf_fn, bool fromsql)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	rel = table_open(TransformRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_transform t = (Form_pg_transform) GETSTRUCT(tup);

		if ((fromsql ? t->trffromsql : t->trftosql) == trf_fn)
		{
			result = t->trftype;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	if (!OidIsValid(result))
		elog(ERROR, "could not find pg_transform entry for function %u", trf_fn);
	return result;
}

/*
 * The OID of an ltree-extension function living in the ltree type's own
 * namespace: ltree2text(ltree) or text2ltree(text).
 */
static Oid
lookup_ltree_fn(Oid ltree_oid, const char *name, Oid argtype)
{
	HeapTuple	typtup;
	Oid			nsp;
	oidvector  *argv;
	HeapTuple	ftup;
	Oid			result;

	typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(ltree_oid));
	if (!HeapTupleIsValid(typtup))
		elog(ERROR, "cache lookup failed for type %u", ltree_oid);
	nsp = ((Form_pg_type) GETSTRUCT(typtup))->typnamespace;
	ReleaseSysCache(typtup);

	argv = buildoidvector(&argtype, 1);
	ftup = SearchSysCache3(PROCNAMEARGSNSP, CStringGetDatum(name),
						   PointerGetDatum(argv), ObjectIdGetDatum(nsp));
	if (!HeapTupleIsValid(ftup))
		elog(ERROR, "could not find the ltree extension's %s function", name);
#if PG_VERSION_NUM >= 120000
	result = ((Form_pg_proc) GETSTRUCT(ftup))->oid;
#else
	result = HeapTupleGetOid(ftup);
#endif
	ReleaseSysCache(ftup);
	return result;
}

/* Resolve (once per FmgrInfo) the conversion function this direction uses. */
static Oid
conversion_fn(FunctionCallInfo fcinfo, bool fromsql)
{
	Oid		   *cached = (Oid *) fcinfo->flinfo->fn_extra;

	if (cached == NULL)
	{
		Oid			ltree_oid = ltree_type_from_transform(fcinfo->flinfo->fn_oid,
														  fromsql);

		cached = (Oid *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(Oid));
		if (fromsql)
			*cached = lookup_ltree_fn(ltree_oid, "ltree2text", ltree_oid);
		else
			*cached = lookup_ltree_fn(ltree_oid, "text2ltree", TEXTOID);
		fcinfo->flinfo->fn_extra = cached;
	}
	return *cached;
}

/*
 * A Ruby String in the database encoding: tag UTF-8 databases properly and
 * leave anything else as binary (matching plruby's fallback behavior).
 */
static VALUE
lt_str_new(const char *ptr, long len)
{
	if (GetDatabaseEncoding() == PG_UTF8)
		return rb_enc_str_new(ptr, len, rb_utf8_encoding());
	return rb_str_new(ptr, len);
}

Datum
ltree_to_plruby(PG_FUNCTION_ARGS)
{
	Oid			to_text = conversion_fn(fcinfo, true);
	text	   *t;
	VALUE		path;

	/* ltree2text: 'a.b.c' (an empty ltree is the empty string) */
	t = DatumGetTextPP(OidFunctionCall1(to_text, PG_GETARG_DATUM(0)));
	path = lt_str_new(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));

	/* Labels never contain '.', so a literal split yields the label Array;
	 * "".split('.') is [], so an empty ltree becomes an empty Array. */
	PG_RETURN_DATUM((Datum) rb_str_split(path, "."));
}

Datum
plruby_to_ltree(PG_FUNCTION_ARGS)
{
	Oid			from_text = conversion_fn(fcinfo, false);
	VALUE		v = (VALUE) PG_GETARG_DATUM(0);
	VALUE		joined;
	text	   *t;

	if (!RB_TYPE_P(v, T_ARRAY))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot transform Ruby object of class %s to ltree",
						rb_obj_classname(v)),
				 errhint("An ltree value is built from an Array of labels.")));

	/* Array#join stringifies each element (Symbols, numbers, ...) with '.'. */
	joined = rb_ary_join(v, rb_str_new_cstr("."));
	t = cstring_to_text_with_len(RSTRING_PTR(joined), RSTRING_LEN(joined));

	PG_RETURN_DATUM(OidFunctionCall1(from_text, PointerGetDatum(t)));
}
