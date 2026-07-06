# Changelog

All notable changes to PL/Ruby are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project aims to follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Inline `class`/`module` definitions in function bodies.** Bodies now
  compile to a top-level lambda (with a delegator method), where `class` is
  legal — it was a SyntaxError anywhere inside the previous `def`-based
  compilation. Definitions register globally for the session, exactly like
  `plruby_modules` code, and work in functions, triggers, and `DO` blocks.
- **Transforms reach nested contexts.** A `TRANSFORM FOR TYPE` declaration
  now also applies to fields of composite arguments and results, OUT
  parameters, array elements (e.g. `jsonb[]`), and `return_next` rows,
  including `RETURNS SETOF jsonb`/`hstore`. SPI results are deliberately
  unaffected. (The SRF row builder is now datum-based, which also lets
  `return_next` rows carry nested composite values.)
- **`hstore_plruby`** — a companion extension (in `hstore_plruby/`) providing
  `TRANSFORM FOR TYPE hstore`: opted-in functions receive hstore arguments as
  a Ruby `Hash` of String keys to String-or-`nil` values and may return a
  `Hash` into an hstore result (keys/values stringified, `nil` ↔ `NULL`).
  Implemented against hstore's public SQL functions, so it needs no hstore
  headers and works with the packaged hstore on PostgreSQL 11–18.

## [2.2.0] — 2026-07-05

Native jsonb, modern Ruby, and CI. The `jsonb_plruby` companion extension
exchanges jsonb with native Ruby data; RubyGems is enabled in the embedded
interpreter (restoring csv/bigdecimal/base64 on Ruby 3.4 and making
installed gems requirable); every push is now verified by GitHub Actions on
PostgreSQL 12–18 and Ruby 3.2/3.3/3.4 (PG 11 verified out-of-band). All
plruby changes are in the shared library; `ALTER EXTENSION plruby UPDATE`
completes the upgrade after installing the new binary.

### Added

- **`jsonb_plruby`** — a companion extension (in `jsonb_plruby/`) providing
  `TRANSFORM FOR TYPE jsonb`: opted-in functions receive jsonb arguments as
  native Ruby data (`Hash`/`Array`/`String`/`Integer`/`Float`/booleans/`nil`)
  and return Ruby data into jsonb directly. Integers beyond `Float` precision
  and `BigDecimal` values serialize exactly.
- **Structured `pg_raise`** — `pg_raise(level, message, detail:, hint:,
  sqlstate:)` maps the keywords onto the corresponding `ereport` fields (like
  PL/pgSQL's `RAISE ... USING`); a PL/Ruby caller that rescues the resulting
  error reads them back via `PLRuby::Error#detail` / `#hint` / `#sqlstate`.
- **`PLRuby::Error#detail` / `#hint`** — a caught database error now carries
  its `DETAIL` and `HINT` alongside the SQLSTATE.
- **Streaming `spi_query_prepared`** — executes a prepared plan through a
  cursor (block form and `PLRuby::Cursor` handle form) instead of
  materializing; the plan stays reusable after the cursor closes. It was
  previously an alias of `spi_exec_prepared`.
- **Regression suite 35 → 37 tests** (`replace`: mid-session
  `CREATE OR REPLACE` recompilation; extended `misc`/`sqlstate`/`prepare`),
  plus the `jsonb_plruby` suite.
- **CI (GitHub Actions)** — every push and pull request builds both
  extensions and runs both suites on PostgreSQL 12–18 (system Ruby 3.2) and
  on Ruby 3.3/3.4 (PostgreSQL 18).
- **Ruby 3.3 and 3.4 support** — alternate expected files cover their
  changed `NoMethodError`/`Hash#inspect` output; validation errors are
  reported as a single line (Ruby 3.4's parser quotes the generated wrapper
  source over many lines); the cookbook's token recipe encodes with
  `Array#pack('m0')` instead of the `base64` library.

### Changed

- **Domain arguments arrive as the base type's Ruby value** (a domain over
  `int` is an `Integer`, over `int[]` an `Array`, ...); previously they
  arrived as the text-form String.
