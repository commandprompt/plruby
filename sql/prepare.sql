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

-- spi_query_prepared is an alias; multiple placeholders.
CREATE FUNCTION count_between(int, int) RETURNS int LANGUAGE plruby AS $$
    plan = spi_prepare('select count(*) as c from things where id between $1 and $2',
                       'int4', 'int4')
    spi_fetch_row(spi_query_prepared(plan, args[0], args[1]))['c']
$$;
SELECT count_between(1, 2);

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

DROP TABLE things;
