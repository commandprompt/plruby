--
-- $_SD: a Hash private to each function, persisting across calls to that
-- function within a session (counterpart of PL/Python's SD; $_SHARED is GD).
--

-- Persists across calls to the same function.
CREATE FUNCTION sd_counter() RETURNS int LANGUAGE plruby AS $$
    $_SD['n'] = ($_SD['n'] || 0) + 1
$$;
SELECT sd_counter();
SELECT sd_counter();
SELECT sd_counter();

-- A different function has its own independent $_SD.
CREATE FUNCTION sd_counter2() RETURNS int LANGUAGE plruby AS $$
    $_SD['n'] = ($_SD['n'] || 0) + 1
$$;
SELECT sd_counter2();
SELECT sd_counter();          -- unaffected by sd_counter2, keeps counting

-- $_SD is distinct from the session-global $_SHARED.
CREATE FUNCTION sd_vs_shared() RETURNS text LANGUAGE plruby AS $$
    $_SD['who'] = 'sd'
    $_SHARED['who'] = 'shared'
    "sd=#{$_SD['who']} shared=#{$_SHARED['who']}"
$$;
SELECT sd_vs_shared();

-- CREATE OR REPLACE resets $_SD (it is tied to the compiled function).
SELECT sd_counter();          -- still climbing before replace
CREATE OR REPLACE FUNCTION sd_counter() RETURNS int LANGUAGE plruby AS $$
    $_SD['n'] = ($_SD['n'] || 0) + 1
$$;
SELECT sd_counter();          -- back to 1 after replace

-- $_SD survives even when a later call in the same function raises.
CREATE FUNCTION sd_stash_then_fail() RETURNS int LANGUAGE plruby AS $$
    $_SD['kept'] = 'yes'
    raise 'deliberate failure'
$$;
SELECT sd_stash_then_fail();
CREATE FUNCTION sd_read_kept() RETURNS text LANGUAGE plruby AS $$
    $_SD['kept'] || 'unset'
$$;
SELECT sd_read_kept();        -- its own $_SD: never set 'kept'
