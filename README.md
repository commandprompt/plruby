<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="doc/assets/plruby-logo-dark.svg">
  <img src="doc/assets/plruby-logo.svg" alt="PL/Ruby — Procedural Language for PostgreSQL" width="440">
</picture>

[![CI](https://github.com/commandprompt/plruby/actions/workflows/ci.yml/badge.svg)](https://github.com/commandprompt/plruby/actions/workflows/ci.yml)
[![PostgreSQL](https://img.shields.io/badge/PostgreSQL-11_to_18-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![Ruby](https://img.shields.io/badge/Ruby-3.x-CC342D?logo=ruby&logoColor=white)](https://www.ruby-lang.org/)
[![Tests](https://img.shields.io/badge/tests-44_passing-brightgreen)](sql/)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

PL/Ruby is a procedural-language handler that lets you write database functions
in **Ruby**, stored and executed inside PostgreSQL. You get the expressiveness
of Ruby and its standard library with the full power of a native PostgreSQL
function: plain functions, set-returning functions, triggers, event triggers,
and procedures with transaction control.

```sql
CREATE EXTENSION plruby;

CREATE FUNCTION hello(text) RETURNS text LANGUAGE plruby AS $$
    "Hello, #{args[0]}!"
$$;

SELECT hello('world');   -- Hello, world!
```

> [!NOTE]
> PL/Ruby embeds an MRI Ruby interpreter in the backend. It targets **PostgreSQL
> 11-18** and **Ruby 3.x**, installs as a first-class `CREATE EXTENSION`, and
> mirrors the feature set of PL/php with a large set of PL/Perl- and
> PL/Tcl-inspired capabilities.

---

## Features

| | |
|---|---|
| 🧩 **Scalars, arrays, composites** | Arguments arrive as native Ruby values: `Integer`, `Float`, `true`/`false`, `String`, nested `Array`, and composite/record types as `Hash`. |
| 🔁 **Set-returning functions** | `RETURNS SETOF` / `RETURNS TABLE` with `return_next`. |
| ⚡ **Triggers** | Row & statement triggers via `$_TD` (with `'SKIP'` / `'MODIFY'`). |
| 📣 **Event triggers** | Back `CREATE EVENT TRIGGER` with `RETURNS event_trigger`. |
| 🗄️ **Database access (SPI)** | `spi_exec`, `spi_fetch_row`, `spi_processed`, `spi_status`, `spi_rewind`, and result column metadata (`spi_colnames` / `spi_coltypes` / `spi_coltypmods`). |
| 🌊 **Cursor streaming** | `spi_query` (block or handle), `spi_fetchrow`, `spi_cursor_close`, `Cursor#each`. Consume large results without materializing them. |
| 📝 **Prepared statements** | `spi_prepare` / `spi_exec_prepared` / `spi_query_prepared` / `spi_freeplan`. |
| 🔐 **Transaction control** | `spi_commit` / `spi_rollback` in procedures, plus `subtransaction` blocks. |
| 🧰 **Utilities** | `quote_literal` / `quote_nullable` / `quote_ident`, `elog`, session-shared `$_SHARED`, and per-function `$_SD`. |
| 📦 **Session setup** | Anonymous `DO` blocks, `plruby_modules` autoloading, and a `plruby.start_proc` hook. |
| 🔄 **Transforms** | `jsonb_plruby`, `hstore_plruby`, and `ltree_plruby`: functions declared `TRANSFORM FOR TYPE` exchange native Ruby Hashes/Arrays with `jsonb`, `hstore`, and `ltree`. |

See the [**language reference**](doc/plruby.md) for the full API, the
[**cookbook**](doc/cookbook.md) for tested recipes, and the
[PL/Perl](doc/plperl-comparison.md) and [PL/Tcl](doc/pltcl-comparison.md)
comparisons for feature-by-feature detail.

## Examples

**A set-returning function**

```sql
CREATE FUNCTION squares(lim integer)
RETURNS TABLE(n integer, square integer) LANGUAGE plruby AS $$
    (1..lim).each do |i|
        n = i
        square = i * i
        return_next
    end
$$;

SELECT * FROM squares(3);   -- (1,1), (2,4), (3,9)
```

**Querying the database with a prepared plan**

```sql
CREATE FUNCTION lookup(int) RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select name from things where id = $1', 'int4')
    row  = spi_fetch_row(spi_exec_prepared(plan, args[0]))
    spi_freeplan(plan)
    row['name']
$$;
```

**A row trigger that transforms data**

```sql
CREATE FUNCTION uppercase_name() RETURNS trigger LANGUAGE plruby AS $$
    $_TD['new']['name'] = $_TD['new']['name'].upcase
    'MODIFY'
$$;
```

## Requirements

- **PostgreSQL 11 or newer** (tested on 11-18; 18 recommended), with the server
  development files that provide `pg_config`.
- **Ruby 3.x** built as a shared library (`ENABLE_SHARED=yes`) with development
  headers. On Debian/Ubuntu, install `ruby-dev`.

## Installation

```sh
make
sudo make install
```

Then, in a database:

```sql
CREATE EXTENSION plruby;
```

See [**INSTALL**](INSTALL) for details, and run the regression suite with
`make installcheck`.

## Security

> [!WARNING]
> **PL/Ruby is an untrusted language.** Ruby 3.0 and later have no sandbox
> (`$SAFE` and object tainting were removed in Ruby 3.0), so a PL/Ruby function
> can do anything the PostgreSQL server's operating-system user can: read and
> write files, open network connections, run shell commands, and so on.

The language is created **without** the `TRUSTED` attribute, so only superusers
can install the extension or create PL/Ruby functions. Grant that ability only
to roles you would trust with the server's OS account.

## Documentation

- [Language reference](doc/plruby.md)
- [Cookbook: tested recipes](doc/cookbook.md)
- [Installation](INSTALL)
- [Changelog](CHANGELOG.md)
- [Feature comparison: PL/Ruby vs PL/php vs PL/Perl vs PL/Tcl](doc/comparison.md)
- [PL/Ruby vs PL/Perl](doc/plperl-comparison.md)
- [PL/Ruby vs PL/Tcl](doc/pltcl-comparison.md)
- [Why PL/Ruby embeds CRuby (MRI), not mruby](doc/why-not-mruby.md)

## License

PL/Ruby is licensed under the **MIT License**; see [LICENSE](LICENSE).
