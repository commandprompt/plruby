--
-- Error model: how Ruby exceptions and PostgreSQL errors interact, what is
-- preserved when a database error is caught, and what is not.
--

-- A specific exception class's message is preserved when it propagates out.
-- (A function body compiles to a method, so the class must be a built-in one;
-- defining a class inline is not allowed.)
CREATE FUNCTION es_custom() RETURNS int LANGUAGE plruby AS $$
    raise ArgumentError, 'custom failure'
$$;
SELECT es_custom();

-- A caught database error: it is a StandardError, its message names the
-- offending object, and the session keeps working afterwards.
CREATE FUNCTION es_caught() RETURNS text LANGUAGE plruby AS $$
    begin
        spi_exec('select * from missing_relation_xyz')
        'no error'
    rescue => e
        "std=#{e.is_a?(StandardError)} names_it=#{e.message.include?('missing_relation_xyz')}"
    end
$$;
SELECT es_caught();

-- The SQLSTATE of a caught database error is NOT surfaced on the Ruby
-- exception (only the message is bridged) -- documents a current limitation.
CREATE FUNCTION es_no_sqlstate() RETURNS boolean LANGUAGE plruby AS $$
    begin
        spi_exec('select 1 / 0')
        false
    rescue => e
        e.respond_to?(:sqlstate) || e.respond_to?(:errcode)
    end
$$;
SELECT es_no_sqlstate();

-- An ensure block runs on both the success and the rescue path.
CREATE FUNCTION es_ensure(boolean) RETURNS text LANGUAGE plruby AS $$
    log = []
    r = begin
            raise 'boom' if args[0]
            'ok'
        rescue
            'rescued'
        ensure
            log << 'ensured'
        end
    "#{r}:#{log.join}"
$$;
SELECT es_ensure(false);
SELECT es_ensure(true);

-- pg_raise and elog both reject an unknown level with a PL/Ruby error.
CREATE FUNCTION es_badraise() RETURNS void LANGUAGE plruby AS $$
    pg_raise('BOGUS', 'nope')
$$;
SELECT es_badraise();
CREATE FUNCTION es_badelog() RETURNS void LANGUAGE plruby AS $$
    elog('BOGUS', 'nope')
$$;
SELECT es_badelog();
