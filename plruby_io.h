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

/* Build a HeapTuple from a Ruby Hash/Array and a TupleDesc. */
extern HeapTuple plruby_htup_from_value(VALUE val, TupleDesc tupdesc);

/* Like plruby_htup_from_value but positional, for SRFs, reusing a context. */
extern HeapTuple plruby_srf_htup_from_value(VALUE val, AttInMetadata *attinmeta,
											 MemoryContext cxt);

/* Rebuild the NEW tuple of a BEFORE trigger from $_TD['new']. */
extern HeapTuple plruby_modify_tuple(VALUE td, TriggerData *tdata);

#endif							/* PLRUBY_IO_H */
