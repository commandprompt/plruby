--
-- hstore <-> Ruby Hash transform.  Functions must opt in with
-- TRANSFORM FOR TYPE hstore.
--
CREATE EXTENSION hstore_plruby CASCADE;

-- An hstore argument arrives as a Hash of String => String/nil.
CREATE FUNCTION ht_classes(hstore) RETURNS text
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    args[0].sort.map { |k, v|
        "#{k.class.name}(#{k})=#{v.nil? ? 'nil' : "#{v.class.name}(#{v})"}"
    }.join(' ')
$$;
SELECT ht_classes('a=>1, b=>NULL, "key with spaces"=>"and \"quotes\""'::hstore);

-- Round-trip: modify and return; nil becomes an hstore NULL.
CREATE FUNCTION ht_upcase(hstore) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    args[0].to_h { |k, v| [k.upcase, v&.upcase] }
$$;
SELECT ht_upcase('name=>widget, note=>NULL');

-- Build an hstore from Ruby data: keys and values are stringified.
CREATE FUNCTION ht_build(int) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    {:n => args[0], 'double' => args[0] * 2, 'ok' => args[0].odd?, 'gone' => nil}
$$;
SELECT ht_build(3);
SELECT ht_build(4)->'double' AS doubled;

-- Idiomatic Hash work: merge and prune in one step.
CREATE FUNCTION ht_merge(a hstore, b hstore) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    a.merge(b).reject { |_, v| v.nil? }
$$;
SELECT ht_merge('a=>1, b=>2', 'b=>20, c=>NULL, d=>4');

-- The empty hstore round-trips.
CREATE FUNCTION ht_empty(hstore) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    args[0]
$$;
SELECT ht_empty(''::hstore);

-- A NULL hstore argument is nil; returning nil is SQL NULL.
SELECT ht_empty(NULL) IS NULL AS null_roundtrip;

-- Returning something other than a Hash is rejected cleanly.
CREATE FUNCTION ht_bad() RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    ['not', 'a', 'hash']
$$;
SELECT ht_bad();

-- The transform reaches nested contexts: SETOF rows via return_next...
CREATE FUNCTION ht_shatter(hstore) RETURNS SETOF hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    args[0].sort.each { |k, v| return_next({k => v}) }
    nil
$$;
SELECT * FROM ht_shatter('b=>2, a=>1');

-- ...and composite rows with an hstore column.
CREATE FUNCTION ht_rows(hstore) RETURNS TABLE (key text, meta hstore)
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    args[0].sort.each do |k, v|
        return_next({'key' => k, 'meta' => {'value' => v.to_s, 'null' => v.nil?.to_s}})
    end
    nil
$$;
SELECT * FROM ht_rows('x=>hi, y=>NULL');

-- Triggers that declare the transform get $_TD hstore columns as Hashes,
-- and 'MODIFY' accepts a Hash back.
CREATE TABLE ht_items (id int, attrs hstore);
CREATE FUNCTION ht_normalize() RETURNS trigger
TRANSFORM FOR TYPE hstore
LANGUAGE plruby AS $$
    attrs = $_TD['new']['attrs']
    elog('NOTICE', "attrs is a #{attrs.class.name}")
    $_TD['new']['attrs'] = attrs.to_h { |k, v| [k.downcase, v] }
                                .merge('checked' => 'yes')
    'MODIFY'
$$;
CREATE TRIGGER ht_items_norm BEFORE INSERT ON ht_items
    FOR EACH ROW EXECUTE PROCEDURE ht_normalize();
INSERT INTO ht_items VALUES (1, 'Color=>red, SIZE=>xl');
SELECT id, attrs FROM ht_items;
DROP TABLE ht_items;

-- Without the TRANSFORM clause, hstore still travels as a String.
CREATE FUNCTION ht_untransformed(hstore) RETURNS text LANGUAGE plruby AS $$
    args[0].class.name
$$;
SELECT ht_untransformed('a=>1'::hstore);
