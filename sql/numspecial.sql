--
-- Special numeric and floating-point values.
--

-- numeric is always carried losslessly as a String, including the NaN value.
CREATE FUNCTION n_num(numeric) RETURNS text LANGUAGE plruby AS $$
    "#{args[0].class}:#{args[0]}"
$$;
SELECT n_num('NaN'::numeric);
SELECT n_num(0.10);
SELECT n_num(-12345678901234567890.123);

-- float8 infinities and NaN map to Ruby Float and back.
CREATE FUNCTION n_finfo(float8) RETURNS text LANGUAGE plruby AS $$
    "#{args[0].class}:#{args[0]}:inf=#{args[0].infinite?.inspect}:nan=#{args[0].nan?}"
$$;
SELECT n_finfo('Infinity'::float8);
SELECT n_finfo('-Infinity'::float8);
SELECT n_finfo('NaN'::float8);

CREATE FUNCTION n_finf() RETURNS float8 LANGUAGE plruby AS $$ Float::INFINITY $$;
SELECT n_finf();
CREATE FUNCTION n_fnan() RETURNS float8 LANGUAGE plruby AS $$ Float::NAN $$;
SELECT n_fnan();

-- A Bignum result that does not fit the declared integer type overflows...
CREATE FUNCTION n_overflow() RETURNS integer LANGUAGE plruby AS $$ 2 ** 40 $$;
SELECT n_overflow();

-- ...but the same value fits bigint, and arbitrary precision fits numeric.
CREATE FUNCTION n_big() RETURNS bigint LANGUAGE plruby AS $$ 2 ** 40 $$;
SELECT n_big();
CREATE FUNCTION n_huge() RETURNS numeric LANGUAGE plruby AS $$ 10 ** 50 $$;
SELECT n_huge();
