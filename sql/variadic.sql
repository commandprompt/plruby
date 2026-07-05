--
-- VARIADIC functions.  The variadic tail is delivered to Ruby as a single
-- Array argument (matching the SQL-level array), including when the caller
-- uses the VARIADIC array form.
--
CREATE FUNCTION v_sum(VARIADIC int[]) RETURNS int LANGUAGE plruby AS $$
    args[0].sum
$$;
SELECT v_sum(1, 2, 3);
SELECT v_sum(10);
SELECT v_sum(VARIADIC ARRAY[4, 5, 6]);

-- args.length is 1: the tail is one Array, not N scalars.
CREATE FUNCTION v_shape(VARIADIC int[]) RETURNS text LANGUAGE plruby AS $$
    "argc=#{args.length} tail=#{args[0].inspect}"
$$;
SELECT v_shape(7, 8, 9);

-- Fixed arguments before the variadic tail.
CREATE FUNCTION v_join(sep text, VARIADIC parts text[]) RETURNS text LANGUAGE plruby AS $$
    args[1].join(args[0])
$$;
SELECT v_join('-', 'a', 'b', 'c');
SELECT v_join(', ', 'solo');

-- NULL elements in the tail arrive as nil.
CREATE FUNCTION v_nils(VARIADIC text[]) RETURNS text LANGUAGE plruby AS $$
    args[0].map { |e| e.nil? ? '<nil>' : e }.join(',')
$$;
SELECT v_nils('x', NULL, 'y');

-- Named parameters still alias the variadic tail as a local.
CREATE FUNCTION v_named(VARIADIC nums int[]) RETURNS int LANGUAGE plruby AS $$
    nums.max
$$;
SELECT v_named(3, 1, 4, 1, 5);
