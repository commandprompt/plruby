--
-- plruby.start_proc: a function run once, before any user body, when the
-- interpreter is first used in a session.  Here it seeds $_SHARED.
--
CREATE FUNCTION sp_mark() RETURNS void LANGUAGE plruby AS $$
    $_SHARED['booted'] = 'yes'
    nil
$$;
CREATE FUNCTION sp_check() RETURNS text LANGUAGE plruby AS $$
    "booted=#{$_SHARED['booted'].inspect}"
$$;

SET plruby.start_proc = 'sp_mark';

-- The first PL/Ruby call in this session triggers session init, which runs
-- sp_mark first; sp_check then observes the value it stashed.
SELECT sp_check();

RESET plruby.start_proc;
DROP FUNCTION sp_check();
DROP FUNCTION sp_mark();
