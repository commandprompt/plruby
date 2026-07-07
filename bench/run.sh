#!/bin/sh
# Benchmark PL/Ruby against PL/pgSQL, PL/Perl, PL/Python and PL/Tcl.
#
# Usage: PGPORT=5432 [PGBENCH=/usr/lib/postgresql/18/bin/pgbench] sh bench/run.sh
# Requires: a running cluster you can create a "plruby_bench" database in, with
# plruby installed and (optionally) plperl / plpython3u / pltcl available.
#
# Each workload runs one function per language under `pgbench -c 1` for $SECS
# seconds and reports transactions per second (higher is better).
set -e
PGBENCH=${PGBENCH:-pgbench}
DB=plruby_bench
SECS=${SECS:-8}
LANGS=${LANGS:-"ruby pgsql perl python tcl"}

# A 100-element int[] literal for the array workload, built once.
ARR=$(seq -s, 1 100)

dropdb --if-exists $DB 2>/dev/null || true
createdb $DB
psql -qX -d $DB -f "$(dirname "$0")/setup.sql"

for fn in call str spi arr; do
  case $fn in
    call) body='\set a random(1,1000)
SELECT FN(:a);' ;;
    str)  body='SELECT FN(chr(97+(random()*20)::int) || repeat(chr(98), 30));' ;;
    spi)  body='SELECT FN();' ;;
    arr)  body="SELECT FN('{$ARR}'::int[]);" ;;
  esac
  for lang in $LANGS; do
    printf '%s\n' "$body" | sed "s/FN/${fn}_${lang}/" > /tmp/plruby_bench_$$.sql
    tps=$($PGBENCH -n -c 1 -T $SECS -f /tmp/plruby_bench_$$.sql $DB 2>/dev/null \
          | awk '/^tps/ {printf "%.0f", $3}')
    printf '%-12s %10s tps\n' "${fn}_${lang}" "${tps:-skipped}"
  done
done
rm -f /tmp/plruby_bench_$$.sql
