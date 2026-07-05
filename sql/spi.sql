--
-- Database access via SPI.
--
CREATE TABLE spi_test (id int, name text);
INSERT INTO spi_test VALUES (1, 'one'), (2, 'two'), (3, 'three');

-- spi_exec + spi_fetch_row loop, with typed columns.
CREATE FUNCTION sum_ids() RETURNS int LANGUAGE plruby AS $$
    r = spi_exec("select id from spi_test order by id")
    total = 0
    while (row = spi_fetch_row(r)); total += row['id']; end
    total
$$;
SELECT sum_ids();

-- spi_processed / spi_status.
CREATE FUNCTION spi_meta() RETURNS text LANGUAGE plruby AS $$
    r = spi_exec("select * from spi_test")
    "processed=#{spi_processed(r)} status=#{spi_status(r)}"
$$;
SELECT spi_meta();

-- spi_rewind restarts iteration.
CREATE FUNCTION spi_first_twice() RETURNS text LANGUAGE plruby AS $$
    r = spi_exec("select name from spi_test order by id")
    a = spi_fetch_row(r)['name']
    spi_rewind(r)
    b = spi_fetch_row(r)['name']
    "#{a},#{b}"
$$;
SELECT spi_first_twice();

-- spi_exec honors a row limit.
CREATE FUNCTION spi_limited(int) RETURNS int LANGUAGE plruby AS $$
    spi_processed(spi_exec("select * from spi_test", args[0]))
$$;
SELECT spi_limited(2);

-- A failing query rolls back its subtransaction and re-raises.
CREATE FUNCTION spi_bad() RETURNS int LANGUAGE plruby AS $$
    spi_exec("select * from no_such_table")
    1
$$;
SELECT spi_bad();

-- The session is still usable afterwards.
SELECT sum_ids();

DROP TABLE spi_test;

-- DML through spi_exec reports the affected row count and operation status.
CREATE TABLE spi_dml (id int, val text);
CREATE FUNCTION spi_writes() RETURNS text LANGUAGE plruby AS $$
    out = []
    r = spi_exec("insert into spi_dml values (1,'a'),(2,'b'),(3,'c')")
    out << "ins=#{spi_processed(r)}/#{spi_status(r)}"
    r = spi_exec("update spi_dml set val = upper(val) where id <= 2")
    out << "upd=#{spi_processed(r)}/#{spi_status(r)}"
    r = spi_exec("delete from spi_dml where id = 3")
    out << "del=#{spi_processed(r)}/#{spi_status(r)}"
    out.join(' ')
$$;
SELECT spi_writes();
SELECT * FROM spi_dml ORDER BY id;

-- A NULL column in a fetched row arrives as nil; other columns keep their type.
CREATE FUNCTION spi_nullcol() RETURNS text LANGUAGE plruby AS $$
    row = spi_fetch_row(spi_exec(
        "select 1 as i, null::text as t, 2.5::float8 as f, array[1,2] as a"))
    "i=#{row['i'].class} t=#{row['t'].inspect} " +
        "f=#{row['f'].class} a=#{row['a'].inspect}"
$$;
SELECT spi_nullcol();

-- A utility command is accepted, reported as SPI_OK_UTILITY.
CREATE FUNCTION spi_utility() RETURNS text LANGUAGE plruby AS $$
    spi_status(spi_exec("set local work_mem = '2MB'"))
$$;
SELECT spi_utility();

DROP TABLE spi_dml;
