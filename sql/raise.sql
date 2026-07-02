--
-- Messaging: elog and pg_raise.
--
CREATE FUNCTION do_notice() RETURNS void LANGUAGE plruby AS $$
    elog('NOTICE', 'a notice from ruby')
    nil
$$;
SELECT do_notice();

CREATE FUNCTION do_warning() RETURNS void LANGUAGE plruby AS $$
    elog('WARNING', 'a warning from ruby')
    nil
$$;
SELECT do_warning();

CREATE FUNCTION do_error() RETURNS void LANGUAGE plruby AS $$
    elog('ERROR', 'an error from ruby')
    nil
$$;
SELECT do_error();

-- elog with an unknown level.
CREATE FUNCTION do_badlevel() RETURNS void LANGUAGE plruby AS $$
    elog('BOGUS', 'nope')
    nil
$$;
SELECT do_badlevel();

-- An uncaught Ruby exception becomes a PostgreSQL error.
CREATE FUNCTION do_raise() RETURNS void LANGUAGE plruby AS $$
    raise 'kaboom'
$$;
SELECT do_raise();

-- Calling an undefined method also surfaces as an error.
CREATE FUNCTION do_undef() RETURNS void LANGUAGE plruby AS $$
    this_method_does_not_exist()
$$;
SELECT do_undef();

-- pg_raise (the older, narrower spelling).
CREATE FUNCTION do_pg_raise() RETURNS void LANGUAGE plruby AS $$
    pg_raise('notice', 'via pg_raise')
    nil
$$;
SELECT do_pg_raise();
