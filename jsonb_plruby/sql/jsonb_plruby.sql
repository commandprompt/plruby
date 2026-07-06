--
-- jsonb <-> Ruby transform.  Functions must opt in with
-- TRANSFORM FOR TYPE jsonb.
--
CREATE EXTENSION jsonb_plruby CASCADE;

-- A jsonb argument arrives as native Ruby data.
CREATE FUNCTION jt_classes(jsonb) RETURNS text
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    walk = lambda { |v|
        case v
        when Hash  then "{#{v.map { |k, e| "#{k}:#{walk.call(e)}" }.join(',')}}"
        when Array then "[#{v.map { |e| walk.call(e) }.join(',')}]"
        else "#{v.class.name}(#{v.inspect})"
        end
    }
    walk.call(args[0])
$$;
SELECT jt_classes('{"s": "text", "i": 5, "f": 2.5, "t": true, "n": null, "a": [1, "two"]}');

-- Raw scalars unwrap.
SELECT jt_classes('42');
SELECT jt_classes('"just a string"');
SELECT jt_classes('null');

-- Integral numbers arrive as Integer (not Float), big ones included.
CREATE FUNCTION jt_sum(jsonb) RETURNS text
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    total = args[0].sum
    "#{total.class.name}=#{total}"
$$;
SELECT jt_sum('[1, 2, 3]');
SELECT jt_sum('[9007199254740993, 1]');

-- Returning Ruby data into jsonb, no JSON.generate needed.
CREATE FUNCTION jt_build(int) RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    {'n' => args[0], 'squares' => (1..args[0]).map { |i| i * i },
     'odd' => args[0].odd?, 'label' => :generated, 'missing' => nil}
$$;
SELECT jt_build(3);
SELECT jt_build(4)->'squares'->2 AS third_square;

-- Round-trip fidelity through both directions.
CREATE FUNCTION jt_echo(jsonb) RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    args[0]
$$;
SELECT jt_echo('{"a": [1, 2.5, "x", true, null], "b": {"nested": {"deep": []}}}');
SELECT jt_echo('[]');
SELECT jt_echo('{}');
SELECT jt_echo('3.14');

-- A rewrite in native Ruby: redact keys anywhere, hashes all the way down.
CREATE FUNCTION jt_redact(doc jsonb, key text) RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    redact = lambda { |v|
        case v
        when Hash  then v.to_h { |k, e| [k, k == key ? '[redacted]' : redact.call(e)] }
        when Array then v.map { |e| redact.call(e) }
        else v
        end
    }
    redact.call(doc)
$$;
SELECT jt_redact('{"user": "jd", "password": "s3cret", "kids": [{"password": "also"}]}',
                 'password');

-- A NULL jsonb argument is nil; returning nil is SQL NULL.
CREATE FUNCTION jt_null(jsonb) RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    args[0]
$$;
SELECT jt_null(NULL) IS NULL AS null_roundtrip;

-- BigDecimal (any Numeric) serializes losslessly through its text form.
CREATE FUNCTION jt_bigdec() RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    require 'bigdecimal'
    {'exact' => BigDecimal('0.1') + BigDecimal('0.2')}
$$;
SELECT jt_bigdec();

-- Unsupported Ruby objects are rejected cleanly.
CREATE FUNCTION jt_bad() RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    {'when' => Time.at(0)}
$$;
SELECT jt_bad();

-- Non-finite floats cannot become jsonb numbers.
CREATE FUNCTION jt_inf() RETURNS jsonb
TRANSFORM FOR TYPE jsonb
LANGUAGE plruby AS $$
    [Float::INFINITY]
$$;
SELECT jt_inf();

-- Without the TRANSFORM clause, jsonb still travels as a String.
CREATE FUNCTION jt_untransformed(jsonb) RETURNS text LANGUAGE plruby AS $$
    args[0].class.name
$$;
SELECT jt_untransformed('{"a": 1}');
