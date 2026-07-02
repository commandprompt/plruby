--
-- Explicit subtransactions.
--
CREATE TABLE sx (id int, name text);

-- Block form: a raise inside rolls back the subtransaction, and is catchable.
CREATE FUNCTION safe_insert(text) RETURNS text LANGUAGE plruby AS $$
    begin
        subtransaction {
            spi_exec("insert into sx values (1, " + quote_literal(args[0]) + ")")
            raise 'empty name' if args[0] == ''
        }
        'inserted'
    rescue => e
        "skipped: #{e.message}"
    end
$$;
SELECT safe_insert('ok');
SELECT safe_insert('');
SELECT * FROM sx ORDER BY name;

-- The normal path commits and returns the block's value.
CREATE FUNCTION sx_value() RETURNS int LANGUAGE plruby AS $$
    subtransaction { 40 + 2 }
$$;
SELECT sx_value();

-- Callable form with extra arguments passed through.
CREATE FUNCTION sx_callable(int, int) RETURNS int LANGUAGE plruby AS $$
    adder = ->(a, b) { a + b }
    subtransaction(adder, args[0], args[1])
$$;
SELECT sx_callable(20, 22);

-- A database error inside a subtransaction is rolled back and re-raised
-- (catchable as a Ruby exception under PL/Ruby's error model).
CREATE FUNCTION sx_dberr() RETURNS text LANGUAGE plruby AS $$
    begin
        subtransaction { spi_exec("insert into sx values ('not an int', 'x')") }
        'inserted'
    rescue => e
        'caught db error'
    end
$$;
SELECT sx_dberr();
SELECT count(*) FROM sx;

DROP TABLE sx;
