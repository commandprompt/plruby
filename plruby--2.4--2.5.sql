/* Upgrade PL/Ruby from 2.4 to 2.5.
 *
 * Every 2.5 change lives in the shared library ($_SD per-function storage, the
 * bytea<->binary String mapping, and the spi_colnames/spi_coltypes/
 * spi_coltypmods result accessors); the ltree transform is a separate
 * extension (ltree_plruby).  The core extension's SQL-level objects are
 * unchanged, so this script has nothing to do beyond moving the version.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plruby UPDATE TO '2.5'" to load this file. \quit
