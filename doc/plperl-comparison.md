# PL/Ruby vs PL/Perl feature comparison

This compares PL/Ruby against PostgreSQL's built-in PL/Perl and records which
PL/Perl features PL/Ruby provides. A few items are intentionally out of scope,
with the rationale given.

## Language capabilities

| Capability                              | PL/Perl | PL/Ruby | Notes |
|-----------------------------------------|:-------:|:-------:|-------|
| Scalar functions                        | ✅ | ✅ | Arguments arrive as native Ruby values |
| Array arguments / return (multi-dim)    | ✅ | ✅ | Nested Ruby `Array` |
| Composite / record arguments and return | ✅ | ✅ | Ruby `Hash` (string keys) |
| Set-returning functions (`return_next`) | ✅ | ✅ | |
| `RETURNS TABLE` / OUT / INOUT arguments  | ✅ | ✅ | PL/Ruby also aliases named parameters as locals, which PL/Perl does not |
| Trigger functions (`$_TD`)              | ✅ | ✅ | Both use `$_TD` |
| Session-shared data                     | `%_SHARED` | `$_SHARED` | A Ruby `Hash` |
| Anonymous code blocks (`DO`)            | ✅ | ✅ | `DO $$ ... $$ LANGUAGE plruby` |
| Procedures with transaction control     | ✅ | ✅ | `CALL` a `PROCEDURE`; `spi_commit`/`spi_rollback` in a non-atomic context |
| `VARIADIC` functions                    | ❌ | ✅ | The variadic tail arrives as one Ruby `Array` (`VARIADIC "any"` unsupported) |
| `INSTEAD OF` / `TRUNCATE` triggers      | ✅ | ✅ | |
| `jsonb` transform (`TRANSFORM FOR TYPE`) | `jsonb_plperl` | `jsonb_plruby` | Native `Hash`/`Array` for jsonb; PL/Ruby keeps big integers exact |
| `hstore` transform                      | `hstore_plperl` | ❌ | Use `hstore_to_jsonb` + `jsonb_plruby`, or the text form |
| Trusted (sandboxed) variant             | `plperl` (Safe.pm) | ❌ by design | Ruby's `$SAFE`/tainting were removed in 3.0; PL/Ruby is untrusted/superuser-only (see [Security](plruby.md#security)) |

## Built-in functions

| PL/Perl                       | PL/Ruby | Notes |
|-------------------------------|---------|-------|
| `spi_exec_query`              | `spi_exec` | Runs a query, returns a result to iterate |
| `spi_fetchrow` / row access   | `spi_fetch_row` | Returns a `Hash`, or `nil` at end |
| (row count / status)          | `spi_processed`, `spi_status`, `spi_rewind` | |
| `elog(level, msg)`            | `elog` | DEBUG/LOG/INFO/NOTICE/WARNING/ERROR; `pg_raise` is the older, narrower spelling |
| `spi_prepare`                 | `spi_prepare` | Placeholder types given as SQL type-name strings |
| `spi_exec_prepared`           | `spi_exec_prepared` | |
| `spi_query_prepared`          | `spi_query_prepared` | Streams a prepared plan through a cursor |
| `spi_freeplan`                | `spi_freeplan` | |
| `quote_literal`               | `quote_literal` | |
| `quote_nullable`              | `quote_nullable` | |
| `quote_ident`                 | `quote_ident` | |
| `spi_commit` / `spi_rollback` | `spi_commit` / `spi_rollback` | Transaction control in a procedure invoked by `CALL` (non-atomic) |
| `looks_like_number`           | — | Use Ruby: `Float(x) rescue nil`, `x =~ /\A\d+\z/`, etc. |
| `encode_bytea` / `decode_bytea` | — | Use Ruby's `Array#pack` / `String#unpack1`, `[..].pack('H*')`, Base64, etc. |
| `encode_array_literal` / `encode_typed_literal` | — | Return a Ruby `Array` directly, or build literals with the quoting helpers |

## Intentionally not implemented

- **Trusted/sandboxed language.** PL/Perl's `plperl` uses `Safe.pm` to restrict
  operations. Modern Ruby has no equivalent (`$SAFE` and object tainting were
  removed in Ruby 3.0), so PL/Ruby is untrusted only.
- **Cursor-streaming SPI** (a portal-backed `spi_query` + row-at-a-time fetch).
  PL/Ruby's `spi_exec` materializes results and `spi_fetch_row` iterates them,
  covering the same use cases; true streaming of very large result sets without
  materializing is not provided.
- **Interpreter-init hooks / GUCs** beyond `plruby.start_proc` (e.g.
  `plperl.on_init`, `plperl.use_strict`). Session setup is done with
  `plruby_modules` and `plruby.start_proc` instead.

See [`doc/plruby.md`](plruby.md) for full usage of the functions listed above.
