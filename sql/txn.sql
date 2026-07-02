--
-- Transaction control in procedures (spi_commit / spi_rollback).
--
CREATE TABLE txn_log (n int);

CREATE PROCEDURE txn_commit_each() LANGUAGE plruby AS $$
    (1..3).each do |i|
        spi_exec("insert into txn_log values (#{i})")
        spi_commit()
    end
$$;
CALL txn_commit_each();
SELECT count(*) FROM txn_log;

CREATE PROCEDURE txn_rollback_each() LANGUAGE plruby AS $$
    (10..12).each do |i|
        spi_exec("insert into txn_log values (#{i})")
        spi_rollback()
    end
$$;
CALL txn_rollback_each();
-- Rolled back inserts did not persist; still just the first three rows.
SELECT count(*) FROM txn_log;

-- spi_commit is invalid outside a non-atomic procedure context.
CREATE FUNCTION txn_in_func() RETURNS void LANGUAGE plruby AS $$
    spi_commit()
$$;
SELECT txn_in_func();

DROP TABLE txn_log;