- **RubyGems is enabled in the embedded interpreter** (was `--disable-gems`):
  Ruby 3.4 ships `csv`, `bigdecimal`, and `base64` as bundled gems, which
  plain `require` cannot see without it — and installed gems become
  requirable as a side benefit. `did_you_mean`/`error_highlight` are disabled
  so error messages stay deterministic. Verified on Ruby 3.2, 3.3, and 3.4
  (CI covers all three).

## [2.1.0] — 2026-07-05

Feature and hardening release: broader trigger and argument-mode coverage,
richer error objects, streaming SPI, a tested cookbook, and a regression
suite grown to 35 tests verified on PostgreSQL 11 through 18. All changes
are in the shared library; `ALTER EXTENSION plruby UPDATE` completes the
upgrade after installing the new binary.

### Added

- **Expanded regression suite** — 20 → 28 tests, verified on **PostgreSQL 11,
  12, 13, 14, 16, and 18** with Ruby 3.2. New coverage: `bytea` conversion,
  special numeric/float values (`NaN`, `±Infinity`, integer overflow), nested
  and array-bearing composites, SPI DML status codes and NULL columns,
  DELETE-trigger `$_TD['old']` handling, the error/`ensure` model, prepared-plan
  reuse, standard-library `require`, `$stdout`/`$stderr` redirection, quoting
  helpers, and `plruby.start_proc`.

- **Cursor streaming** — `spi_query(sql)` opens a portal and reads rows a batch
  at a time, so large results stream without materializing. Block form
  (`spi_query(sql) { |row| … }`), handle form (`spi_fetchrow` /
  `spi_cursor_close`), and `PLRuby::Cursor#each` (Enumerable).
- **Broader string encoding** — text is tagged with the Ruby encoding matching
  the database encoding (LATIN*, WIN*, EUC*, SJIS, KOI8, Big5, GBK, …), not just
  UTF-8; unmapped encodings fall back to ASCII-8BIT.
- **`PLRuby::Error#sqlstate`** — a caught PostgreSQL error now carries its
  five-character `SQLSTATE` (e.g. `42P01`, `22012`) on the Ruby exception; it
  is `nil` on a PL/Ruby error not backed by a database error.
- **`VARIADIC` arguments** — the variadic tail arrives as a single Ruby
  `Array` argument (`VARIADIC "any"` remains unsupported).
- **`INSTEAD OF` triggers** on views — `$_TD['when']` is `INSTEAD OF`, with
  the same `nil`/`'SKIP'`/`'MODIFY'` return handling as BEFORE row triggers.
- **Cookbook** — `doc/cookbook.md`, practical recipes built on Ruby's stdlib
  (JSON reshaping, HMAC/PBKDF2, audit trigger, CSV, Zlib-to-bytea, BigDecimal,
  slugify, ERB, streaming scans, batch commits); every recipe in the "tested"
  section runs verbatim in the regression suite (`cookbook` test).
- **Regression suite 28 → 35 tests**, adding `datetime` (date/time/timestamp/
  interval), `jsonb`, `misc` (uuid/inet/enum/domain), `variadic`, `trigger2`
  (INSTEAD OF, `WHEN` clauses, deferred constraint triggers, composite-column
  `'MODIFY'`), `hostile` (mid-SRF errors, cursor misuse, ~1MB TOAST values,
  prepared-plan edge cases), and `cookbook`.

### Fixed

- **Nested composites** are now built and read recursively: a composite field
  whose value is a `Hash` becomes a sub-record (to any depth), and a function
  may return an array of composite. A nested composite argument arrives as a
  nested `Hash`. (The tuple builder in `plruby_io.c` is now datum-based.)
- **`TRUNCATE` triggers** are now dispatched: `$_TD['event']` is `TRUNCATE`,
  statement-level, return value ignored.
- **`spi_freeplan`** now invalidates the plan handle; reusing a freed plan
  raises a clear PL/Ruby error instead of silently failing.
- **Trigger `'MODIFY'` with composite columns** — the modified-tuple path is
  now datum-based, so assigning a `Hash` to a composite-typed field of
  `$_TD['new']` works (it previously failed with "malformed record literal").
- **`spi_fetchrow` after `spi_cursor_close`** now returns `nil`; previously it
  could keep returning rows left in the cursor's prefetch buffer.

### Known limitations

- A function body compiles to a method, so a `class`/`module` definition cannot
  appear inline in the body (use a `plruby_modules` module instead).

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
