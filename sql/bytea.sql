--
-- bytea conversion.  Like every type, a bytea reaches Ruby through its output
-- function, so it arrives as its *textual* (hex) representation -- a String --
-- not as raw bytes.  Returning a bytea feeds the String back through bytea's
-- input function.
--
SET bytea_output = 'hex';

-- A bytea argument is the hex text string (UTF-8 tagged), preserving NUL and
-- high bytes in that representation.
CREATE FUNCTION b_form(bytea) RETURNS text LANGUAGE plruby AS $$
    "#{args[0].class}:#{args[0]}"
$$;
SELECT b_form('\xdeadbeef'::bytea);
SELECT b_form('\x00ff01fe'::bytea);

-- Round-trip: returning the hex string yields the identical bytea.
CREATE FUNCTION b_echo(bytea) RETURNS bytea LANGUAGE plruby AS $$
    args[0]
$$;
SELECT b_echo('\x00ff01fe'::bytea);

-- Building a bytea from a Ruby-produced hex string.
CREATE FUNCTION b_make() RETURNS bytea LANGUAGE plruby AS $$
    "\\x" + [0, 255, 16, 32].map { |n| "%02x" % n }.join
$$;
SELECT b_make();

-- The empty bytea.
CREATE FUNCTION b_empty() RETURNS bytea LANGUAGE plruby AS $$
    "\\x"
$$;
SELECT b_empty();
