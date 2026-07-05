--
-- Quoting helpers: quote_literal, quote_nullable, quote_ident.
--
CREATE FUNCTION q_all(text) RETURNS text LANGUAGE plruby AS $$
    "lit=#{quote_literal(args[0])} " +
        "null=#{quote_nullable(args[0])} " +
        "ident=#{quote_ident(args[0])}"
$$;
SELECT q_all('plain');
SELECT q_all($$O'Reilly$$);
SELECT q_all('a"b');
SELECT q_all('back\slash');

-- nil: quote_nullable yields NULL, quote_literal coerces to the empty literal.
CREATE FUNCTION q_nil() RETURNS text LANGUAGE plruby AS $$
    "null=#{quote_nullable(nil)} lit=#{quote_literal(nil)}"
$$;
SELECT q_nil();

-- The quoted forms are usable as identifiers and literals in generated SQL.
CREATE TABLE "weird name" (id int);
CREATE FUNCTION q_use(text) RETURNS int LANGUAGE plruby AS $$
    spi_exec("insert into #{quote_ident('weird name')} values " +
             "(length(#{quote_literal(args[0])}))")
    spi_fetch_row(spi_exec("select max(id) as m from #{quote_ident('weird name')}"))['m']
$$;
SELECT q_use('abcde');
DROP TABLE "weird name";
