/*
 * plruby.h - shared declarations for the PL/Ruby interpreter glue.
 */
#ifndef PLRUBY_H
#define PLRUBY_H

#include "postgres.h"

/* Ruby's headers.  Include after postgres.h. */
#include <ruby.h>

/*
 * Interpreter-wide Ruby objects, created once in plruby_init() and kept alive
 * with rb_gc_register_address().
 */
extern VALUE rb_mPLRuby;			/* the PLRuby module */
extern VALUE rb_ePLRubyError;		/* PLRuby::Error < StandardError */
extern VALUE plruby_recv;			/* receiver object for compiled methods */
extern VALUE plruby_proc_cache;		/* Hash: internal name (String) -> "%p" */

/*
 * Call a Ruby method under rb_protect.  On an unhandled Ruby exception this
 * reports it as a PostgreSQL ERROR (via elog) and does not return.
 */
extern VALUE plruby_protected_call(VALUE recv, ID mid, int argc, VALUE *argv);

/*
 * Evaluate a string of Ruby source under protection.  On failure, raise a
 * PostgreSQL ERROR whose message incorporates the Ruby exception and the
 * supplied context prefix (e.g. "unable to compile function \"foo\"").
 */
extern void plruby_eval_string(const char *source, const char *errctx);

/*
 * If a Ruby exception is pending, return its message palloc'd in the current
 * context and clear it; otherwise return NULL.
 */
extern char *plruby_pop_exception_message(void);

/* Convert a pending-exception state into an elog(ERROR); never returns. */
extern void plruby_report_exception(void);

#endif							/* PLRUBY_H */
