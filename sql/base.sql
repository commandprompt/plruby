--
-- Base functionality: scalars, arrays, composites, recursion.
--

-- A function returning nothing.
CREATE FUNCTION test_void() RETURNS integer LANGUAGE plruby AS $$
    nil
$$;
SELECT test_void();

-- A plain integer.
CREATE FUNCTION test_an_int() RETURNS integer LANGUAGE plruby AS $$
    1
$$;
SELECT test_an_int();
SELECT * FROM test_an_int();

-- Arrays.
CREATE FUNCTION test_an_array() RETURNS int[] LANGUAGE plruby AS $$
    [1]
$$;
SELECT test_an_array();

CREATE FUNCTION test_ndim_array() RETURNS int[] LANGUAGE plruby AS $$
    [[1, 3, 5], [2, 4, 6]]
$$;
SELECT test_ndim_array();

-- A function declared to return an array must return one.
CREATE FUNCTION test_a_bogus_array() RETURNS int[] LANGUAGE plruby AS $$
    1
$$;
SELECT test_a_bogus_array();

-- A scalar function may not return an array.
CREATE FUNCTION test_a_bogus_int() RETURNS integer LANGUAGE plruby AS $$
    [1]
$$;
SELECT test_a_bogus_int();

-- Native argument types: integers arrive as Integer.
CREATE FUNCTION r_max(integer, integer) RETURNS integer STRICT LANGUAGE plruby AS $$
    args[0] > args[1] ? args[0] : args[1]
$$;
SELECT r_max(1, 2);
SELECT r_max(-1, -999999999);

-- STRICT: NULL argument yields NULL result automatically.
SELECT r_max(NULL, 0) IS NULL AS is_null;

-- CALLED ON NULL INPUT: NULL arrives as nil.
CREATE FUNCTION r_max_null(integer, integer) RETURNS integer
CALLED ON NULL INPUT LANGUAGE plruby AS $$
    return args[1] if args[0].nil?
    return args[0] if args[1].nil?
    args[0] > args[1] ? args[0] : args[1]
$$;
SELECT r_max_null(NULL, 5);
SELECT r_max_null(5, NULL);
SELECT r_max_null(NULL, NULL) IS NULL AS both_null;

-- Text.
CREATE FUNCTION r_concat(text, text) RETURNS text STRICT LANGUAGE plruby AS $$
    args[0] + args[1]
$$;
SELECT r_concat('foo', 'bar');

-- Multidimensional array indexing on input.
CREATE FUNCTION r_arr_in(integer[], integer, integer) RETURNS integer
LANGUAGE plruby AS $$
    args[0][args[1]][args[2]]
$$;
SELECT r_arr_in(ARRAY[[1, 10], [2, 4], [3, 5]], 0, 0);
SELECT r_arr_in(ARRAY[[1, 10], [2, 4], [3, 5]], 2, 1);

-- Composite return via Hash.
CREATE TYPE tuple AS (name text, value integer);
CREATE FUNCTION make_tuple(text, integer) RETURNS tuple LANGUAGE plruby AS $$
    {'name' => args[0], 'value' => args[1]}
$$;
SELECT * FROM make_tuple('answer', 42);

-- Composite argument arrives as a Hash.
CREATE FUNCTION tuple_name(tuple) RETURNS text LANGUAGE plruby AS $$
    args[0]['name']
$$;
SELECT tuple_name(ROW('foo', 1)::tuple);

-- Recursive function through SPI.
CREATE FUNCTION r_fib(integer) RETURNS integer LANGUAGE plruby AS $$
    return 1 if args[0] <= 1
    a = spi_fetch_row(spi_exec("select r_fib(#{args[0]} - 1) as a"))['a']
    b = spi_fetch_row(spi_exec("select r_fib(#{args[0]} - 2) as b"))['b']
    a + b
$$;
SELECT r_fib(1);
SELECT r_fib(5);
SELECT r_fib(10);
