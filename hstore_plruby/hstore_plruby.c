/**********************************************************************
 * hstore_plruby.c - TRANSFORM between hstore and Ruby Hashes for PL/Ruby.
 *
 * hstore_to_plruby (FROM SQL) converts an hstore datum into a Ruby Hash of
 * String keys to String (or nil) values.  plruby_to_hstore (TO SQL)
 * converts a Ruby Hash back: keys and values are stringified (Symbols,
 * numbers, ... included) and a nil value becomes an hstore NULL.
 *
 * Unlike the in-core hstore_plperl, this module never touches hstore's
 * internal representation: it converts through the hstore extension's own
 * SQL-callable functions, hstore_to_array(hstore) and hstore(text[]),
 * whose OIDs are resolved at run time.  The hstore type is found through
 * the pg_transform row that points at this very function, so the lookup is
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
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(hstore_to_plruby);
PG_FUNCTION_INFO_V1(plruby_to_hstore);

/*
 * The hstore type OID, found via the pg_transform row whose FromSQL (or
 * ToSQL) function is this transform function itself.
 */
static Oid
hstore_type_from_transform(Oid trf_fn, bool fromsql)
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
 * The OID of an hstore-extension function living in the hstore type's own
 * namespace: hstore_to_array(hstore) or hstore(text[]).
 */
static Oid
lookup_hstore_fn(Oid hstore_oid, const char *name, Oid argtype)
{
	HeapTuple	typtup;
	Oid			nsp;
	oidvector  *argv;
	HeapTuple	ftup;
	Oid			result;

	typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(hstore_oid));
	if (!HeapTupleIsValid(typtup))
		elog(ERROR, "cache lookup failed for type %u", hstore_oid);
	nsp = ((Form_pg_type) GETSTRUCT(typtup))->typnamespace;
	ReleaseSysCache(typtup);

	argv = buildoidvector(&argtype, 1);
	ftup = SearchSysCache3(PROCNAMEARGSNSP, CStringGetDatum(name),
						   PointerGetDatum(argv), ObjectIdGetDatum(nsp));
	if (!HeapTupleIsValid(ftup))
		elog(ERROR, "could not find the hstore extension's %s function", name);
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
		Oid			hstore_oid = hstore_type_from_transform(fcinfo->flinfo->fn_oid,
															fromsql);

		cached = (Oid *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(Oid));
		if (fromsql)
			*cached = lookup_hstore_fn(hstore_oid, "hstore_to_array",
									   hstore_oid);
		else
			*cached = lookup_hstore_fn(hstore_oid, "hstore", TEXTARRAYOID);
		fcinfo->flinfo->fn_extra = cached;
	}
	return *cached;
}

/*
 * A Ruby String in the database encoding: tag UTF-8 databases properly and
 * leave anything else as binary (matching plruby's fallback behavior).
 */
static VALUE
hs_str_new(const char *ptr, long len)
{
	if (GetDatabaseEncoding() == PG_UTF8)
		return rb_enc_str_new(ptr, len, rb_utf8_encoding());
	return rb_str_new(ptr, len);
}

Datum
hstore_to_plruby(PG_FUNCTION_ARGS)
{
	Oid			to_array = conversion_fn(fcinfo, true);
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			i;
	VALUE		hash;

	/* hstore_to_array: alternating key, value; a NULL value stays NULL */
	arr = DatumGetArrayTypeP(OidFunctionCall1(to_array, PG_GETARG_DATUM(0)));
	deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, &nulls, &nelems);

	hash = rb_hash_new();
	for (i = 0; i + 1 < nelems; i += 2)
	{
		text	   *kt = DatumGetTextPP(elems[i]);
		VALUE		k = hs_str_new(VARDATA_ANY(kt), VARSIZE_ANY_EXHDR(kt));
		VALUE		v = Qnil;

		if (!nulls[i + 1])
		{
			text	   *vt = DatumGetTextPP(elems[i + 1]);

			v = hs_str_new(VARDATA_ANY(vt), VARSIZE_ANY_EXHDR(vt));
		}
		rb_hash_aset(hash, k, v);
	}

	PG_RETURN_DATUM((Datum) hash);
}

/* ---------------------------------------------------------------------
 * Ruby Hash -> hstore
 * ------------------------------------------------------------------- */

typedef struct hs_build_state
{
	Datum	   *elems;
	bool	   *nulls;
	int			next;
} hs_build_state;

static int
hs_hash_pair(VALUE key, VALUE val, VALUE arg)
{
	hs_build_state *st = (hs_build_state *) arg;
	VALUE		kstr = rb_obj_as_string(key);

	st->elems[st->next] =
		PointerGetDatum(cstring_to_text_with_len(RSTRING_PTR(kstr),
												 RSTRING_LEN(kstr)));
	st->nulls[st->next] = false;
	st->next++;

	if (NIL_P(val))
	{
		st->elems[st->next] = (Datum) 0;
		st->nulls[st->next] = true;
	}
	else
	{
		VALUE		vstr = rb_obj_as_string(val);

		st->elems[st->next] =
			PointerGetDatum(cstring_to_text_with_len(RSTRING_PTR(vstr),
													 RSTRING_LEN(vstr)));
		st->nulls[st->next] = false;
	}
	st->next++;
	return ST_CONTINUE;
}

Datum
plruby_to_hstore(PG_FUNCTION_ARGS)
{
	Oid			from_array = conversion_fn(fcinfo, false);
	VALUE		v = (VALUE) PG_GETARG_DATUM(0);
	hs_build_state st;
	long		npairs;
	int			dims[1];
	int			lbs[1] = {1};
	ArrayType  *arr;

	if (!RB_TYPE_P(v, T_HASH))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot transform Ruby object of class %s to hstore",
						rb_obj_classname(v)),
				 errhint("An hstore value is built from a Hash.")));

	npairs = RHASH_SIZE(v);
	st.elems = (Datum *) palloc(npairs * 2 * sizeof(Datum));
	st.nulls = (bool *) palloc(npairs * 2 * sizeof(bool));
	st.next = 0;
	rb_hash_foreach(v, hs_hash_pair, (VALUE) &st);

	dims[0] = st.next;
	arr = construct_md_array(st.elems, st.nulls, 1, dims, lbs,
							 TEXTOID, -1, false, 'i');

	PG_RETURN_DATUM(OidFunctionCall1(from_array, PointerGetDatum(arr)));
}