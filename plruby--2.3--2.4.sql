/* Upgrade PL/Ruby from 2.3 to 2.4.
 *
 * Every 2.4 change lives in the shared library (the plruby.on_init GUC, the
 * anycompatible/anycompatiblearray polymorphics, error CONTEXT lines, and a
 * compiled-function cache fix); the SQL-level objects are unchanged, so this
 * script has nothing to do beyond moving the version.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plruby UPDATE TO '2.4'" to load this file. \quit
