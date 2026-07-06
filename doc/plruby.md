# PL/Ruby Language Reference

PL/Ruby lets you write PostgreSQL functions and triggers in Ruby. This document
describes the programming interface. For build and install instructions see
[`INSTALL`](../INSTALL); for a feature summary see [`README`](../README.md).

Tested on **PostgreSQL 11 through 18** with **Ruby 3.2** (shared library,
non-thread-safe embedding).

- [Enabling the language](#enabling-the-language)
- [Writing functions](#writing-functions)
- [Anonymous code blocks (DO)](#anonymous-code-blocks-do)
- [Data type mapping](#data-type-mapping)
- [Composite types and records](#composite-types-and-records)
- [Arguments: IN, OUT, INOUT, TABLE, named](#arguments)
- [Set-returning functions](#set-returning-functions)
- [Trigger functions](#trigger-functions)
- [Event trigger functions](#event-trigger-functions)
- [Database access (SPI)](#database-access-spi)
- [Prepared statements](#prepared-statements)
- [Transaction control](#transaction-control)
- [Subtransactions](#subtransactions)
- [Quoting helpers](#quoting-helpers)
- [Messaging: elog and pg_raise](#messaging-elog-and-pg_raise)
- [Shared data: `$_SHARED`](#shared-data-_shared)
- [Session initialization: modules and start_proc](#session-initialization-modules-and-start_proc)
- [Errors and exceptions](#errors-and-exceptions)
- [Security](#security)

## Enabling the language

```sql
CREATE EXTENSION plruby;
```

PL/Ruby is an untrusted language, so the extension is superuser-only to install
and only superusers can create PL/Ruby functions. See [Security](#security).

## Writing functions

The body of a `LANGUAGE plruby` function is a Ruby method body. Inside it you
have:

- `args` — a 0-indexed Array of the call arguments.
- `argc` — the number of declared arguments.
- The function returns the value of its **last expression** (or an explicit
  `return`).

```sql
CREATE FUNCTION r_max(integer, integer) RETURNS integer
STRICT LANGUAGE plruby AS $$
    args[0] > args[1] ? args[0] : args[1]
$$;

SELECT r_max(3, 7);   -- 7
```

`STRICT` (a.k.a. `RETURNS NULL ON NULL INPUT`) makes the function return NULL
automatically when any argument is NULL. Without it, a NULL argument arrives as
`nil`, so test with `args[n].nil?`.

## Anonymous code blocks (DO)

You can run a one-off block of Ruby without defining a function, using `DO`:

```sql
DO $$
    r = spi_exec('select count(*) as n from pg_class')
    elog('NOTICE', "pg_class has #{spi_fetch_row(r)['n']} rows")
$$ LANGUAGE plruby;
```

A `DO` block takes no arguments and returns nothing; use it for procedural
one-offs, migrations, or ad-hoc maintenance. Because PL/Ruby is untrusted, only
superusers can run `DO ... LANGUAGE plruby`.

## Data type mapping

Arguments are converted from their PostgreSQL representation to native Ruby
values; return values are converted back using the declared return type's input
function.

| PostgreSQL                     | Ruby value in `args`      | Ruby return value          |
|--------------------------------|---------------------------|----------------------------|
| smallint / integer / bigint / oid | `Integer`              | `Integer` (or any `to_s`-able) |
| real / double precision        | `Float`                   | `Float`                    |
| numeric                        | `String` (lossless)       | `Numeric` or `String`      |
| boolean                        | `true` / `false`          | `true` / `false`           |
| text / varchar / etc.          | `String`                  | `String`                   |
| arrays (e.g. `int[]`)          | nested `Array`            | nested `Array`             |
| composite / row / record       | `Hash` (string keys)      | `Hash` (or positional `Array`) |
| NULL                           | `nil`                     | `nil`                      |

Arrays map naturally, including multidimensional arrays:

```sql
CREATE FUNCTION an_array() RETURNS int[] LANGUAGE plruby AS $$
    [[1, 3, 5], [2, 4, 6]]
$$;
SELECT an_array();   -- {{1,3,5},{2,4,6}}
```

Strings arrive tagged with the Ruby encoding that matches the database encoding
(`UTF8` → `UTF-8`, `LATIN1` → `ISO-8859-1`, `EUC_JP` → `EUC-JP`, `WIN1251` →
`Windows-1251`, and so on), so `length`, `reverse`, and regexps operate on
characters, not bytes. Encodings Ruby does not recognize fall back to
`ASCII-8BIT` (binary), which is byte-preserving. `bytea` is passed as its
textual `\x…` hex representation (a `String`), not raw bytes.

## Composite types and records

A composite argument arrives as a `Hash` keyed by column name (string keys); a
composite/record return value is built the same way.

```sql
CREATE TYPE tuple AS (name text, value integer);

CREATE FUNCTION make_tuple(text, integer) RETURNS tuple LANGUAGE plruby AS $$
    {'name' => args[0], 'value' => args[1]}
$$;

SELECT * FROM make_tuple('answer', 42);   -- answer | 42
```

Functions declared `RETURNS record` must be called with a column definition
list, e.g. `SELECT * FROM f() AS (a int, b text)`.

A composite may contain array fields and other composites: a composite field
whose value is a `Hash` is built recursively as a sub-record (to any depth), and
a function may return an array of composite (a Ruby `Array` of `Hash`es). In the
read direction a nested composite argument arrives as a nested `Hash`. A missing
`Hash` key and an explicit `nil` both become SQL `NULL`.

```sql
CREATE TYPE inner_t AS (a int, b text);
CREATE TYPE outer_t AS (id int, child inner_t);

CREATE FUNCTION nest() RETURNS outer_t LANGUAGE plruby AS $$
    {'id' => 1, 'child' => {'a' => 2, 'b' => 'deep'}}
$$;
SELECT nest();   -- (1,"(2,deep)")
```

### The jsonb transform

By default `json`/`jsonb` travel as Strings of JSON text. The companion
extension **`jsonb_plruby`** provides a `TRANSFORM FOR TYPE jsonb`: a function
that opts in receives jsonb arguments as native Ruby data (`Hash`, `Array`,
`String`, `Integer`, `Float`, `true`/`false`, `nil`) and may return Ruby data
into a `jsonb` result — no `JSON.parse`/`JSON.generate` round-trip, and
integers beyond `Float` precision survive exactly:

```sql
CREATE EXTENSION jsonb_plruby;   -- requires plruby

CREATE FUNCTION redact(doc jsonb, key text) RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    redact = lambda { |v|
        case v
        when Hash  then v.to_h { |k, e| [k, k == key ? '[redacted]' : redact.call(e)] }
        when Array then v.map { |e| redact.call(e) }
        else v
        end
    }
    redact.call(doc)
$$;
```

Returned Hash keys and Symbols become JSON strings; any other `Numeric`
(e.g. `BigDecimal`) serializes losslessly through its text form. Unsupported
objects (and non-finite Floats) are rejected with a clear error. The
transform applies to top-level arguments and return values; jsonb inside a
composite, OUT parameters, and `return_next` rows still use the String form.

## Arguments

PL/Ruby supports the full range of argument modes.

- **IN** (the default) arguments appear in `args`.
- **OUT** / **INOUT** arguments: assign the result to a local variable named
  after the argument. A single OUT argument becomes the scalar result; multiple
  OUT arguments become the result row.
- **TABLE(...)** columns behave like OUT arguments for a set-returning function.
- **VARIADIC** tails arrive as a single `Array` argument (the caller's extra
  arguments are folded into it, exactly as SQL sees them). `VARIADIC "any"` is
  not supported.
- **Named** parameters are also available as local variables named after the
  argument, in addition to positional `args`.

```sql
CREATE FUNCTION add_sub(a integer, b integer, OUT sum integer, OUT diff integer)
LANGUAGE plruby AS $$
    sum  = a + b
    diff = a - b
$$;

SELECT * FROM add_sub(10, 4);   -- sum=14, diff=6
```

Named arguments are aliased as locals only when the name is a valid Ruby
local-variable name (lowercase/underscore start). Others remain reachable
positionally through `args`.

## Set-returning functions

Declare the function `RETURNS SETOF ...` (or `RETURNS TABLE(...)`) and call
`return_next` once per output row.

```sql
CREATE FUNCTION squares(lim integer)
RETURNS TABLE(n integer, square integer) LANGUAGE plruby AS $$
    (1..lim).each do |i|
        n = i
        square = i * i
        return_next               # emit current n, square
    end
$$;

SELECT * FROM squares(3);         -- (1,1), (2,4), (3,9)
```

`return_next(value)` emits an explicit row (a scalar, an `Array`, or a `Hash`
matching the result columns). `return_next` with no argument emits a row built
from the current OUT/TABLE column variables.

## Trigger functions

A trigger function is declared `RETURNS trigger`. Trigger metadata and the rows
involved are available in the `Hash` `$_TD`:

| Key                  | Meaning                                           |
|----------------------|---------------------------------------------------|
| `$_TD['name']`       | trigger name                                      |
| `$_TD['relid']`      | table OID                                         |
| `$_TD['relname']`    | table name                                        |
| `$_TD['schemaname']` | schema name                                       |
| `$_TD['when']`       | `BEFORE`, `AFTER`, or `INSTEAD OF`                |
| `$_TD['level']`      | `ROW` or `STATEMENT`                              |
| `$_TD['event']`      | `INSERT`, `UPDATE`, `DELETE`, or `TRUNCATE`       |
| `$_TD['new']`        | new row (INSERT/UPDATE), as a `Hash`              |
| `$_TD['old']`        | old row (UPDATE/DELETE), as a `Hash`              |
| `$_TD['argc']`       | number of trigger arguments                       |
| `$_TD['args']`       | trigger arguments, as an `Array`                  |

Return value of a **BEFORE** or **INSTEAD OF** ... **FOR EACH ROW** trigger:

- `nil` — proceed with the operation using the unmodified row.
- `'SKIP'` — silently skip the operation for this row.
- `'MODIFY'` — proceed using the (modified) `$_TD['new']` row. Modify fields in
  place, e.g. `$_TD['new']['col'] = 'value'`. Only valid for INSERT/UPDATE; on a
  DELETE there is no new row, so return `nil` (proceed) or `'SKIP'` instead.

On a DELETE trigger `$_TD['old']` holds the row and `$_TD['new']` is absent; on
an INSERT, `$_TD['old']` is absent.

```sql
CREATE FUNCTION uppercase_name() RETURNS trigger LANGUAGE plruby AS $$
    $_TD['new']['name'] = $_TD['new']['name'].upcase
    'MODIFY'
$$;
```

Statement-level `TRUNCATE` triggers are also supported: `$_TD['event']` is
`TRUNCATE`, `$_TD['level']` is `STATEMENT`, there is no row, and the return
value is ignored.

`INSTEAD OF` triggers on views work like BEFORE row triggers: the function
performs the real operation itself (typically with `spi_exec` against the base
tables) and returns `nil` to report the row as processed, or `'SKIP'` to
suppress it. `WHEN (...)` conditions and deferrable constraint triggers behave
as they do for any trigger function.

## Event trigger functions

An event trigger function is declared `RETURNS event_trigger` and fires on DDL
events rather than on table rows. Its `$_TD` carries:

| Key             | Meaning                                             |
|-----------------|-----------------------------------------------------|
| `$_TD['event']` | the firing event, e.g. `ddl_command_start`          |
| `$_TD['tag']`   | the command tag, e.g. `CREATE TABLE`                |

```sql
CREATE FUNCTION no_drop() RETURNS event_trigger LANGUAGE plruby AS $$
    pg_raise('error', 'dropping tables is not allowed') if $_TD['tag'] == 'DROP TABLE'
$$;

CREATE EVENT TRIGGER guard ON ddl_command_start EXECUTE FUNCTION no_drop();
```

The return value of an event trigger function is ignored.

## Database access (SPI)

Run queries against the current database from within a function:

- `spi_exec(query [, limit])` — execute `query` (optionally limiting rows) and
  return a result object. The call runs in a subtransaction that is rolled back
  automatically if the query raises an error.
- `spi_fetch_row(result)` — return the next row as a `Hash`, or `nil` when the
  rows are exhausted.
- `spi_processed(result)` — number of rows the query produced.
- `spi_status(result)` — the SPI status code as a `String`.
- `spi_rewind(result)` — restart iteration from the first row.

```sql
CREATE FUNCTION sum_series(n integer) RETURNS integer LANGUAGE plruby AS $$
    r = spi_exec("select generate_series(1, #{args[0]}) as g")
    total = 0
    while (row = spi_fetch_row(r)); total += row['g']; end
    total
$$;
```

`spi_exec` materializes the whole result. To stream a large result without
holding it all in memory, use a cursor:

- `spi_query(query)` — open a cursor over `query`. With a block it yields each
  row `Hash` and closes the cursor automatically; without a block it returns a
  `PLRuby::Cursor`.
- `spi_fetchrow(cursor)` — the next row as a `Hash`, or `nil` at end of result.
- `spi_cursor_close(cursor)` — close a cursor early (otherwise it closes when
  exhausted or when the function returns). After closing, `spi_fetchrow`
  returns `nil` and closing again is a no-op.
- `PLRuby::Cursor` is `Enumerable` (via `#each`), so `map`, `select`, etc. work.

```sql
CREATE FUNCTION big_total() RETURNS bigint LANGUAGE plruby AS $$
    total = 0
    spi_query("select n from huge_table") { |row| total += row['n'] }
    total
$$;
```

Rows are read from the portal in batches, so memory stays bounded no matter how
large the result. A query error during streaming is raised as a Ruby exception
but is terminal for the surrounding statement (like a PL/pgSQL cursor loop).

## Prepared statements

For queries you run repeatedly, prepare a plan once and execute it with
parameters. `spi_prepare` takes the query text followed by the SQL type name of
each `$1`, `$2`, ... placeholder and returns a plan object:

- `spi_prepare(query, type1, type2, ...)` — returns a plan.
- `spi_exec_prepared(plan, arg1, arg2, ...)` — execute the plan; returns a
  result object just like `spi_exec`.
- `spi_query_prepared(plan, arg1, ...)` — stream the plan's result through a
  cursor instead of materializing it: with a block it yields each row and
  closes the cursor; without one it returns a `PLRuby::Cursor`. Closing the
  cursor leaves the plan reusable (`spi_freeplan` stays your job).
- `spi_freeplan(plan)` — release the plan when you are done with it.

```sql
CREATE FUNCTION lookup(int) RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select name from things where id = $1', 'int4')
    row  = spi_fetch_row(spi_exec_prepared(plan, args[0]))
    spi_freeplan(plan)
    row['name']
$$;
```

A plan can be cached in `$_SHARED` and reused across calls within a session for
better performance; free it with `spi_freeplan` when no longer needed.

## Transaction control

Inside a **procedure** invoked by `CALL` in a non-atomic context, you can commit
or roll back the current transaction:

- `spi_commit` — commit the current transaction and begin a new one.
- `spi_rollback` — roll back the current transaction and begin a new one.

```sql
CREATE PROCEDURE import_batch() LANGUAGE plruby AS $$
    (0...1000).each do |i|
        spi_exec("insert into staging select * from source where batch = #{i}")
        spi_commit            # make each batch durable as it completes
    end
$$;

CALL import_batch();
```

These are only valid in a non-atomic call context. Calling them from an ordinary
function, or from a procedure invoked inside an explicit `BEGIN`/`COMMIT` block,
raises `invalid transaction termination`.

## Subtransactions

`subtransaction { ... }` runs a block inside an internal subtransaction and
returns the block's value. You may also pass a callable and arguments:
`subtransaction(callable, arg, ...)`. If the block **raises an exception**, the
subtransaction's database changes are rolled back and the exception propagates
to the caller, where it can be rescued:

```sql
CREATE FUNCTION safe_insert(text) RETURNS text LANGUAGE plruby AS $$
    begin
        subtransaction do
            spi_exec("insert into t(name) values (#{quote_literal(args[0])})")
            raise 'empty name' if args[0] == ''
        end
        'inserted'
    rescue => e
        "skipped: #{e.message}"   # the insert was rolled back
    end
$$;
```

Under PL/Ruby's error model, a *database* error inside the block (for example a
constraint violation surfaced by `spi_exec`) is also converted to a rescuable
Ruby exception after the subtransaction is rolled back, so `rescue` can recover
from it.

## Quoting helpers

When building SQL dynamically, quote values and identifiers so the result is
safe and syntactically correct:

- `quote_literal(string)` — quote a value as an SQL string literal.
- `quote_nullable(value)` — like `quote_literal`, but a Ruby `nil` becomes the
  SQL keyword `NULL`.
- `quote_ident(name)` — quote a string for use as an SQL identifier (only when
  needed).

```ruby
sql = "select * from #{quote_ident(table)} where name = #{quote_literal(name)}"
```

## Messaging: elog and pg_raise

`elog(level, message)` emits a message at any PostgreSQL log level:

```ruby
elog('DEBUG',   'detailed diagnostics')
elog('LOG',     'goes to the server log')
elog('INFO',    'informational')
elog('NOTICE',  'shown to the client')
elog('WARNING', 'something looks off')
elog('ERROR',   'stop right here')   # aborts, like a PostgreSQL ERROR
```

The level is one of `DEBUG`, `LOG`, `INFO`, `NOTICE`, `WARNING`, or `ERROR`
(case-insensitive). Anything a function writes to standard output (`puts`,
`print`, `$stdout`) is also forwarded to the PostgreSQL log.

`pg_raise(level, message, ...)` accepts `notice`, `warning`, or `error` and
optionally attaches the structured error fields, like PL/pgSQL's
`RAISE ... USING`:

```ruby
pg_raise('ERROR', 'order rejected',
         detail: 'order 42 exceeds the credit limit',
         hint: 'raise the limit or split the order',
         sqlstate: 'P0001')
```

`detail:` and `hint:` become the error's `DETAIL` and `HINT` lines; `sqlstate:`
(five characters, digits or `A`–`Z`) sets its `SQLSTATE`. A PL/Ruby caller
that rescues such an error gets all three back via `PLRuby::Error#detail`,
`#hint`, and `#sqlstate` (see [Errors and exceptions](#errors-and-exceptions)).

## Shared data: `$_SHARED`

`$_SHARED` is a `Hash` that persists across function calls within the same
database session. Use it to cache data or share state between PL/Ruby functions:

```sql
CREATE FUNCTION set_shared(key text, val text) RETURNS void LANGUAGE plruby AS $$
    $_SHARED[args[0]] = args[1]
    nil
$$;

CREATE FUNCTION get_shared(key text) RETURNS text LANGUAGE plruby AS $$
    $_SHARED[args[0]]
$$;
```

## Session initialization: modules and start_proc

Two mechanisms let you run Ruby setup code the first time PL/Ruby is used in a
session.

**Modules.** If a table named `plruby_modules(modname text, modseq int, modsrc
text)` exists, its rows are evaluated at the top level (ordered by `modname`,
`modseq`) when the interpreter initializes. Any methods or classes the code
defines become available to every PL/Ruby function in the session — a
convenient place for a shared library of helpers:

```sql
CREATE TABLE plruby_modules (modname text, modseq int, modsrc text);
INSERT INTO plruby_modules VALUES
    ('util', 0, 'def slugify(s); s.strip.downcase.gsub(/\s+/, %q{-}); end');
-- slugify() is now callable from any PL/Ruby function in new sessions.
```

**start_proc.** The `plruby.start_proc` configuration setting names a PL/Ruby
function to call once, when the interpreter is first initialized in a session:

```sql
-- e.g. in postgresql.conf, ALTER DATABASE ... SET, or a session that has
-- already loaded PL/Ruby:
SET plruby.start_proc = 'my_setup';
```

Both run inside the first PL/Ruby call of the session, after the interpreter is
ready.

## Errors and exceptions

Function bodies are syntax-checked at `CREATE FUNCTION` time by the validator;
an invalid body is rejected with the Ruby parse error.

At run time, an uncaught Ruby exception (for example, calling an undefined
method or a `TypeError`) is reported as a PostgreSQL `ERROR`, aborting the
statement. You can raise an error yourself with `raise`, `elog('ERROR', ...)`,
or `pg_raise('error', ...)`.

A database error raised through the SPI functions (a failed `spi_exec`, a
constraint violation, and so on) arrives as a rescuable `PLRuby::Error`. The
exception's message is the PostgreSQL error message, and its **`sqlstate`**
method returns the original five-character `SQLSTATE` code:

```sql
CREATE FUNCTION divide(int, int) RETURNS text LANGUAGE plruby AS $$
    begin
        spi_exec("select #{args[0]} / #{args[1]}")
        'ok'
    rescue PLRuby::Error => e
        e.sqlstate == '22012' ? 'division by zero' : "failed: #{e.sqlstate}"
    end
$$;
```

The exception also carries the error's `DETAIL` and `HINT` when present, via
`PLRuby::Error#detail` and `#hint` — including fields attached by a PL/Ruby
function further down the stack with `pg_raise(..., detail:, hint:, sqlstate:)`.

`sqlstate`/`detail`/`hint` are `nil` on a `PLRuby::Error` that does not
originate from a database error (for example one raised by
`elog('ERROR', ...)`); ordinary Ruby exceptions do not have the methods at all.

## Security

PL/Ruby is an **untrusted** language. Historically a trusted variant could
restrict user code using Ruby's `$SAFE` levels and object tainting, but **both
were removed in Ruby 3.0**, so on modern Ruby nothing sandboxes a PL/Ruby
function: it can do whatever the PostgreSQL server's operating-system user can
do — read and write files, open network connections, run shell commands, and so
on.

Accordingly, the language is created without the `TRUSTED` attribute: the
extension is superuser-only to install, and only superusers can create PL/Ruby
functions. Do not hand that ability to roles you would not trust with the
server's operating-system account.
