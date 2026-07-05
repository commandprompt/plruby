--
-- Cursor streaming: spi_query opens a portal and rows are read one at a time
-- (block form, handle form, and Cursor#each), so large results are consumed
-- without materializing them all at once.
--
CREATE TABLE cur_src AS
    SELECT g AS n, 'row' || g AS label FROM generate_series(1, 600) g;

-- Block form: yields each row Hash and closes the cursor automatically.  600
-- rows crosses several internal fetch batches.
CREATE FUNCTION cur_sum() RETURNS bigint LANGUAGE plruby AS $$
    total = 0
    spi_query("select n from cur_src order by n") { |row| total += row['n'] }
    total
$$;
SELECT cur_sum();

-- Handle form: spi_query returns a cursor; spi_fetchrow reads rows.
CREATE FUNCTION cur_first() RETURNS text LANGUAGE plruby AS $$
    cur = spi_query("select n, label from cur_src order by n")
    row = spi_fetchrow(cur)
    spi_cursor_close(cur)
    "n=#{row['n']} label=#{row['label']}"
$$;
SELECT cur_first();

-- spi_fetchrow returns nil at end of result.
CREATE FUNCTION cur_drain() RETURNS text LANGUAGE plruby AS $$
    cur = spi_query("select n from cur_src where n <= 2 order by n")
    got = []
    while (row = spi_fetchrow(cur)); got << row['n']; end
    "rows=#{got.inspect} at_end=#{spi_fetchrow(cur).inspect}"
$$;
SELECT cur_drain();

-- Cursor#each makes a cursor Enumerable, so map/select compose over it.
CREATE FUNCTION cur_evens() RETURNS int LANGUAGE plruby AS $$
    cur = spi_query("select n from cur_src where n <= 10 order by n")
    cur.select { |r| r['n'].even? }.length
$$;
SELECT cur_evens();

-- A bad query raises a catchable Ruby exception; the session stays usable.
CREATE FUNCTION cur_bad() RETURNS text LANGUAGE plruby AS $$
    begin
        spi_query("select * from no_such_cursor_table") { |r| }
        'no error'
    rescue => e
        e.message.include?('no_such_cursor_table') ? 'caught' : 'other'
    end
$$;
SELECT cur_bad();
SELECT cur_sum();

DROP TABLE cur_src;
