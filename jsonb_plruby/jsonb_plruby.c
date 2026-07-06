/**********************************************************************
 * jsonb_plruby.c - TRANSFORM between jsonb and Ruby data for PL/Ruby.
 *
 * jsonb_to_plruby (FROM SQL) converts a jsonb datum into native Ruby data:
 * objects become Hashes, arrays become Arrays, strings Strings, numbers
 * Integer or Float, booleans true/false, null nil.
 *
 * plruby_to_jsonb (TO SQL) converts Ruby data back: Hash, Array, String,
 * Symbol (as its name), any Numeric (through its text form), true/false,
 * and nil (a JSON null inside a container; a top-level nil is already an
 * SQL NULL before the transform is consulted).
 *
 * The Ruby VM is owned and initialized by plruby; these functions only run
 * inside a PL/Ruby function call, so it is always up.
 *
 * Copyright (c) 2026 ChronicallyJD.  MIT License; see LICENSE.
 **********************************************************************/

#include "postgres.h"

#include <ruby.h>
#include <ruby/encoding.h>

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(jsonb_to_plruby);
PG_FUNCTION_INFO_V1(plruby_to_jsonb);

/*
 * A Ruby String in the database encoding.  jsonb strings are stored in the
 * server encoding; tag UTF-8 databases properly and leave anything else as
 * binary (matching plruby's fallback behavior).
 */
static VALUE
jb_str_new(const char *ptr, long len)
{
	if (GetDatabaseEncoding() == PG_UTF8)
		return rb_enc_str_new(ptr, len, rb_utf8_encoding());
	return rb_str_new(ptr, len);
}

/* A PG numeric as a Ruby Integer (when integral) or Float. */
static VALUE
jb_num_from_numeric(Numeric num)
{
	char	   *str = DatumGetCString(DirectFunctionCall1(numeric_out,
														  NumericGetDatum(num)));
	VALUE		v;

	if (strpbrk(str, ".eE") == NULL)
		v = rb_cstr2inum(str, 10);
	else
		v = rb_float_new(strtod(str, NULL));
	pfree(str);
	return v;
}

static VALUE
jb_scalar_to_ruby(JsonbValue *jbv)
{
	switch (jbv->type)
	{
		case jbvNull:
			return Qnil;
		case jbvBool:
			return jbv->val.boolean ? Qtrue : Qfalse;
		case jbvNumeric:
			return jb_num_from_numeric(jbv->val.numeric);
		case jbvString:
			return jb_str_new(jbv->val.string.val, jbv->val.string.len);
		default:
			elog(ERROR, "unexpected jsonb scalar type %d", (int) jbv->type);
			return Qnil;		/* unreachable */
	}
}

static VALUE
jb_to_ruby(JsonbContainer *jbc)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken tok;
	VALUE		stack[64];		/* open containers; nesting deeper errors */
	VALUE		keys[64];		/* pending object key per open container */
	int			depth = -1;
	VALUE		result = Qnil;

	it = JsonbIteratorInit(jbc);
	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		VALUE		item = Qnil;
		bool		have_item = false;

		check_stack_depth();

		switch (tok)
		{
			case WJB_BEGIN_ARRAY:
				/* A raw scalar is wrapped in a pseudo array: no container */
				if (v.val.array.rawScalar)
					break;
				if (depth >= 63)
					elog(ERROR, "jsonb is nested too deeply");
				depth++;
				stack[depth] = rb_ary_new();
				keys[depth] = Qnil;
				break;
			case WJB_BEGIN_OBJECT:
				if (depth >= 63)
					elog(ERROR, "jsonb is nested too deeply");
				depth++;
				stack[depth] = rb_hash_new();
				keys[depth] = Qnil;
				break;
			case WJB_KEY:
				keys[depth] = jb_str_new(v.val.string.val, v.val.string.len);
				break;
			case WJB_VALUE:
			case WJB_ELEM:
				item = jb_scalar_to_ruby(&v);
				have_item = true;
				break;
			case WJB_END_ARRAY:
				if (depth < 0)	/* closing the raw-scalar pseudo array */
					break;
				/* FALLTHROUGH */
			case WJB_END_OBJECT:
				item = stack[depth];
				depth--;
				have_item = true;
				break;
			default:
				elog(ERROR, "unexpected jsonb iterator token %d", (int) tok);
		}

		if (have_item)
		{
			if (depth < 0)
				result = item;
			else if (RB_TYPE_P(stack[depth], T_HASH))
			{
				rb_hash_aset(stack[depth], keys[depth], item);
				keys[depth] = Qnil;
			}
			else
				rb_ary_push(stack[depth], item);
		}
	}

	return result;
}

Datum
jsonb_to_plruby(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);

	PG_RETURN_DATUM((Datum) jb_to_ruby(&in->root));
}

/* ---------------------------------------------------------------------
 * Ruby -> jsonb
 * ------------------------------------------------------------------- */

static JsonbValue *ruby_to_jsonb_container(JsonbParseState **ps, VALUE v);
static void ruby_to_jsonb_value(JsonbParseState **ps, VALUE v, bool is_elem);

