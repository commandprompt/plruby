--
-- OUT arguments combined with set-returning functions (TABLE) and records.
--

-- RETURNS TABLE columns behave like OUT arguments for a SRF.
CREATE FUNCTION gen_rows(n int) RETURNS TABLE(i int, sq int) LANGUAGE plruby AS $$
    (1..n).each { |x| i = x; sq = x * x; return_next() }
$$;
SELECT * FROM gen_rows(4);

-- Explicit per-row values for a TABLE function.
CREATE FUNCTION gen_explicit(n int) RETURNS TABLE(i int, sq int) LANGUAGE plruby AS $$
    (1..n).each { |x| return_next([x, x * x]) }
$$;
SELECT * FROM gen_explicit(3);

-- OUT arguments returned as an array from the body.
CREATE FUNCTION minmax(arr int[], OUT lo int, OUT hi int) LANGUAGE plruby AS $$
    lo = arr.min
    hi = arr.max
$$;
SELECT * FROM minmax(ARRAY[3, 1, 4, 1, 5, 9, 2, 6]);
