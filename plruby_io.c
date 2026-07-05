/**********************************************************************
 * plruby_io.c
 *
 * Conversions between the PostgreSQL Datum representation and Ruby
 * VALUEs: scalars, (possibly nested) arrays, and composite/record
 * tuples, in both directions.
 *
 * Copyright (c) 2026 ChronicallyJD.  MIT License; see LICENSE.
 **********************************************************************/

#include "postgres.h"
#include "plruby.h"
#include "plruby_io.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include <ruby/encoding.h>

/* ---------------------------------------------------------------------
 * Scalar leaf conversion
 * ------------------------------------------------------------------- */

/*
 * Map the current database encoding to the equivalent Ruby encoding name, or
 * NULL if there is no good match (SQL_ASCII, MULE_INTERNAL, ...), in which case
 * the caller falls back to ASCII-8BIT (binary), which is byte-preserving.
 */
static const char *
plruby_pg_to_ruby_encname(int pgenc)
{
	switch (pgenc)
	{
		case PG_UTF8:			return "UTF-8";
		case PG_LATIN1:			return "ISO-8859-1";
		case PG_LATIN2:			return "ISO-8859-2";
		case PG_LATIN3:			return "ISO-8859-3";
		case PG_LATIN4:			return "ISO-8859-4";
		case PG_LATIN5:			return "ISO-8859-9";
		case PG_LATIN6:			return "ISO-8859-10";
		case PG_LATIN7:			return "ISO-8859-13";
		case PG_LATIN8:			return "ISO-8859-14";
		case PG_LATIN9:			return "ISO-8859-15";
		case PG_LATIN10:		return "ISO-8859-16";
		case PG_WIN1250:		return "Windows-1250";
		case PG_WIN1251:		return "Windows-1251";
		case PG_WIN1252:		return "Windows-1252";
		case PG_WIN1253:		return "Windows-1253";
		case PG_WIN1254:		return "Windows-1254";
		case PG_WIN1255:		return "Windows-1255";
		case PG_WIN1256:		return "Windows-1256";
		case PG_WIN1257:		return "Windows-1257";
		case PG_WIN1258:		return "Windows-1258";
		case PG_WIN866:			return "IBM866";
		case PG_WIN874:			return "Windows-874";
		case PG_KOI8R:			return "KOI8-R";
		case PG_KOI8U:			return "KOI8-U";
		case PG_EUC_JP:			return "EUC-JP";
		case PG_EUC_CN:			return "GB2312";
		case PG_EUC_KR:			return "EUC-KR";
		case PG_EUC_TW:			return "EUC-TW";
		case PG_EUC_JIS_2004:	return "EUC-JP";
		case PG_SJIS:			return "Windows-31J";
		case PG_SHIFT_JIS_2004:	return "Shift_JIS";
		case PG_BIG5:			return "Big5";
		case PG_GBK:			return "GBK";
		case PG_UHC:			return "CP949";
		case PG_GB18030:		return "GB18030";
		default:				return NULL;
	}
}

/*
 * The Ruby encoding matching the database encoding, resolved once per backend
 * (the database encoding is fixed for the life of the connection).  Encodings
 * Ruby does not know fall back to ASCII-8BIT.
 */
static rb_encoding *
plruby_db_ruby_encoding(void)
{
	static int	cached_pgenc = -1;
	static rb_encoding *cached = NULL;
	int			pgenc = GetDatabaseEncoding();

	if (pgenc != cached_pgenc)
	{
		const char *name = plruby_pg_to_ruby_encname(pgenc);
		int			idx = (name != NULL) ? rb_enc_find_index(name) : -1;

		cached = (idx >= 0) ? rb_enc_from_index(idx) : rb_ascii8bit_encoding();
		cached_pgenc = pgenc;
	}
	return cached;
}

/*
 * Build a Ruby String from PostgreSQL text, tagged with the Ruby encoding that
 * matches the database encoding, so multibyte String operations (length,
 * reverse, regexp, ...) behave.  Unknown encodings become ASCII-8BIT (binary),
 * which is byte-preserving and never corrupts data.
 */
VALUE
plruby_str_from_pg(const char *str, long len)
{
	return rb_enc_str_new(str, len, plruby_db_ruby_encoding());
}

VALUE
plruby_scalar_from_cstring(const char *str, Oid typeoid)
{
	switch (typeoid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
			return rb_cstr2inum(str, 10);
		case FLOAT4OID:
		case FLOAT8OID:
			return rb_float_new(strtod(str, NULL));
		case BOOLOID:
			return (str[0] == 't' || str[0] == 'T') ? Qtrue : Qfalse;
		default:
			/* numeric, text, varchar, everything else: keep as a String */
			return plruby_str_from_pg(str, strlen(str));
	}
}

