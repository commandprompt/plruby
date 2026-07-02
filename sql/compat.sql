--
-- Anonymous DO blocks and assorted compatibility behaviors.
--

-- A DO block runs one-off Ruby.
DO $$
    r = spi_exec('select count(*) as n from pg_class')
    elog('NOTICE', "pg_class query returned a row: #{spi_fetch_row(r)['n'] > 0}")
$$ LANGUAGE plruby;

-- Quoting helpers.
CREATE FUNCTION quoting(text) RETURNS text LANGUAGE plruby AS $$
    "#{quote_literal(args[0])} | #{quote_ident(args[0])} | #{quote_nullable(nil)}"
$$;
SELECT quoting($$O'Brien$$);

-- Booleans arrive as true/false and can be returned as such.
CREATE FUNCTION negate(bool) RETURNS bool LANGUAGE plruby AS $$
    !args[0]
$$;
SELECT negate(true), negate(false);

-- Floating point and numeric.
CREATE FUNCTION half(float8) RETURNS float8 LANGUAGE plruby AS $$
    args[0] / 2.0
$$;
SELECT half(5.0);

-- A text array round-trips with quoting of special characters.
CREATE FUNCTION arr_roundtrip(text[]) RETURNS text[] LANGUAGE plruby AS $$
    args[0].map { |s| s.nil? ? nil : s.upcase }
$$;
SELECT arr_roundtrip(ARRAY['a,b', 'c"d', NULL, 'e']);
