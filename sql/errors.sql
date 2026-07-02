--
-- Error handling: how Ruby failures surface as PostgreSQL errors, and how
-- user code can catch and recover from them.
--

-- An uncaught RuntimeError becomes a PostgreSQL ERROR.
CREATE FUNCTION e_raise() RETURNS int LANGUAGE plruby AS $$
    raise 'boom'
$$;
SELECT e_raise();

-- A specific exception class's message is preserved.
CREATE FUNCTION e_argerror() RETURNS int LANGUAGE plruby AS $$
    Integer('not a number')
$$;
SELECT e_argerror();

-- A ZeroDivisionError surfaces as an error.
CREATE FUNCTION e_divzero() RETURNS int LANGUAGE plruby AS $$
    1 / 0
$$;
SELECT e_divzero();

-- Exceptions can be rescued inside the function and turned into a value.
CREATE FUNCTION e_rescued() RETURNS text LANGUAGE plruby AS $$
    begin
        raise 'inner failure'
    rescue => ex
        "caught: #{ex.message}"
    end
$$;
SELECT e_rescued();

-- A database error from spi_exec is catchable as a Ruby exception.
CREATE FUNCTION e_spi_caught() RETURNS text LANGUAGE plruby AS $$
    begin
        spi_exec('select * from a_table_that_does_not_exist')
        'no error'
    rescue => ex
        'caught spi error'
    end
$$;
SELECT e_spi_caught();

-- Returning the wrong shape is a datatype error.
CREATE FUNCTION e_wrong_array() RETURNS int[] LANGUAGE plruby AS $$
    42
$$;
SELECT e_wrong_array();

-- elog('ERROR', ...) aborts like a PostgreSQL error.
CREATE FUNCTION e_elog() RETURNS void LANGUAGE plruby AS $$
    elog('ERROR', 'explicit elog error')
$$;
SELECT e_elog();

-- After an error, the session remains usable.
CREATE FUNCTION e_ok() RETURNS int LANGUAGE plruby AS $$
    123
$$;
SELECT e_ok();
