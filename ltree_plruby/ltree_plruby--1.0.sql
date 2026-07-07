/* Transform between ltree and Ruby Arrays for PL/Ruby.
 *
 * With this transform, a function created with TRANSFORM FOR TYPE ltree
 * receives ltree arguments as a Ruby Array of label Strings, and may return
 * an Array of labels into an ltree result.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION ltree_plruby" to load this file. \quit

CREATE FUNCTION ltree_to_plruby(val internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION plruby_to_ltree(val internal)
RETURNS ltree
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TRANSFORM FOR ltree LANGUAGE plruby (
    FROM SQL WITH FUNCTION ltree_to_plruby(internal),
    TO SQL WITH FUNCTION plruby_to_ltree(internal)
);

COMMENT ON TRANSFORM FOR ltree LANGUAGE plruby
    IS 'transform between ltree and Ruby Arrays';