/* ---------------------------------------------------------------------
 * PostgreSQL array text -> nested Ruby Array
 * ------------------------------------------------------------------- */

typedef struct
{
	const char *s;
	Oid			elemtype;
} arrparse;

static VALUE parse_array_rec(arrparse *a);

static VALUE
parse_element(arrparse *a)
{
	StringInfoData buf;
	VALUE		v;

	if (*a->s == '{')
		return parse_array_rec(a);

	initStringInfo(&buf);

	if (*a->s == '"')
	{
		/* quoted element: honor \\ and \" escapes */
		a->s++;
		while (*a->s && *a->s != '"')
		{
			if (*a->s == '\\' && a->s[1] != '\0')
				a->s++;
			appendStringInfoChar(&buf, *a->s);
			a->s++;
		}
		if (*a->s == '"')
			a->s++;
	}
	else
	{
		/* unquoted element: read up to the next delimiter */
		while (*a->s && *a->s != ',' && *a->s != '}')
		{
			appendStringInfoChar(&buf, *a->s);
			a->s++;
		}
		if (pg_strcasecmp(buf.data, "NULL") == 0)
		{
			pfree(buf.data);
			return Qnil;
		}
	}

	v = plruby_scalar_from_cstring(buf.data, a->elemtype);
	pfree(buf.data);
	return v;
}

static VALUE
parse_array_rec(arrparse *a)
{
	VALUE		arr = rb_ary_new();

	/* *a->s == '{' */
	a->s++;
	if (*a->s == '}')
	{
		a->s++;
		return arr;
	}
	for (;;)
	{
		rb_ary_push(arr, parse_element(a));
		if (*a->s == ',')
		{
			a->s++;
			continue;
		}
		if (*a->s == '}')
			a->s++;
		break;
	}
	return arr;
}

VALUE
plruby_array_from_pg(char *input, Oid elemtype)
{
	arrparse	a;

	/* Skip an optional dimension decoration like "[1:3]={...}" */
	if (*input == '[')
	{
		char	   *eq = strchr(input, '=');

		if (eq != NULL)
			input = eq + 1;
	}

	if (*input != '{')
		/* not actually an array literal; hand back the raw string */
		return rb_str_new_cstr(input);

	a.s = input;
	a.elemtype = elemtype;
	return parse_array_rec(&a);
}

/* ---------------------------------------------------------------------
 * Ruby Array -> PostgreSQL array input literal
 * ------------------------------------------------------------------- */

static void
array_to_pg_rec(StringInfo str, VALUE array)
{
	long		i,
				n = RARRAY_LEN(array);

	appendStringInfoChar(str, '{');
	for (i = 0; i < n; i++)
	{
		VALUE		el = rb_ary_entry(array, i);

		if (i > 0)
			appendStringInfoChar(str, ',');

		switch (TYPE(el))
		{
			case T_ARRAY:
				array_to_pg_rec(str, el);
				break;
			case T_NIL:
				appendStringInfoString(str, "NULL");
				break;
			case T_TRUE:
				appendStringInfoString(str, "t");
				break;
			case T_FALSE:
				appendStringInfoString(str, "f");
				break;
			case T_FIXNUM:
			case T_BIGNUM:
			case T_FLOAT:
			{
				VALUE		s = rb_obj_as_string(el);

				appendBinaryStringInfo(str, RSTRING_PTR(s), RSTRING_LEN(s));
				break;
			}
			default:
			{
				/* strings and anything else: quote and escape */
				VALUE		s = rb_obj_as_string(el);
				const char *p = RSTRING_PTR(s);
				long		len = RSTRING_LEN(s),
							j;

				appendStringInfoChar(str, '"');
				for (j = 0; j < len; j++)
				{
					if (p[j] == '"' || p[j] == '\\')
						appendStringInfoChar(str, '\\');
					appendStringInfoChar(str, p[j]);
				}
				appendStringInfoChar(str, '"');
				break;
			}
		}
	}
	appendStringInfoChar(str, '}');
}

char *
plruby_array_to_pg(VALUE array)
{
	StringInfoData str;

	initStringInfo(&str);
	array_to_pg_rec(&str, array);
	return str.data;
}

/* ---------------------------------------------------------------------
 * Ruby VALUE -> C string
 * ------------------------------------------------------------------- */

