--
-- The validator rejects syntactically invalid function bodies at creation.
--

-- A syntax error is reported by CREATE FUNCTION.
CREATE FUNCTION bad_syntax() RETURNS int LANGUAGE plruby AS $$
    def (
$$;

-- A well-formed body is accepted.
CREATE FUNCTION good_syntax() RETURNS int LANGUAGE plruby AS $$
    1 + 1
$$;
SELECT good_syntax();

-- Redefining a function recompiles it.
CREATE OR REPLACE FUNCTION good_syntax() RETURNS int LANGUAGE plruby AS $$
    2 + 2
$$;
SELECT good_syntax();
