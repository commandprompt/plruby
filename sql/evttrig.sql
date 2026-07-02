--
-- Event triggers.
--
CREATE FUNCTION log_ddl() RETURNS event_trigger LANGUAGE plruby AS $$
    elog('NOTICE', "event=#{$_TD['event']} tag=#{$_TD['tag']}")
$$;
CREATE EVENT TRIGGER et_log ON ddl_command_start EXECUTE FUNCTION log_ddl();

CREATE TABLE et_demo (id int);
DROP TABLE et_demo;

DROP EVENT TRIGGER et_log;

-- An event trigger that forbids an operation.
CREATE FUNCTION no_drop() RETURNS event_trigger LANGUAGE plruby AS $$
    pg_raise('error', "dropping tables is not allowed") if $_TD['tag'] == 'DROP TABLE'
$$;
CREATE EVENT TRIGGER et_guard ON ddl_command_start EXECUTE FUNCTION no_drop();

CREATE TABLE et_guarded (id int);
DROP TABLE et_guarded;

DROP EVENT TRIGGER et_guard;
DROP TABLE et_guarded;