char *
plruby_value_to_cstring(VALUE val, bool do_array, bool null_ok)
{
	switch (TYPE(val))
	{
		case T_NIL:
			if (null_ok)
				return NULL;
			elog(ERROR, "unexpected NULL value");
			return NULL;		/* keep compiler quiet */
		case T_TRUE:
			return pstrdup("true");
		case T_FALSE:
			return pstrdup("false");
		case T_ARRAY:
			if (!do_array)
				elog(ERROR, "cannot use an array value here");
			return plruby_array_to_pg(val);
		case T_FIXNUM:
		case T_BIGNUM:
		case T_FLOAT:
		case T_STRING:
		default:
		{
			VALUE		s = rb_obj_as_string(val);

			return pnstrdup(RSTRING_PTR(s), RSTRING_LEN(s));
		}
	}
}

/* ---------------------------------------------------------------------
 * Ruby VALUE -> Datum (recursive: scalars, arrays, composites)
 * ------------------------------------------------------------------- */

static Datum plruby_composite_datum(VALUE val, Oid typeoid, int32 typmod);
static Datum plruby_array_datum(VALUE val, Oid elemtype);
static void plruby_array_flatten(VALUE val, int depth, int ndims, int *dims,
								 Oid elemtype, Datum *elems, bool *nulls,
								 int *idx, bool *hasnull);

Datum
plruby_datum_from_value(VALUE val, Oid typeoid, int32 typmod, bool *isnull)
{
	Oid			elemtype;

	if (NIL_P(val))
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	/* An array type fed a Ruby Array: build an array datum recursively. */
	elemtype = get_element_type(typeoid);
	if (OidIsValid(elemtype) && RB_TYPE_P(val, T_ARRAY))
		return plruby_array_datum(val, elemtype);

	/* A composite type fed a Ruby Hash (or positional Array): build a record. */
	if (type_is_rowtype(typeoid) &&
		(RB_TYPE_P(val, T_HASH) || RB_TYPE_P(val, T_ARRAY)))
		return plruby_composite_datum(val, typeoid, typmod);

	/* Scalar leaf: convert via the target type's input function. */
	{
		char	   *s = plruby_value_to_cstring(val, true, true);
		Oid			infn,
					ioparam;
		Datum		d;

		getTypeInputInfo(typeoid, &infn, &ioparam);
		d = OidInputFunctionCall(infn, s, ioparam, typmod);
		if (s != NULL)
			pfree(s);
		return d;
	}
}

static Datum
plruby_composite_datum(VALUE val, Oid typeoid, int32 typmod)
{
	TupleDesc	tupdesc;
	HeapTuple	tup;
	Datum		d;

	tupdesc = lookup_rowtype_tupdesc(typeoid, typmod);
	tup = plruby_htup_from_value(val, tupdesc);
	d = HeapTupleGetDatum(tup);
	ReleaseTupleDesc(tupdesc);
	return d;
}

static Datum
plruby_array_datum(VALUE val, Oid elemtype)
{
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];
	int			nelems = 1;
	Datum	   *elems;
	bool	   *nulls;
	bool		hasnull = false;
	int			idx = 0;
	int			i;
	ArrayType  *arr;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);

	if (type_is_rowtype(elemtype))
	{
		/* Composite elements: a plain 1-D array (no multidimensional descent). */
		ndims = 1;
		dims[0] = RARRAY_LEN(val);
		lbs[0] = 1;
	}
	else
	{
		/* Scalar elements: infer dimensions from the nested Ruby arrays. */
		VALUE		v = val;

		while (RB_TYPE_P(v, T_ARRAY) && ndims < MAXDIM)
		{
			dims[ndims] = RARRAY_LEN(v);
			lbs[ndims] = 1;
			ndims++;
			if (RARRAY_LEN(v) == 0)
				break;
			v = rb_ary_entry(v, 0);
		}
	}

	for (i = 0; i < ndims; i++)
		nelems *= dims[i];

	if (nelems == 0)
		return PointerGetDatum(construct_empty_array(elemtype));

	elems = (Datum *) palloc(nelems * sizeof(Datum));
	nulls = (bool *) palloc(nelems * sizeof(bool));

	plruby_array_flatten(val, 0, ndims, dims, elemtype,
						 elems, nulls, &idx, &hasnull);

	arr = construct_md_array(elems, hasnull ? nulls : NULL, ndims, dims, lbs,
							 elemtype, elmlen, elmbyval, elmalign);
	return PointerGetDatum(arr);
}

