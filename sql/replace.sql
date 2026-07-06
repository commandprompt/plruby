--
-- CREATE OR REPLACE FUNCTION invalidates the cached compiled body: the
-- per-backend proc cache keys on pg_proc xmin/tid, so a replaced function
-- must recompile on its next call in the same session.
--
CREATE FUNCTION rpl_greet() RETURNS text LANGUAGE plruby AS $$
    'version one'
$$;
SELECT rpl_greet();

CREATE OR REPLACE FUNCTION rpl_greet() RETURNS text LANGUAGE plruby AS $$
    'version two'
$$;
SELECT rpl_greet();

-- Replacing inside a transaction takes effect there too.
BEGIN;
CREATE OR REPLACE FUNCTION rpl_greet() RETURNS text LANGUAGE plruby AS $$
    'version three'
$$;
SELECT rpl_greet();
ROLLBACK;

-- After rollback, the previous definition is back.
SELECT rpl_greet();

-- A replaced function keeps its identity for a caller that runs it via SPI.
CREATE FUNCTION rpl_caller() RETURNS text LANGUAGE plruby AS $$
    spi_fetch_row(spi_exec('select rpl_greet() as g'))['g']
$$;
SELECT rpl_caller();
CREATE OR REPLACE FUNCTION rpl_greet() RETURNS text LANGUAGE plruby AS $$
    'version four'
$$;
SELECT rpl_caller();

-- Changing the argument list creates a differently keyed compilation.
CREATE FUNCTION rpl_add(int) RETURNS int LANGUAGE plruby AS $$
    args[0] + 1
$$;
SELECT rpl_add(1);
CREATE OR REPLACE FUNCTION rpl_add(int) RETURNS int LANGUAGE plruby AS $$
    args[0] + 100
$$;
SELECT rpl_add(1);
