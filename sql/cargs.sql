--
-- Argument modes: IN, OUT, INOUT, named.
--

-- Named IN arguments are available as local variables.
CREATE FUNCTION named_in(a int, b int) RETURNS int LANGUAGE plruby AS $$
    a * b
$$;
SELECT named_in(6, 7);

-- A single OUT argument.
CREATE FUNCTION one_out(a int, OUT doubled int) LANGUAGE plruby AS $$
    doubled = a * 2
$$;
SELECT one_out(21);
SELECT * FROM one_out(21);

-- Multiple OUT arguments.
CREATE FUNCTION add_sub(a int, b int, OUT sum int, OUT diff int)
LANGUAGE plruby AS $$
    sum = a + b
    diff = a - b
$$;
SELECT * FROM add_sub(10, 4);

-- INOUT argument.
CREATE FUNCTION inc(INOUT x int) LANGUAGE plruby AS $$
    x = x + 1
$$;
SELECT inc(41);

-- An explicit return inside the body still lets OUT collection run.
CREATE FUNCTION out_with_return(a int, OUT r int) LANGUAGE plruby AS $$
    r = a
    return if a > 0
    r = -a
$$;
SELECT out_with_return(5);
