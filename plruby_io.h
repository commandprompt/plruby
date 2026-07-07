/*
 * plruby_io.h - conversions between PostgreSQL Datums and Ruby VALUEs.
 */
#ifndef PLRUBY_IO_H
#define PLRUBY_IO_H

#include "postgres.h"
#include "access/htup.h"
#include "commands/trigger.h"
#include "funcapi.h"

#include <ruby.h>

/*
 * TRANSFORM FOR TYPE support.  A function's resolved transform list (typid +
 * FromSQL/ToSQL function OIDs).  The call handler points
 * plruby_call_transforms at the running function's list for the duration of
 * the call: TO-SQL conversions (plruby_datum_from_value and friends) consult
 * it wherever they run, including return_next during the body.  The FROM-SQL
 * list, plruby_arg_transforms, is only set while the argument array is being
 * built, so SPI/cursor/trigger row reads are never affected.
 */
typedef struct plruby_transform_info
{
	Oid			typid;
	Oid			fromsql;		/* datum -> VALUE, or InvalidOid */
	Oid			tosql;			/* VALUE -> datum, or InvalidOid */
} plruby_transform_info;

extern plruby_transform_info *plruby_call_transforms;
extern int	plruby_call_ntransforms;
extern plruby_transform_info *plruby_arg_transforms;
extern int	plruby_arg_ntransforms;

/* The ToSQL / FromSQL function for a type, or InvalidOid. */
extern Oid	plruby_transform_tosql(Oid typid);
extern Oid	plruby_transform_fromsql(Oid typid);

/*
 * An array datum -> nested Ruby Array.  A valid fromsql_fn converts each
 * element through that FromSQL transform; InvalidOid means convert per
 * plruby_binary_from_datum (bytea[] elements as binary Strings).
 */
extern VALUE plruby_array_from_datum(Datum d, Oid elemtype, Oid fromsql_fn);

/*
 * bytea Datum -> binary (ASCII-8BIT) Ruby String of its raw bytes, or Qundef
 * if typeoid is not bytea (caller then uses the normal text path).
 */
extern VALUE plruby_binary_from_datum(Datum value, Oid typeoid);

/* Build a Ruby String from PG text tagged with the database encoding. */
extern VALUE plruby_str_from_pg(const char *str, long len);

/*
 * Scalar leaf conversion: turn a C string (the text output of typeoid's output
 * function) into a native Ruby value -- Integer, Float, true/false, or String.
 */
extern VALUE plruby_scalar_from_cstring(const char *str, Oid typeoid);

/* Build a Ruby Hash (string keys, typed values) from a tuple. */
extern VALUE plruby_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc);

/* Convert a PostgreSQL array output string into a (possibly nested) Ruby Array. */
extern VALUE plruby_array_from_pg(char *input, Oid elemtype);

/* Convert a Ruby Array into a PostgreSQL array input literal (palloc'd). */
extern char *plruby_array_to_pg(VALUE array);

/*
 * Turn a Ruby VALUE into a C string for feeding a type input function.
 * nil -> NULL when null_ok, else error; Array -> array literal when do_array.
 * The result (if non-NULL) is palloc'd in the current context.
 */
extern char *plruby_value_to_cstring(VALUE val, bool do_array, bool null_ok);

/*
 * Convert a Ruby VALUE into a Datum of the given type, recursively -- handles
 * scalars, arrays (including multidimensional and array-of-composite), and
 * composite types (Hash/Array -> record, nested to any depth).  Sets *isnull.
 */
extern Datum plruby_datum_from_value(VALUE val, Oid typeoid, int32 typmod,
									 bool *isnull);

/* Build a HeapTuple from a Ruby Hash/Array and a TupleDesc. */
extern HeapTuple plruby_htup_from_value(VALUE val, TupleDesc tupdesc);

/* Like plruby_htup_from_value but positional, for SRFs, reusing a context. */
extern HeapTuple plruby_srf_htup_from_value(VALUE val, AttInMetadata *attinmeta,
											 MemoryContext cxt);

/* Rebuild the NEW tuple of a BEFORE trigger from $_TD['new']. */
extern HeapTuple plruby_modify_tuple(VALUE td, TriggerData *tdata);

#endif							/* PLRUBY_IO_H */
