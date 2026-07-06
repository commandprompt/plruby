--
-- plruby.on_init: raw Ruby evaluated once when the interpreter is first
-- initialized in a session, before plruby_modules and plruby.start_proc.
-- The counterpart of plperl.on_init.  A constant and a helper method it
-- defines are visible to every function body in the session.
--
SET plruby.on_init = 'ON_INIT_MARKER = 1234; def doubled_by_on_init(x); x * 2; end';

CREATE FUNCTION oi_marker() RETURNS int LANGUAGE plruby AS $$
    ON_INIT_MARKER
$$;
CREATE FUNCTION oi_helper(int) RETURNS int LANGUAGE plruby AS $$
    doubled_by_on_init(args[0])
$$;

-- The first PL/Ruby call in this session runs on_init, so both are defined.
SELECT oi_marker();
SELECT oi_helper(21);

RESET plruby.on_init;
DROP FUNCTION oi_marker();
DROP FUNCTION oi_helper(int);
