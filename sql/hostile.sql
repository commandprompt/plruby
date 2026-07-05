--
-- Edge and failure paths: an error in the middle of a set-returning
-- function, cursor misuse, TOAST-sized values, and prepared-plan edges.
--

-- An exception after some rows have been returned aborts cleanly...
CREATE FUNCTION h_srf_boom() RETURNS SETOF int LANGUAGE plruby AS $$
    return_next 1
    return_next 2
    raise 'boom after two rows'
$$;
SELECT * FROM h_srf_boom();

-- ...and a following SRF call works (SRF state was restored).
CREATE FUNCTION h_srf_ok() RETURNS SETOF int LANGUAGE plruby AS $$
    (1..3).each { |i| return_next i }
    nil
$$;
SELECT * FROM h_srf_ok();

-- Breaking out of a spi_query block closes the portal (via ensure).
CREATE TABLE h_src AS SELECT g AS n FROM generate_series(1, 1000) g;
CREATE FUNCTION h_break() RETURNS int LANGUAGE plruby AS $$
    seen = 0
    spi_query('select n from h_src order by n') do |row|
        seen += 1
        break if row['n'] >= 3
    end
    seen
$$;
SELECT h_break();
SELECT count(*) FROM pg_cursors;

-- Enumerable early termination on a cursor handle; then the cursor is
-- closed, and fetching from or closing a closed cursor is a quiet no-op.
CREATE FUNCTION h_first() RETURNS text LANGUAGE plruby AS $$
    c = spi_query('select n from h_src order by n')
    two = c.first(2).map { |r| r['n'] }
    spi_cursor_close(c)
    spi_cursor_close(c)
    after = spi_fetchrow(c)
    "two=#{two.inspect} after_close=#{after.inspect}"
$$;
SELECT h_first();
SELECT count(*) FROM pg_cursors;

-- TOAST-sized values round-trip: a ~1MB string out and back in.
CREATE FUNCTION h_big_out(int) RETURNS text LANGUAGE plruby AS $$
    'abcdefgh' * args[0]
$$;
SELECT length(h_big_out(131072)) AS len, md5(h_big_out(131072)) AS digest;
CREATE FUNCTION h_big_in(text) RETURNS text LANGUAGE plruby AS $$
    "len=#{args[0].length} head=#{args[0][0, 8]} tail=#{args[0][-8, 8]}"
$$;
SELECT h_big_in(repeat('xyz', 400000));

-- Prepared plans: nil binds as NULL, wrong argument count is a clean error,
-- and a saved plan survives a transaction boundary (spi_commit) in a
-- procedure.
CREATE FUNCTION h_plan_null() RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select $1::int is null as isnull', 'int4')
    r1 = spi_fetch_row(spi_exec_prepared(plan, nil))['isnull']
    r2 = spi_fetch_row(spi_exec_prepared(plan, 5))['isnull']
    spi_freeplan(plan)
    "nil->#{r1} 5->#{r2}"
$$;
SELECT h_plan_null();

CREATE FUNCTION h_plan_argc() RETURNS text LANGUAGE plruby AS $$
    plan = spi_prepare('select $1::int + $2::int as s', 'int4', 'int4')
    begin
        spi_exec_prepared(plan, 1)
        'no error'
    rescue PLRuby::Error => e
        e.message
    ensure
        spi_freeplan(plan)
    end
$$;
SELECT h_plan_argc();

CREATE TABLE h_batch (n int);
CREATE PROCEDURE h_plan_across_commit() LANGUAGE plruby AS $$
    plan = spi_prepare('insert into h_batch values ($1)', 'int4')
    (1..3).each do |i|
        spi_exec_prepared(plan, i)
        spi_commit
    end
    spi_freeplan(plan)
$$;
CALL h_plan_across_commit();
SELECT * FROM h_batch ORDER BY n;

DROP TABLE h_batch;
DROP TABLE h_src;
