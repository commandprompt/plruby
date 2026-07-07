/* PL/Ruby language, packaged as an extension.
 *
 * The language is UNTRUSTED (created without the TRUSTED attribute): on modern
 * Ruby there is no sandbox ($SAFE was removed in Ruby 3.0), so PL/Ruby
 * functions can do anything the server's OS user can.  Creating them is
 * therefore restricted to superusers.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plruby" to load this file. \quit

CREATE FUNCTION plruby_call_handler()
RETURNS language_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION plruby_inline_handler(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION plruby_validator(oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE LANGUAGE plruby
    HANDLER plruby_call_handler
    INLINE plruby_inline_handler
    VALIDATOR plruby_validator;

COMMENT ON LANGUAGE plruby IS 'PL/Ruby procedural language (untrusted)';
