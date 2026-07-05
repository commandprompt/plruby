# Changelog

All notable changes to PL/Ruby are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project aims to follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Expanded regression suite** — 20 → 28 tests, verified on **PostgreSQL 11,
  12, 13, 14, 16, and 18** with Ruby 3.2. New coverage: `bytea` conversion,
  special numeric/float values (`NaN`, `±Infinity`, integer overflow), nested
  and array-bearing composites, SPI DML status codes and NULL columns,
  DELETE-trigger `$_TD['old']` handling, the error/`ensure` model, prepared-plan
  reuse, standard-library `require`, `$stdout`/`$stderr` redirection, quoting
  helpers, and `plruby.start_proc`.

### Documented (known limitations surfaced by the new tests)

- *Nested* composites are not built recursively: a composite field whose value
  is a `Hash`, and an array-of-composite return, are rejected with "malformed
  record literal". Flat composites with array fields work.
- Statement-level `TRUNCATE` triggers are not dispatched ("unknown firing event
  for trigger function").
- A caught PostgreSQL error surfaces only its message to Ruby; the original
  `SQLSTATE` is not exposed on the exception object.

## [2.0.0] — 2026-07-01

The initial release of the modernized PL/Ruby: an MRI Ruby interpreter embedded
in PostgreSQL, packaged as a `CREATE EXTENSION` and offering the same feature
set as PL/php. Tested on **PostgreSQL 11 through 18** with **Ruby 3.2** (shared
library, non-thread-safe embedding).

### Added

- **Functions** with native Ruby argument and return types — `Integer`,
  `Float`, `true`/`false`, `String`, `nil`, nested `Array` (multidimensional
  PostgreSQL arrays), and composite/record types as `Hash`.
- **Set-returning functions** — `RETURNS SETOF` and `RETURNS TABLE(...)` with
  `return_next` (both explicit-value and no-argument forms, the latter reading
  the TABLE/OUT column locals from the running body's binding).
- **Argument modes** — `IN`, `OUT`, `INOUT`, `TABLE`, and named parameters
  aliased as local variables.
- **Trigger functions** — `RETURNS trigger` with the `$_TD` hash and
  `nil`/`'SKIP'`/`'MODIFY'` return semantics.
- **Event trigger functions** — `RETURNS event_trigger`, with `$_TD['event']`
  and `$_TD['tag']`.
- **Anonymous `DO` blocks** — `DO $$ ... $$ LANGUAGE plruby`.
- **Database access (SPI)** — `spi_exec`, `spi_fetch_row`, `spi_processed`,
  `spi_status`, `spi_rewind`.
- **Prepared statements** — `spi_prepare`, `spi_exec_prepared`,
  `spi_query_prepared`, and `spi_freeplan`.
- **Transaction control** in procedures — `spi_commit` and `spi_rollback`.
- **Explicit subtransactions** — `subtransaction { ... }` and
  `subtransaction(callable, ...)`, rolling back and re-raising on any exception.
- **Quoting helpers** — `quote_literal`, `quote_nullable`, `quote_ident`.
- **`elog(level, message)`** supporting `DEBUG`/`LOG`/`INFO`/`NOTICE`/`WARNING`/`ERROR`,
  and the narrower `pg_raise(level, message)`.
- **`$_SHARED`** — a hash persisting across calls within a session; standard
  output (`puts`, `print`) is forwarded to the server log.
- **Session initialization** — module autoloading from a `plruby_modules` table
  and a `plruby.start_proc` configuration setting.
- **UTF-8 string handling** — text from the database is tagged with the
  database encoding (UTF-8 when the database is UTF-8), so multibyte Ruby String
  operations (`length`, `reverse`, regexp, ...) behave correctly.
- Packaging as a first-class extension (`CREATE EXTENSION plruby`) and a
  regression suite of 20 tests covering every feature, including dedicated
  type-mapping (`types`) and error-handling (`errors`) unit tests.
- Documentation: a language reference (`doc/plruby.md`), a combined
  PL/Ruby vs PL/php vs PL/Perl vs PL/Tcl feature matrix (`doc/comparison.md`),
  and the individual PL/Perl and PL/Tcl comparisons.

### License

- Released under the **MIT License** (see `LICENSE`).

### Security

- Ruby's `$SAFE` levels and object tainting were removed in Ruby 3.0, so PL/Ruby
  cannot be sandboxed. It is an **untrusted, superuser-only** language, created
  without the `TRUSTED` attribute; only superusers may install the extension or
  create PL/Ruby functions.
