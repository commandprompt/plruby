/* Transform between hstore and Ruby Hashes for PL/Ruby.
 *
 * With this transform, a function created with TRANSFORM FOR TYPE hstore
 * receives hstore arguments as a Ruby Hash of String keys to String (or
 * nil) values, and may return a Hash into an hstore result.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plruby" to load this file. \quit

CREATE FUNCTION hstore_to_plruby(val internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION plruby_to_hstore(val internal)
RETURNS hstore
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TRANSFORM FOR hstore LANGUAGE plruby (
    FROM SQL WITH FUNCTION hstore_to_plruby(internal),
    TO SQL WITH FUNCTION plruby_to_hstore(internal)
);

COMMENT ON TRANSFORM FOR hstore LANGUAGE plruby
    IS 'transform between hstore and Ruby Hashes';
