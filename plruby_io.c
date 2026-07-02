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

/* ---------------------------------------------------------------------
 * Scalar leaf conversion
 * ------------------------------------------------------------------- */

/*
 * Build a Ruby String from PostgreSQL text, tagged with a sensible encoding so
 * that multibyte String operations (length, reverse, regexp, ...) behave.  For
 * the common UTF-8 database we tag UTF-8; otherwise we fall back to ASCII-8BIT
 * (binary), which is byte-preserving and never corrupts data.
 */
VALUE
plruby_str_from_pg(const char *str, long len)
{
	if (GetDatabaseEncoding() == PG_UTF8)
		return rb_utf8_str_new(str, len);
	return rb_str_new(str, len);
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
	AttInMetadata *attinmeta;
	char	  **values;
	int			i;

	tmpcxt = AllocSetContextCreate(CurTransactionContext,
								   "plruby htup_from_value",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	values = (char **) palloc0(tupdesc->natts * sizeof(char *));

	if (RB_TYPE_P(val, T_HASH))
	{
		bool		allempty = true;

		for (i = 0; i < tupdesc->natts; i++)
		{
			char	   *key = SPI_fname(tupdesc, i + 1);
			VALUE		el = rb_hash_lookup2(val, rb_str_new_cstr(key), Qundef);

			if (el == Qundef)
				values[i] = NULL;
			else
			{
				allempty = false;
				values[i] = plruby_value_to_cstring(el, true, true);
			}
			pfree(key);
		}

		/* No attribute names matched: fall back to the hash's values in order */
		if (allempty)
		{
			VALUE		vals = rb_funcall(val, rb_intern("values"), 0);
			long		n = RARRAY_LEN(vals);

			for (i = 0; i < tupdesc->natts && i < n; i++)
			{
				VALUE		el = rb_ary_entry(vals, i);

				values[i] = plruby_value_to_cstring(el, true, true);
			}
		}
	}
	else if (RB_TYPE_P(val, T_ARRAY))
	{
		long		n = RARRAY_LEN(val);

		for (i = 0; i < tupdesc->natts && i < n; i++)
			values[i] = plruby_value_to_cstring(rb_ary_entry(val, i), true, true);
	}
	else
	{
		/* a scalar: only sensible for a single-column result */
		values[0] = plruby_value_to_cstring(val, true, true);
	}

	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	MemoryContextSwitchTo(oldcxt);
	ret = BuildTupleFromCStrings(attinmeta, values);

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
