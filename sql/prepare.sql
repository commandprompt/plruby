--
-- Prepared statements.
--
CREATE TABLE things (id int, name text);
INSERT INTO things VALUES (1, 'one'), (2, 'two'), (3, 'three');

CREATE FUNCTION lookup(int) RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select name from things where id = $1', 'int4')
    res = spi_exec_prepared(plan, args[0])
    row = spi_fetch_row(res)
    spi_freeplan(plan)
    row['name']
$$;
SELECT lookup(2);

-- spi_query_prepared streams a prepared plan through a cursor: block form,
-- multiple placeholders.
CREATE FUNCTION names_between(int, int) RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select name from things where id between $1 and $2 order by id',
                       'int4', 'int4')
    names = []
    spi_query_prepared(plan, args[0], args[1]) { |row| names << row['name'] }
    spi_freeplan(plan)
    names.join(',')
$$;
SELECT names_between(1, 2);

-- Handle form: the cursor is Enumerable, closing it leaves the plan
-- reusable, and the caller still owns (and frees) the plan.
CREATE FUNCTION reuse_after_cursor() RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select name from things where id >= $1 order by id', 'int4')
    cur = spi_query_prepared(plan, 1)
    first_two = cur.first(2).map { |r| r['name'] }
    spi_cursor_close(cur)
    rest = []
    spi_query_prepared(plan, 3) { |row| rest << row['name'] }
    spi_freeplan(plan)
    "first_two=#{first_two.join(',')} rest=#{rest.join(',')}"
$$;
SELECT reuse_after_cursor();

-- Argument-count mismatch is a clean error for the cursor form too.
CREATE FUNCTION qp_wrong() RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select $1::int + $2::int as s', 'int4', 'int4')
    begin
        spi_query_prepared(plan, 1)
        'no error'
    rescue PLRuby::Error => e
        e.message
    ensure
        spi_freeplan(plan)
    end
$$;
SELECT qp_wrong();

-- A NULL parameter.
CREATE FUNCTION prep_null() RETURNS int LANGUAGE plruby AS $$
    plan = spi_prepare('select count(*) as c from things where $1::int is null', 'int4')
    spi_fetch_row(spi_exec_prepared(plan, nil))['c']
$$;
SELECT prep_null();

-- Wrong number of arguments is reported.
CREATE FUNCTION prep_wrong() RETURNS int LANGUAGE plruby AS $$
    plan = spi_prepare('select $1::int', 'int4')
    spi_fetch_row(spi_exec_prepared(plan))
    1
$$;
SELECT prep_wrong();

-- One prepared plan reused across many executions in a single call.
CREATE FUNCTION prep_reuse() RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select name from things where id = $1', 'int4')
    names = (1..3).map { |i| spi_fetch_row(spi_exec_prepared(plan, i))['name'] }
    spi_freeplan(plan)
    names.join(',')
$$;
SELECT prep_reuse();

-- Text and numeric parameter types.
CREATE FUNCTION prep_typed(text) RETURNS int LANGUAGE plruby AS $$
    plan = spi_prepare('select count(*) as c from things where name > $1', 'text')
    spi_fetch_row(spi_exec_prepared(plan, args[0]))['c']
$$;
SELECT prep_typed('one');

-- Using a plan after spi_freeplan raises cleanly, and the session recovers.
CREATE FUNCTION prep_after_free() RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select $1::int', 'int4')
    spi_freeplan(plan)
    begin
        spi_exec_prepared(plan, 1)
        'reused freed plan'
    rescue => e
        'freed plan rejected'
    end
$$;
SELECT prep_after_free();

DROP TABLE things;
