/* Upgrade PL/Ruby from 2.0 to 2.1.
 *
 * Every 2.1 change lives in the shared library (SQLSTATE on exceptions,
 * VARIADIC arguments, INSTEAD OF triggers, composite-column 'MODIFY',
 * cursor-close semantics); the SQL-level objects are unchanged, so this
 * script has nothing to do beyond moving the version.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plruby UPDATE TO '2.1'" to load this file. \quit
