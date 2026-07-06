/* Upgrade PL/Ruby from 2.2 to 2.3.
 *
 * Every 2.3 change lives in the shared library (inline class definitions,
 * transforms in nested contexts and triggers); the SQL-level objects are
 * unchanged, so this script has nothing to do beyond moving the version.
 * The hstore transform is the separate hstore_plruby extension.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plruby UPDATE TO '2.3'" to load this file. \quit
