--
-- Argument and column names, including ones that are not valid Ruby
-- local-variable names (accessible only positionally through args).
--

-- A capitalized argument name is not a valid Ruby local; the positional
-- args array still works.
CREATE FUNCTION cap_arg("Value" int) RETURNS int LANGUAGE plruby AS $$
    args[0] + 1
$$;
SELECT cap_arg(41);

-- Numeric-looking column names in a composite result.
CREATE TYPE nums AS ("1" int, "2" int);
CREATE FUNCTION make_nums() RETURNS nums LANGUAGE plruby AS $$
    {'1' => 10, '2' => 20}
$$;
SELECT * FROM make_nums();

-- Underscored and valid names are aliased as locals.
CREATE FUNCTION valid_names(first_name text, last_name text) RETURNS text
LANGUAGE plruby AS $$
    "#{first_name} #{last_name}"
$$;
SELECT valid_names('Grace', 'Hopper');
