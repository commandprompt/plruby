/* Upgrade PL/Ruby from 2.1 to 2.2.
 *
 * Every 2.2 change lives in the shared library (jsonb transform support,
 * RubyGems enablement, streaming spi_query_prepared, structured pg_raise,
 * domain base-type arguments); the SQL-level objects are unchanged, so this
 * script has nothing to do beyond moving the version.  The jsonb transform
 * itself is the separate jsonb_plruby extension.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plruby UPDATE TO '2.2'" to load this file. \quit
