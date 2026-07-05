--
-- Data-modification triggers.
--
CREATE TABLE trig_test (id int, name text);

-- BEFORE INSERT/UPDATE: uppercase the name (MODIFY).
CREATE FUNCTION upper_name() RETURNS trigger LANGUAGE plruby AS $$
    $_TD['new']['name'] = $_TD['new']['name'].upcase
    'MODIFY'
$$;
CREATE TRIGGER t_upper BEFORE INSERT OR UPDATE ON trig_test
    FOR EACH ROW EXECUTE FUNCTION upper_name();

INSERT INTO trig_test VALUES (1, 'alice'), (2, 'bob');
SELECT * FROM trig_test ORDER BY id;
UPDATE trig_test SET name = 'carol' WHERE id = 1;
SELECT * FROM trig_test ORDER BY id;

-- BEFORE INSERT: SKIP rows with an odd id.
CREATE TABLE skip_test (id int);
CREATE FUNCTION skip_odd() RETURNS trigger LANGUAGE plruby AS $$
    return 'SKIP' if $_TD['new']['id'].odd?
    nil
$$;
CREATE TRIGGER t_skip BEFORE INSERT ON skip_test
    FOR EACH ROW EXECUTE FUNCTION skip_odd();
INSERT INTO skip_test VALUES (1), (2), (3), (4);
SELECT * FROM skip_test ORDER BY id;

-- Trigger metadata is exposed in $_TD.
CREATE TABLE meta_test (id int);
CREATE FUNCTION show_meta() RETURNS trigger LANGUAGE plruby AS $$
    elog('NOTICE', "when=#{$_TD['when']} level=#{$_TD['level']} " +
                   "event=#{$_TD['event']} rel=#{$_TD['relname']}")
    nil
$$;
CREATE TRIGGER t_meta_row BEFORE INSERT ON meta_test
    FOR EACH ROW EXECUTE FUNCTION show_meta();
CREATE TRIGGER t_meta_stmt AFTER INSERT ON meta_test
    FOR EACH STATEMENT EXECUTE FUNCTION show_meta();
INSERT INTO meta_test VALUES (1);

-- Trigger arguments.
CREATE TABLE arg_test (id int);
CREATE FUNCTION show_args() RETURNS trigger LANGUAGE plruby AS $$
    elog('NOTICE', "argc=#{$_TD['argc']} args=#{$_TD['args'].inspect}")
    nil
$$;
CREATE TRIGGER t_args BEFORE INSERT ON arg_test
    FOR EACH ROW EXECUTE FUNCTION show_args('a', 'b');
INSERT INTO arg_test VALUES (1);

DROP TABLE trig_test, skip_test, meta_test, arg_test;

-- DELETE trigger: $_TD['old'] holds the row and 'new' is absent.  Returning
-- nil lets the delete proceed; 'SKIP' suppresses it for the guarded row.
CREATE TABLE del_test (id int, name text);
INSERT INTO del_test VALUES (1, 'keep'), (2, 'drop'), (3, 'guard');
CREATE FUNCTION on_delete() RETURNS trigger LANGUAGE plruby AS $$
    elog('NOTICE', "event=#{$_TD['event']} old=#{$_TD['old'].inspect} " +
                   "has_new=#{$_TD.key?('new')}")
    return 'SKIP' if $_TD['old']['name'] == 'guard'
    nil
$$;
CREATE TRIGGER t_del BEFORE DELETE ON del_test
    FOR EACH ROW EXECUTE FUNCTION on_delete();
DELETE FROM del_test WHERE id IN (2, 3);
SELECT * FROM del_test ORDER BY id;

DROP TABLE del_test;

-- KNOWN LIMITATION: TRUNCATE triggers are not yet dispatched -- the handler
-- rejects the firing event.  Kept as a regression guard.
CREATE TABLE trunc_test (id int);
CREATE FUNCTION on_truncate() RETURNS trigger LANGUAGE plruby AS $$
    nil
$$;
CREATE TRIGGER t_trunc BEFORE TRUNCATE ON trunc_test
    FOR EACH STATEMENT EXECUTE FUNCTION on_truncate();
TRUNCATE trunc_test;

DROP TABLE trunc_test;
