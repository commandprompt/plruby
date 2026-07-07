--
-- SPI result column metadata: spi_colnames / spi_coltypes / spi_coltypmods,
-- the counterparts of PL/Python's colnames / coltypes / coltypmods.  Each is a
-- parallel Array over the result's columns; type OIDs are Integers.
--

-- A SELECT result exposes its column names, type OIDs, and type modifiers.
-- (int4 = 23, text = 25, varchar = 1043; varchar(10) typmod = 14 = 10 + 4.)
CREATE FUNCTION colmeta() RETURNS text LANGUAGE plruby AS $$
    r = spi_exec("SELECT 1::int AS id, 'hi'::text AS label, 'ab'::varchar(10) AS code")
    "names=#{spi_colnames(r).inspect} " +
    "types=#{spi_coltypes(r).inspect} " +
    "typmods=#{spi_coltypmods(r).inspect}"
$$;
SELECT colmeta();

-- The OIDs are usable: map them back to type names through the catalog.
CREATE FUNCTION colmeta_typenames() RETURNS text[] LANGUAGE plruby AS $$
    r = spi_exec("SELECT 1::int AS id, 'ab'::varchar(10) AS code")
    spi_coltypes(r).map do |oid|
        t = spi_exec("SELECT #{oid}::regtype::text AS n")
        spi_fetch_row(t)['n']
    end
$$;
SELECT colmeta_typenames();

-- A non-SELECT (no tuple table) has no columns: each metadata Array is empty.
CREATE FUNCTION colmeta_ddl() RETURNS text LANGUAGE plruby AS $$
    spi_exec("CREATE TEMP TABLE t_cm(x int)")
    r = spi_exec("INSERT INTO t_cm VALUES (1)")
    "names=#{spi_colnames(r).inspect} " +
    "types=#{spi_coltypes(r).inspect} " +
    "processed=#{spi_processed(r)}"
$$;
SELECT colmeta_ddl();