static int
jb_hash_pair(VALUE key, VALUE val, VALUE arg)
{
	JsonbParseState **ps = (JsonbParseState **) arg;
	JsonbValue	jbk;
	VALUE		kstr = rb_obj_as_string(key);

	jbk.type = jbvString;
	jbk.val.string.len = (int) RSTRING_LEN(kstr);
	jbk.val.string.val = pnstrdup(RSTRING_PTR(kstr), RSTRING_LEN(kstr));
	pushJsonbValue(ps, WJB_KEY, &jbk);

	ruby_to_jsonb_value(ps, val, false);
	return ST_CONTINUE;
}

static void
ruby_to_jsonb_scalar(JsonbValue *jbv, VALUE v)
{
	switch (TYPE(v))
	{
		case T_NIL:
			jbv->type = jbvNull;
			break;
		case T_TRUE:
			jbv->type = jbvBool;
			jbv->val.boolean = true;
			break;
		case T_FALSE:
			jbv->type = jbvBool;
			jbv->val.boolean = false;
			break;
		case T_FLOAT:
			if (isinf(RFLOAT_VALUE(v)) || isnan(RFLOAT_VALUE(v)))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("cannot transform %s to jsonb",
								isnan(RFLOAT_VALUE(v)) ? "NaN" : "infinity")));
			/* FALLTHROUGH */
		case T_FIXNUM:
		case T_BIGNUM:
			{
				VALUE		s = rb_obj_as_string(v);
				char	   *cstr = pnstrdup(RSTRING_PTR(s), RSTRING_LEN(s));

				jbv->type = jbvNumeric;
				jbv->val.numeric =
					DatumGetNumeric(DirectFunctionCall3(numeric_in,
														CStringGetDatum(cstr),
														ObjectIdGetDatum(InvalidOid),
														Int32GetDatum(-1)));
				break;
			}
		case T_STRING:
			jbv->type = jbvString;
			jbv->val.string.len = (int) RSTRING_LEN(v);
			jbv->val.string.val = pnstrdup(RSTRING_PTR(v), RSTRING_LEN(v));
			break;
		case T_SYMBOL:
			{
				VALUE		s = rb_sym2str(v);

				jbv->type = jbvString;
				jbv->val.string.len = (int) RSTRING_LEN(s);
				jbv->val.string.val = pnstrdup(RSTRING_PTR(s), RSTRING_LEN(s));
				break;
			}
		default:
			/* Any other Numeric (BigDecimal, ...) via its text form */
			if (rb_obj_is_kind_of(v, rb_cNumeric))
			{
				VALUE		s = rb_obj_as_string(v);
				char	   *cstr = pnstrdup(RSTRING_PTR(s), RSTRING_LEN(s));

				jbv->type = jbvNumeric;
				jbv->val.numeric =
					DatumGetNumeric(DirectFunctionCall3(numeric_in,
														CStringGetDatum(cstr),
														ObjectIdGetDatum(InvalidOid),
														Int32GetDatum(-1)));
				break;
			}
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot transform Ruby object of class %s to jsonb",
							rb_obj_classname(v))));
	}
}

/* Push a Ruby Array/Hash; returns the completed tree from the closing push. */
static JsonbValue *
ruby_to_jsonb_container(JsonbParseState **ps, VALUE v)
{
	check_stack_depth();

	if (RB_TYPE_P(v, T_ARRAY))
	{
		long		i;

		pushJsonbValue(ps, WJB_BEGIN_ARRAY, NULL);
		for (i = 0; i < RARRAY_LEN(v); i++)
			ruby_to_jsonb_value(ps, rb_ary_entry(v, i), true);
		return pushJsonbValue(ps, WJB_END_ARRAY, NULL);
	}

	pushJsonbValue(ps, WJB_BEGIN_OBJECT, NULL);
	rb_hash_foreach(v, jb_hash_pair, (VALUE) ps);
	return pushJsonbValue(ps, WJB_END_OBJECT, NULL);
}

static void
ruby_to_jsonb_value(JsonbParseState **ps, VALUE v, bool is_elem)
{
	if (RB_TYPE_P(v, T_ARRAY) || RB_TYPE_P(v, T_HASH))
		(void) ruby_to_jsonb_container(ps, v);
	else
	{
		JsonbValue	jbv;

		ruby_to_jsonb_scalar(&jbv, v);
		pushJsonbValue(ps, is_elem ? WJB_ELEM : WJB_VALUE, &jbv);
	}
}

Datum
plruby_to_jsonb(PG_FUNCTION_ARGS)
{
	VALUE		v = (VALUE) PG_GETARG_DATUM(0);
	JsonbValue *out;

	if (RB_TYPE_P(v, T_ARRAY) || RB_TYPE_P(v, T_HASH))
	{
		JsonbParseState *ps = NULL;

		/* the final WJB_END_* push returns the completed tree */
		out = ruby_to_jsonb_container(&ps, v);
	}
	else
	{
		JsonbValue	jbv;

		ruby_to_jsonb_scalar(&jbv, v);
		out = palloc(sizeof(JsonbValue));
		*out = jbv;
	}

	PG_RETURN_POINTER(JsonbValueToJsonb(out));
}