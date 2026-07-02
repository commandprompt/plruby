--
-- Data type mapping between PostgreSQL and Ruby.
--

-- Each argument arrives as the natural Ruby class.
CREATE FUNCTION t_classes(smallint, integer, bigint, real, double precision,
                          numeric, boolean, text)
RETURNS text LANGUAGE plruby AS $$
    args.map { |a| a.class.name }.join(',')
$$;
SELECT t_classes(1::int2, 2, 3::int8, 4.5::real, 6.5::float8,
                 7.5::numeric, true, 'x');

-- Integer widths round-trip, including bigint.
CREATE FUNCTION t_int8(bigint) RETURNS bigint LANGUAGE plruby AS $$
    args[0] * 2
$$;
SELECT t_int8(5000000000);

-- Arbitrary-precision Integer (Bignum) returned into numeric.
CREATE FUNCTION t_bignum() RETURNS numeric LANGUAGE plruby AS $$
    2 ** 100
$$;
SELECT t_bignum();

-- Float arithmetic.
CREATE FUNCTION t_float(double precision) RETURNS double precision
LANGUAGE plruby AS $$
    args[0] / 4
$$;
SELECT t_float(10.0);

-- numeric arrives losslessly as a String.
CREATE FUNCTION t_numeric(numeric) RETURNS text LANGUAGE plruby AS $$
    "#{args[0].class}:#{args[0]}"
$$;
SELECT t_numeric(3.14159265358979323846);

-- Booleans map to true/false and back.
CREATE FUNCTION t_bool(boolean) RETURNS boolean LANGUAGE plruby AS $$
    !args[0]
$$;
SELECT t_bool(true), t_bool(false);

-- NULL arrives as nil in a non-strict function.
CREATE FUNCTION t_nullarg(integer) RETURNS text LANGUAGE plruby AS $$
    args[0].nil? ? 'nil' : "val:#{args[0]}"
$$;
SELECT t_nullarg(NULL), t_nullarg(7);

-- Empty array.
CREATE FUNCTION t_empty_array() RETURNS int[] LANGUAGE plruby AS $$
    []
$$;
SELECT t_empty_array();

-- Arrays may contain NULLs (nil).
CREATE FUNCTION t_array_with_nil() RETURNS int[] LANGUAGE plruby AS $$
    [1, nil, 3]
$$;
SELECT t_array_with_nil();

-- Text arrays round-trip, quoting commas, quotes, and NULLs correctly.
CREATE FUNCTION t_text_array(text[]) RETURNS text[] LANGUAGE plruby AS $$
    args[0].map { |s| s.nil? ? nil : s.reverse }
$$;
SELECT t_text_array(ARRAY['ab', 'c,d', 'e"f', NULL, 'g\h']);

-- A boolean array.
CREATE FUNCTION t_bool_array(boolean[]) RETURNS boolean[] LANGUAGE plruby AS $$
    args[0].map { |b| b.nil? ? nil : !b }
$$;
SELECT t_bool_array(ARRAY[true, false, NULL]);

-- Multibyte text is handled by character, not by byte (UTF-8 databases).
CREATE FUNCTION t_unicode(text) RETURNS text LANGUAGE plruby AS $$
    "chars=#{args[0].length} rev=#{args[0].reverse}"
$$;
SELECT t_unicode(U&'h\00e9llo');
