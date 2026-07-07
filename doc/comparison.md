# Feature comparison: PL/Ruby vs PL/php vs PL/Perl vs PL/Tcl

This matrix compares **PL/Ruby** with the sibling PL/php handler and the two
PostgreSQL built-in procedural languages it draws its feature set from:
PL/Perl and PL/Tcl.

Legend: ✅ supported · ➖ not applicable / different mechanism · ❌ not provided.

## Language capabilities

| Capability | PL/Ruby | PL/php | PL/Perl | PL/Tcl |
|---|:---:|:---:|:---:|:---:|
| Scalar functions | ✅ | ✅ | ✅ | ✅ |
| Native argument types | ✅ Integer/Float/String/bool/nil | ➖ strings | ➖ strings | ➖ strings |
| Array arguments / return (multi-dim) | ✅ nested `Array` | ✅ | ✅ | ✅ |
| Composite / record arguments and return | ✅ `Hash` | ✅ assoc array | ✅ hashref | ✅ array/list |
| Set-returning functions | ✅ `return_next` | ✅ `return_next` | ✅ `return_next` | ✅ `return_next` |
| `RETURNS TABLE` / OUT / INOUT | ✅ | ✅ | ✅ | ✅ |
| Named parameters as variables | ✅ | ✅ | ❌ | ❌ |
| DML trigger functions | ✅ `$_TD` | ✅ `$_TD` | ✅ `$_TD` | ✅ `$TG_*`/`$NEW`/`$OLD` |
| Event trigger functions | ✅ | ✅ | ✅ | ✅ |
| Anonymous code blocks (`DO`) | ✅ | ✅ | ✅ | ✅ |
| Procedures + transaction control | ✅ `spi_commit`/`spi_rollback` | ✅ | ✅ | ✅ `commit`/`rollback` |
| Explicit subtransactions | ✅ `subtransaction { }` | ✅ `subtransaction(callable)` | ➖ (`eval`/`spi_exec` traps) | ✅ `subtransaction { }` |
| Session-shared data | ✅ `$_SHARED` | ✅ `$_SHARED` | ✅ `%_SHARED` | ✅ `GD` |
| Per-function static data | ✅ `$_SD` | ➖ | ➖ | ➖ |
| Module autoloading | ✅ `plruby_modules` | ✅ `plphp_modules` | ➖ | ✅ `pltcl_modules` + `unknown` |
| Start proc | ✅ `plruby.start_proc` | ✅ `plphp.start_proc` | ✅ `plperl.on_init` etc. | ✅ `pltcl.start_proc` |
| Standard output forwarded to server log | ✅ `puts`/`print` | ✅ | ➖ (`elog`) | ➖ (`elog`) |
| Trusted (sandboxed) variant | ❌ by design | ❌ by design | ✅ `plperl` (Safe.pm) | ✅ `pltcl` (Safe-Tcl) |

## Database access (SPI) and helpers

| Function | PL/Ruby | PL/php | PL/Perl | PL/Tcl |
|---|:---:|:---:|:---:|:---:|
| Run a query | `spi_exec(q[,limit])` | `spi_exec` | `spi_exec_query` | `spi_exec ?-count n?` |
| Iterate rows | `spi_fetch_row` to `Hash`/`nil` | `spi_fetch_row` | `spi_fetchrow` | row loop / `-array` |
| Stream via cursor | `spi_query`(+block)/`spi_fetchrow`/`spi_cursor_close`, `Cursor#each` | ➖ | `spi_query`/`spi_fetchrow`/`spi_cursor_close` | `spi_exec`/cursors |
| Rows processed / status / rewind | `spi_processed`/`spi_status`/`spi_rewind` | ✅ same | ➖ (return hash) | ➖ |
| Result column metadata | `spi_colnames`/`spi_coltypes`/`spi_coltypmods` | ➖ | ➖ | ➖ |
| Prepare a plan | `spi_prepare(q, types...)` | ✅ same | `spi_prepare` | `spi_prepare` |
| Execute a plan | `spi_exec_prepared` / `spi_query_prepared` | ✅ same | `spi_exec_prepared`/`spi_query_prepared` | `spi_execp` |
| Free a plan | `spi_freeplan` | ✅ same | `spi_freeplan` | (auto) |
| Commit / rollback | `spi_commit` / `spi_rollback` | ✅ same | ✅ | `commit`/`rollback` |
| Quote literal / nullable / ident | `quote_literal`/`quote_nullable`/`quote_ident` | ✅ same | ✅ same | `quote` |
| Log a message | `elog(level,msg)`, `pg_raise` | ✅ same | `elog(level,msg)` | `elog level msg` |

## Notes

- **Native types.** PL/Ruby converts arguments to native Ruby objects
  (`Integer`, `Float`, `true`/`false`, `String`, `nil`, nested `Array`,
  composite `Hash`), whereas PL/php, PL/Perl, and PL/Tcl pass most scalars as
  strings. `numeric` is passed as a lossless `String` in PL/Ruby.
- **Trusted variant.** PL/Perl (`Safe.pm`) and PL/Tcl (Safe-Tcl) offer sandboxed
  trusted languages for non-superusers. Neither Ruby (`$SAFE`/tainting
  removed in 3.0) nor PHP (`safe_mode` removed in 5.4) can be sandboxed,
  so PL/Ruby and PL/php are untrusted / superuser-only. See
  [Security](plruby.md#security).
- **Subtransactions.** In PL/Ruby, `subtransaction` takes a block (or a
  callable); it rolls back and re-raises on any exception, including database
  errors surfaced by `spi_exec`, which are converted to rescuable Ruby
  exceptions.

For usage details see the [language reference](plruby.md), and the focused
[PL/Perl](plperl-comparison.md) and [PL/Tcl](pltcl-comparison.md) comparisons.
