--
-- Set-returning functions.
--

-- RETURNS SETOF scalar, return_next(value).
CREATE FUNCTION evens(int) RETURNS SETOF int LANGUAGE plruby AS $$
    (1..args[0]).each { |i| return_next(i * 2) }
$$;
SELECT * FROM evens(4);

-- RETURNS TABLE with a no-argument return_next reading the column locals.
CREATE FUNCTION squares(lim int) RETURNS TABLE(n int, square int)
LANGUAGE plruby AS $$
    (1..lim).each do |i|
        n = i
        square = i * i
        return_next()
    end
$$;
SELECT * FROM squares(3);

-- An empty result set.
SELECT count(*) FROM squares(0);

-- RETURNS SETOF composite via a Hash per row.
CREATE TYPE pair AS (k text, v int);
CREATE FUNCTION pairs() RETURNS SETOF pair LANGUAGE plruby AS $$
    return_next({'k' => 'a', 'v' => 1})
    return_next({'k' => 'b', 'v' => 2})
$$;
SELECT * FROM pairs();

-- return_next outside a set-returning function is an error.
CREATE FUNCTION not_srf() RETURNS int LANGUAGE plruby AS $$
    return_next(1)
    1
$$;
SELECT not_srf();
