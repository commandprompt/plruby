# PL/Ruby vs PL/Tcl feature comparison

This compares PL/Ruby against PostgreSQL's built-in PL/Tcl. The one remaining
gap is noted with its rationale.

## Language capabilities

| Capability                              | PL/Tcl | PL/Ruby | Notes |
|-----------------------------------------|:------:|:-------:|-------|
| Scalar / composite / array functions    | ✅ | ✅ | |
| Set-returning functions                 | ✅ | ✅ | `return_next` |
| DML trigger functions                   | ✅ | ✅ | `$_TD` in PL/Ruby; `$TG_*`/`$NEW`/`$OLD` in PL/Tcl |
| Event trigger functions                 | ✅ | ✅ | `RETURNS event_trigger`; `$_TD['event']`, `$_TD['tag']` |
| Session-shared data                     | `GD` array | `$_SHARED` | A Ruby `Hash` |
| Transaction control in procedures       | `commit`/`rollback` | `spi_commit`/`spi_rollback` | |
| Explicit subtransactions                | `subtransaction {...}` | `subtransaction { ... }` | PL/Ruby takes a block (or a callable) |
| Module autoloading                      | `pltcl_modules` + `unknown` | `plruby_modules` | PL/Ruby eager-loads all module rows at interpreter init |
| Start proc                              | `pltcl.start_proc` | `plruby.start_proc` | |
| Trusted (sandboxed) variant             | `pltcl` (Safe-Tcl) | ❌ by design | see below |

## Built-in commands / functions

| PL/Tcl                        | PL/Ruby | Notes |
|-------------------------------|---------|-------|
| `spi_exec ?-count n? cmd`     | `spi_exec(cmd [, limit])` | |
| `spi_prepare` / `spi_execp`   | `spi_prepare` / `spi_exec_prepared` | |
| row iteration (`-array`/loop) | `spi_fetch_row` loop | Idiomatic Ruby: `while (row = spi_fetch_row(r))` |
| `quote`                       | `quote_literal` (+ `quote_nullable`, `quote_ident`) | |
| `elog level msg`              | `elog(level, msg)` | DEBUG/LOG/INFO/NOTICE/WARNING/ERROR |
| `subtransaction { body }`     | `subtransaction { body }` | Also `subtransaction(callable, ...)` |
| `commit` / `rollback`         | `spi_commit` / `spi_rollback` | |

## The one remaining gap

- **Trusted / sandboxed language.** PL/Tcl's `pltcl` runs untrusted user code in
  a Safe-Tcl interpreter that restricts filesystem, network, and process access,
  so it is safe for non-superusers. Ruby has no comparable sandbox
  (`$SAFE` and object tainting were removed in Ruby 3.0), so PL/Ruby is
  untrusted and superuser-only (see [Security](plruby.md#security)). This is the
  only PL/Tcl capability PL/Ruby cannot practically match.

## Notes on the capabilities

- **Event triggers** dispatch through the same call handler; the function is
  declared `RETURNS event_trigger` and receives `$_TD` with `event` and `tag`.
- **Subtransactions** roll back on any exception raised by the block: a Ruby
  `raise`, or a database error surfaced by `spi_exec` (which PL/Ruby converts to
  a rescuable Ruby exception). See [Subtransactions](plruby.md#subtransactions).
- **Modules** are loaded by evaluating each row's source at the top level, so
  method and class definitions register globally for the session.
- **start_proc** runs during the first PL/Ruby call of a session.

See [`doc/plruby.md`](plruby.md) for full usage, and
[`doc/plperl-comparison.md`](plperl-comparison.md) for the PL/Perl comparison.
