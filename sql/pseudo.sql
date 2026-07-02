--
-- Pseudotypes: record, anyelement, anyarray, void.
--

-- RETURNS record with a column definition list.
CREATE FUNCTION as_record() RETURNS record LANGUAGE plruby AS $$
    {'a' => 1, 'b' => 'two'}
$$;
SELECT * FROM as_record() AS (a int, b text);

-- A record function must return a Hash/Array.
CREATE FUNCTION bad_record() RETURNS record LANGUAGE plruby AS $$
    1
$$;
SELECT * FROM bad_record() AS (a int);

-- anyelement.
CREATE FUNCTION identity(anyelement) RETURNS anyelement LANGUAGE plruby AS $$
    args[0]
$$;
SELECT identity(42);
SELECT identity('hello'::text);

-- void.
CREATE FUNCTION returns_void() RETURNS void LANGUAGE plruby AS $$
    42
$$;
SELECT returns_void();