static void
plruby_array_flatten(VALUE val, int depth, int ndims, int *dims, Oid elemtype,
					 Datum *elems, bool *nulls, int *idx, bool *hasnull)
{
	int			i;

	if (depth == ndims)
	{
		elems[*idx] = plruby_datum_from_value(val, elemtype, -1, &nulls[*idx]);
		if (nulls[*idx])
			*hasnull = true;
		(*idx)++;
		return;
	}

	if (!RB_TYPE_P(val, T_ARRAY) || RARRAY_LEN(val) != dims[depth])
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("multidimensional arrays must have "
						"sub-arrays with matching dimensions")));

	for (i = 0; i < dims[depth]; i++)
		plruby_array_flatten(rb_ary_entry(val, i), depth + 1, ndims, dims,
							 elemtype, elems, nulls, idx, hasnull);
}

/* ---------------------------------------------------------------------
 * Tuple -> Ruby Hash
 * ------------------------------------------------------------------- */

VALUE
plruby_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	VALUE		h = rb_hash_new();
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *attname;
		Datum		attr;
		bool		isnull;
		Oid			typoutput;
		bool		typisvarlena;
		Oid			elemtype;
		char	   *outputstr;
		VALUE		v;

		if (att->attisdropped)
			continue;

		attname = NameStr(att->attname);
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			rb_hash_aset(h, rb_str_new_cstr(attname), Qnil);
			continue;
		}

		/* A composite field: recurse into a nested Hash. */
		if (type_is_rowtype(att->atttypid))
		{
			HeapTupleHeader th = DatumGetHeapTupleHeader(attr);
			TupleDesc	subdesc;
			HeapTupleData tmptup;

			subdesc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(th),
											 HeapTupleHeaderGetTypMod(th));
			tmptup.t_len = HeapTupleHeaderGetDatumLength(th);
			ItemPointerSetInvalid(&(tmptup.t_self));
			tmptup.t_tableOid = InvalidOid;
			tmptup.t_data = th;
			rb_hash_aset(h, rb_str_new_cstr(attname),
						 plruby_hash_from_tuple(&tmptup, subdesc));
			ReleaseTupleDesc(subdesc);
			continue;
		}

		getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
		outputstr = OidOutputFunctionCall(typoutput, attr);

		elemtype = get_element_type(att->atttypid);
		if (OidIsValid(elemtype))
			v = plruby_array_from_pg(outputstr, elemtype);
		else
			v = plruby_scalar_from_cstring(outputstr, att->atttypid);

		rb_hash_aset(h, rb_str_new_cstr(attname), v);
		pfree(outputstr);
	}

	return h;
}

/* ---------------------------------------------------------------------
 * Ruby Hash/Array -> HeapTuple (by attribute name, positional fallback)
 * ------------------------------------------------------------------- */

HeapTuple
plruby_htup_from_value(VALUE val, TupleDesc tupdesc)
{
	MemoryContext tmpcxt,
				oldcxt;
	HeapTuple	ret;
	Datum	   *dvalues;
	bool	   *nulls;
	int			natts = tupdesc->natts;
	int			i;

	tmpcxt = AllocSetContextCreate(CurTransactionContext,
								   "plruby htup_from_value",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	dvalues = (Datum *) palloc0(natts * sizeof(Datum));
	nulls = (bool *) palloc(natts * sizeof(bool));
	for (i = 0; i < natts; i++)
		nulls[i] = true;

	if (RB_TYPE_P(val, T_HASH))
	{
		bool		anymatch = false;

		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			VALUE		el;

			if (att->attisdropped)
				continue;
			el = rb_hash_lookup2(val, rb_str_new_cstr(NameStr(att->attname)),
								 Qundef);
			if (el == Qundef)
				continue;
			anymatch = true;
			dvalues[i] = plruby_datum_from_value(el, att->atttypid,
												 att->atttypmod, &nulls[i]);
		}

		/* No attribute names matched: fall back to the hash's values in order */
		if (!anymatch)
		{
			VALUE		vals = rb_funcall(val, rb_intern("values"), 0);
			long		n = RARRAY_LEN(vals);

			for (i = 0; i < natts && i < n; i++)
			{
				Form_pg_attribute att = TupleDescAttr(tupdesc, i);

				if (att->attisdropped)
					continue;
				dvalues[i] = plruby_datum_from_value(rb_ary_entry(vals, i),
													 att->atttypid,
													 att->atttypmod, &nulls[i]);
			}
		}
	}
	else if (RB_TYPE_P(val, T_ARRAY))
	{
		long		n = RARRAY_LEN(val);

		for (i = 0; i < natts && i < n; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped)
				continue;
			dvalues[i] = plruby_datum_from_value(rb_ary_entry(val, i),
												 att->atttypid,
												 att->atttypmod, &nulls[i]);
		}
	}
	else
	{
		/* a scalar: only sensible for a single-column result */
		Form_pg_attribute att = TupleDescAttr(tupdesc, 0);

		dvalues[0] = plruby_datum_from_value(val, att->atttypid,
											 att->atttypmod, &nulls[0]);
	}

	MemoryContextSwitchTo(oldcxt);
	ret = heap_form_tuple(tupdesc, dvalues, nulls);

	MemoryContextDelete(tmpcxt);

	return ret;
}

