--
-- ltree <-> Ruby Array transform.  Functions must opt in with
-- TRANSFORM FOR TYPE ltree.  An ltree arrives as an Array of its label
-- Strings, and an Array of labels is joined back into an ltree.
--
CREATE EXTENSION ltree_plruby CASCADE;

-- An ltree argument arrives as an Array of label Strings.
CREATE FUNCTION lt_labels(ltree) RETURNS text
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    "#{args[0].class.name}: #{args[0].inspect}"
$$;
SELECT lt_labels('Top.Science.Astronomy'::ltree);

-- Array operations map onto path operations.
CREATE FUNCTION lt_nlabels(ltree) RETURNS int
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    args[0].length
$$;
SELECT lt_nlabels('a.b.c'::ltree);
SELECT lt_nlabels(''::ltree);

CREATE FUNCTION lt_parent(ltree) RETURNS ltree
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    args[0][0...-1]
$$;
SELECT lt_parent('a.b.c'::ltree);

CREATE FUNCTION lt_reverse(ltree) RETURNS ltree
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    args[0].reverse
$$;
SELECT lt_reverse('a.b.c'::ltree);

-- Build an ltree from Ruby data: elements are stringified before joining.
CREATE FUNCTION lt_build(int) RETURNS ltree
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    ['node', args[0], args[0] * 2]
$$;
SELECT lt_build(3);

-- The empty ltree round-trips as the empty Array.
CREATE FUNCTION lt_echo(ltree) RETURNS ltree
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    args[0]
$$;
SELECT lt_echo(''::ltree) = ''::ltree AS empty_roundtrip;

-- A NULL ltree argument is nil; returning nil is SQL NULL.
SELECT lt_echo(NULL) IS NULL AS null_roundtrip;

-- Returning something other than an Array is rejected cleanly.
CREATE FUNCTION lt_bad() RETURNS ltree
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    'not an array'
$$;
SELECT lt_bad();

-- The transform reaches nested contexts: SETOF rows via return_next...
CREATE FUNCTION lt_ancestors(ltree) RETURNS SETOF ltree
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    labels = args[0]
    (1..labels.length).each { |n| return_next(labels[0...n]) }
    nil
$$;
SELECT * FROM lt_ancestors('a.b.c'::ltree);

-- ...and composite rows with an ltree column.
CREATE FUNCTION lt_rows(ltree) RETURNS TABLE (depth int, path ltree)
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    labels = args[0]
    (1..labels.length).each do |n|
        return_next({'depth' => n, 'path' => labels[0...n]})
    end
    nil
$$;
SELECT * FROM lt_rows('x.y'::ltree);

-- Triggers that declare the transform get $_TD ltree columns as Arrays,
-- and 'MODIFY' accepts an Array back.
CREATE TABLE lt_items (id int, path ltree);
CREATE FUNCTION lt_normalize() RETURNS trigger
TRANSFORM FOR TYPE ltree
LANGUAGE plruby AS $$
    p = $_TD['new']['path']
    elog('NOTICE', "path is a #{p.class.name}: #{p.inspect}")
    $_TD['new']['path'] = p.map(&:downcase)
    'MODIFY'
$$;
CREATE TRIGGER lt_items_norm BEFORE INSERT ON lt_items
    FOR EACH ROW EXECUTE PROCEDURE lt_normalize();
INSERT INTO lt_items VALUES (1, 'Top.Science');
SELECT id, path FROM lt_items;
DROP TABLE lt_items;

-- Without the TRANSFORM clause, ltree still travels as a String.
CREATE FUNCTION lt_untransformed(ltree) RETURNS text LANGUAGE plruby AS $$
    "#{args[0].class.name}: #{args[0]}"
$$;
SELECT lt_untransformed('a.b'::ltree);
