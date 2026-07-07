-- Benchmark functions: identical logic in PL/Ruby, PL/pgSQL, PL/Perl,
-- PL/Python and PL/Tcl.  Loaded by bench/run.sh.  Each of the four workloads
-- has one function per language with matching semantics so the numbers compare
-- like for like.
CREATE EXTENSION IF NOT EXISTS plruby;
CREATE EXTENSION IF NOT EXISTS plperl;
CREATE EXTENSION IF NOT EXISTS plpython3u;
CREATE EXTENSION IF NOT EXISTS pltcl;

-- 1. Call overhead: a trivial body that returns its argument unchanged, so the
--    measurement is dominated by the per-invocation interpreter entry cost.
CREATE OR REPLACE FUNCTION call_ruby(a int) RETURNS int LANGUAGE plruby AS $$
    args[0]
$$;
CREATE OR REPLACE FUNCTION call_pgsql(a int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN RETURN a; END;
$$;
CREATE OR REPLACE FUNCTION call_perl(a int) RETURNS int LANGUAGE plperl AS $$
    return $_[0];
$$;
CREATE OR REPLACE FUNCTION call_python(a int) RETURNS int LANGUAGE plpython3u AS $$
return a
$$;
CREATE OR REPLACE FUNCTION call_tcl(a int) RETURNS int LANGUAGE pltcl AS $$
    return $1
$$;

-- 2. String / numeric ops: reverse, upper-case and append the length.  In-
--    language compute with no database access.
CREATE OR REPLACE FUNCTION str_ruby(t text) RETURNS text LANGUAGE plruby AS $$
    s = args[0]; s.reverse.upcase + s.length.to_s
$$;
CREATE OR REPLACE FUNCTION str_pgsql(t text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN RETURN upper(reverse(t)) || length(t); END;
$$;
CREATE OR REPLACE FUNCTION str_perl(t text) RETURNS text LANGUAGE plperl AS $$
    return uc(reverse($_[0])) . length($_[0]);
$$;
CREATE OR REPLACE FUNCTION str_python(t text) RETURNS text LANGUAGE plpython3u AS $$
return t[::-1].upper() + str(len(t))
$$;
CREATE OR REPLACE FUNCTION str_tcl(t text) RETURNS text LANGUAGE pltcl AS $$
    return "[string toupper [string reverse $1]][string length $1]"
$$;

-- 3. SPI queries: sum a column over a 1,000-row table.  Exercises the database-
--    access path and the per-row C-to-language value conversion.
DROP TABLE IF EXISTS bench_rows;
CREATE TABLE bench_rows AS SELECT g AS id, g * 2 AS val FROM generate_series(1, 1000) g;
CREATE OR REPLACE FUNCTION spi_ruby() RETURNS bigint LANGUAGE plruby AS $$
    r = spi_exec("select val from bench_rows")
    s = 0
    while (row = spi_fetch_row(r)); s += row['val']; end
    s
$$;
CREATE OR REPLACE FUNCTION spi_pgsql() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE s bigint := 0; r record;
BEGIN
    FOR r IN SELECT val FROM bench_rows LOOP s := s + r.val; END LOOP;
    RETURN s;
END;
$$;
CREATE OR REPLACE FUNCTION spi_perl() RETURNS bigint LANGUAGE plperl AS $$
    my $rv = spi_exec_query("select val from bench_rows");
    my $s = 0;
    $s += $_->{val} for @{$rv->{rows}};
    return $s;
$$;
CREATE OR REPLACE FUNCTION spi_python() RETURNS bigint LANGUAGE plpython3u AS $$
rv = plpy.execute("select val from bench_rows")
return sum(row["val"] for row in rv)
$$;
CREATE OR REPLACE FUNCTION spi_tcl() RETURNS bigint LANGUAGE pltcl AS $$
    set s 0
    spi_exec "select val from bench_rows" { set s [expr {$s + $val}] }
    return $s
$$;

-- 4. Array / composite marshaling: take an int[] and return each element + 1.
--    Exercises the type-conversion layer in both directions.
CREATE OR REPLACE FUNCTION arr_ruby(a int[]) RETURNS int[] LANGUAGE plruby AS $$
    args[0].map { |x| x + 1 }
$$;
CREATE OR REPLACE FUNCTION arr_pgsql(a int[]) RETURNS int[] LANGUAGE plpgsql AS $$
BEGIN RETURN ARRAY(SELECT x + 1 FROM unnest(a) AS x); END;
$$;
CREATE OR REPLACE FUNCTION arr_perl(a int[]) RETURNS int[] LANGUAGE plperl AS $$
    return [ map { $_ + 1 } @{$_[0]} ];
$$;
CREATE OR REPLACE FUNCTION arr_python(a int[]) RETURNS int[] LANGUAGE plpython3u AS $$
return [x + 1 for x in a]
$$;
-- PL/Tcl passes an array argument as its PostgreSQL text form ("{1,2,3}"),
-- not a Tcl list, so parse and rebuild the literal explicitly.
CREATE OR REPLACE FUNCTION arr_tcl(a int[]) RETURNS int[] LANGUAGE pltcl AS $$
    set out {}
    foreach x [split [string trim $1 "{}"] ","] { lappend out [expr {$x + 1}] }
    return "{[join $out ,]}"
$$;