/* ---------------------------------------------------------------------
 * Ruby value -> HeapTuple for a SRF row (positional, shared context)
 * ------------------------------------------------------------------- */

HeapTuple
plruby_srf_htup_from_value(VALUE val, AttInMetadata *attinmeta,
						   MemoryContext cxt)
{
	MemoryContext oldcxt;
	HeapTuple	ret;
	char	  **values;
	int			i = 0;
	int			natts = attinmeta->tupdesc->natts;

	oldcxt = MemoryContextSwitchTo(cxt);

	values = (char **) palloc0(natts * sizeof(char *));

	if (RB_TYPE_P(val, T_HASH))
	{
		/* map by attribute name */
		for (i = 0; i < natts; i++)
		{
			char	   *key = NameStr(TupleDescAttr(attinmeta->tupdesc, i)->attname);
			VALUE		el = rb_hash_lookup2(val, rb_str_new_cstr(key), Qundef);

			if (el == Qundef || NIL_P(el))
				values[i] = NULL;
			else
				values[i] = plruby_value_to_cstring(el, true, true);
		}
	}
	else if (RB_TYPE_P(val, T_ARRAY))
	{
		if (natts == 1)
		{
			Form_pg_attribute att = TupleDescAttr(attinmeta->tupdesc, 0);

			/* single column: if it is itself an array type, pass the whole
			 * Ruby array as the one value; otherwise use its first element */
			if (att->attndims != 0 || OidIsValid(get_element_type(att->atttypid)))
				values[0] = plruby_value_to_cstring(val, true, true);
			else
			{
				VALUE		el = (RARRAY_LEN(val) > 0) ? rb_ary_entry(val, 0) : Qnil;

				values[0] = plruby_value_to_cstring(el, true, true);
			}
		}
		else
		{
			long		n = RARRAY_LEN(val);

			for (i = 0; i < natts && i < n; i++)
				values[i] = plruby_value_to_cstring(rb_ary_entry(val, i), true, true);
			if (n > natts)
				elog(WARNING, "more elements in array than attributes in return type");
		}
	}
	else
	{
		if (natts != 1)
			ereport(ERROR,
					(errmsg("returned value does not match the declared return type")));
		values[0] = plruby_value_to_cstring(val, true, true);
	}

	MemoryContextSwitchTo(oldcxt);
	ret = BuildTupleFromCStrings(attinmeta, values);
	MemoryContextReset(cxt);

	return ret;
}

/* ---------------------------------------------------------------------
 * $_TD['new'] -> modified NEW tuple for a BEFORE trigger
 * ------------------------------------------------------------------- */

HeapTuple
plruby_modify_tuple(VALUE td, TriggerData *tdata)
{
	TupleDesc	tupdesc;
	HeapTuple	rettuple;
	VALUE		newtup;
	AttInMetadata *attinmeta;
	char	  **vals;
	int			i;
	MemoryContext tmpcxt,
				oldcxt;

	if (!RB_TYPE_P(td, T_HASH))
		elog(ERROR, "$_TD is not a Hash");

	newtup = rb_hash_aref(td, rb_str_new_cstr("new"));
	if (!RB_TYPE_P(newtup, T_HASH))
		elog(ERROR, "$_TD['new'] must be a Hash");

	tmpcxt = AllocSetContextCreate(CurTransactionContext,
								   "plruby NEW context",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	tupdesc = tdata->tg_relation->rd_att;
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	vals = (char **) palloc0(tupdesc->natts * sizeof(char *));

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *attname;
		VALUE		el;

		if (att->attisdropped)
			continue;

		attname = NameStr(att->attname);
		el = rb_hash_lookup2(newtup, rb_str_new_cstr(attname), Qundef);
		if (el == Qundef)
			elog(ERROR, "$_TD['new'] does not contain attribute \"%s\"", attname);

		vals[i] = plruby_value_to_cstring(el, true, true);
	}

	MemoryContextSwitchTo(oldcxt);
	rettuple = BuildTupleFromCStrings(attinmeta, vals);
	MemoryContextDelete(tmpcxt);

	return rettuple;
}
