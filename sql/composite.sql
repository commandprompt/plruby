--
-- Composite / record conversion: composites containing arrays, nested
-- composites (a composite field that is itself a record), arrays of composite,
-- and NULL fields -- in both directions.
--
CREATE TYPE box_t AS (id int, tags text[]);
CREATE TYPE nest_t AS (id int, child box_t);

-- A composite whose fields include an array round-trips through a Ruby Hash.
CREATE FUNCTION c_box() RETURNS box_t LANGUAGE plruby AS $$
    {'id' => 1, 'tags' => ['x', 'y']}
$$;
SELECT * FROM c_box();

-- Reading a composite argument exposes the array field as a Ruby Array.
CREATE FUNCTION c_box_read(box_t) RETURNS text LANGUAGE plruby AS $$
    "id=#{args[0]['id']} tags=#{args[0]['tags'].inspect}"
$$;
SELECT c_box_read(ROW(7, ARRAY['p', 'q'])::box_t);

-- A nested composite: a composite field whose value is itself a Hash.
CREATE FUNCTION c_nested() RETURNS nest_t LANGUAGE plruby AS $$
    {'id' => 1, 'child' => {'id' => 2, 'tags' => ['deep']}}
$$;
SELECT * FROM c_nested();

-- Reading a nested composite argument: the child arrives as a nested Hash.
CREATE FUNCTION c_nested_read(nest_t) RETURNS text LANGUAGE plruby AS $$
    o = args[0]
    "id=#{o['id']} child.id=#{o['child']['id']} " +
        "child.tags=#{o['child']['tags'].inspect}"
$$;
SELECT c_nested_read(ROW(1, ROW(2, ARRAY['deep'])::box_t)::nest_t);

-- An array of composite.
CREATE FUNCTION c_arr() RETURNS box_t[] LANGUAGE plruby AS $$
    [{'id' => 1, 'tags' => ['a']}, {'id' => 2, 'tags' => ['b', 'c']}]
$$;
SELECT c_arr();

-- A missing Hash key and an explicit nil both become SQL NULL.
CREATE FUNCTION c_nulls() RETURNS box_t LANGUAGE plruby AS $$
    {'id' => nil}
$$;
SELECT * FROM c_nulls();

DROP FUNCTION c_box_read(box_t);
DROP FUNCTION c_nested_read(nest_t);
DROP TYPE nest_t CASCADE;
DROP TYPE box_t CASCADE;
