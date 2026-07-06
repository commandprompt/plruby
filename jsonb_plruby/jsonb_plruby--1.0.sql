/* Transform between jsonb and Ruby data for PL/Ruby.
 *
 * With this transform, a function created with TRANSFORM FOR TYPE jsonb
 * receives jsonb arguments as native Ruby data (Hash/Array/String/Integer/
 * Float/true/false/nil) and may return Ruby data into a jsonb result, with
 * no JSON.parse / JSON.generate round-trip.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_plruby" to load this file. \quit

CREATE FUNCTION jsonb_to_plruby(val internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION plruby_to_jsonb(val internal)
RETURNS jsonb
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;

CREATE TRANSFORM FOR jsonb LANGUAGE plruby (
    FROM SQL WITH FUNCTION jsonb_to_plruby(internal),
    TO SQL WITH FUNCTION plruby_to_jsonb(internal)
);

COMMENT ON TRANSFORM FOR jsonb LANGUAGE plruby
    IS 'transform between jsonb and Ruby data';
