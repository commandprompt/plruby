--
-- Trigger variants beyond the basics: INSTEAD OF on a view, WHEN clauses,
-- deferred constraint triggers, and 'MODIFY' touching a composite column.
--

-- INSTEAD OF INSERT on a view: the trigger routes the row to the base table.
CREATE TABLE t2_base (id int, val text);
CREATE VIEW t2_view AS SELECT id, val FROM t2_base;
CREATE FUNCTION t2_instead() RETURNS trigger LANGUAGE plruby AS $$
    spi_exec("insert into t2_base values (#{$_TD['new']['id'].to_i}, " +
             quote_literal($_TD['new']['val'].to_s.upcase) + ")")
    nil
$$;
CREATE TRIGGER t2_view_ins INSTEAD OF INSERT ON t2_view
    FOR EACH ROW EXECUTE PROCEDURE t2_instead();
INSERT INTO t2_view VALUES (1, 'hello');
SELECT * FROM t2_base;

-- WHEN clause: the function only runs when the condition holds.
CREATE TABLE t2_when (n int);
CREATE FUNCTION t2_notice() RETURNS trigger LANGUAGE plruby AS $$
    elog('NOTICE', "fired for n=#{$_TD['new']['n']}")
    nil
$$;
CREATE TRIGGER t2_when_trig BEFORE INSERT ON t2_when
    FOR EACH ROW WHEN (NEW.n > 10) EXECUTE PROCEDURE t2_notice();
INSERT INTO t2_when VALUES (5);
INSERT INTO t2_when VALUES (15);
SELECT * FROM t2_when ORDER BY n;

-- Deferred constraint trigger: fires at COMMIT, not at the statement.
CREATE TABLE t2_ct (n int);
CREATE FUNCTION t2_ct_check() RETURNS trigger LANGUAGE plruby AS $$
    elog('NOTICE', "constraint trigger for n=#{$_TD['new']['n']}")
    nil
$$;
CREATE CONSTRAINT TRIGGER t2_ct_trig AFTER INSERT ON t2_ct
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE PROCEDURE t2_ct_check();
BEGIN;
INSERT INTO t2_ct VALUES (1);
SELECT 'inside transaction, no notice yet' AS marker;
COMMIT;

-- 'MODIFY' with a composite-typed column: the new tuple carries a Hash for
-- the composite field, exercising the datum-based tuple modification.
CREATE TYPE t2_point AS (x int, y int);
CREATE TABLE t2_comp (id int, p t2_point);
CREATE FUNCTION t2_move() RETURNS trigger LANGUAGE plruby AS $$
    p = $_TD['new']['p']
    $_TD['new']['p'] = {'x' => p['x'] + 100, 'y' => p['y'] + 100}
    'MODIFY'
$$;
CREATE TRIGGER t2_comp_trig BEFORE INSERT ON t2_comp
    FOR EACH ROW EXECUTE PROCEDURE t2_move();
INSERT INTO t2_comp VALUES (1, ROW(1, 2));
SELECT id, (p).x, (p).y FROM t2_comp;

DROP TABLE t2_comp;
DROP TYPE t2_point;
DROP TABLE t2_ct;
DROP TABLE t2_when;
DROP VIEW t2_view;
DROP TABLE t2_base;
