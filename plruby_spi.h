/*
 * plruby_spi.h - Ruby-callable SPI/database API and interpreter setup.
 */
#ifndef PLRUBY_SPI_H
#define PLRUBY_SPI_H

#include "postgres.h"
#include "executor/spi.h"
#include "funcapi.h"

#include <ruby.h>

/*
 * Register the global (Kernel) functions PL/Ruby exposes -- spi_exec, elog,
 * quote_literal, return_next, ... -- plus $_SHARED and $stdout/$stderr
 * redirection.  Called once from plruby_init().
 */
extern void plruby_spi_init(void);

/*
 * SRF support: state the running set-returning function and return_next()
 * share, mirroring PL/php.
 */
extern FunctionCallInfo current_fcinfo;
extern TupleDesc current_tupledesc;
extern AttInMetadata *current_attinmeta;
extern MemoryContext current_memcxt;
extern Tuplestorestate *current_tuplestore;
extern VALUE current_srf_binding;	/* Binding for a no-arg return_next() */

#endif							/* PLRUBY_SPI_H */
