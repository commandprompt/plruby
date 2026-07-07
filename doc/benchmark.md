# PL/Ruby performance

How fast is PL/Ruby compared to the built-in procedural languages and its
scripting-language peers? These are the first published numbers for the
extension. Reproduce them any time with the committed suite:

```sh
PGPORT=5432 sh bench/run.sh
```

The harness loads `bench/setup.sql` — the same four workloads written in
PL/Ruby, PL/pgSQL, PL/Perl, PL/Python and PL/Tcl with matching semantics — and
times each function under `pgbench -c 1`.

## Results

PostgreSQL 18.4, Ruby 3.2 (MRI, embedded), single client, `pgbench -T 8`, one
warm session, Ubuntu 24.04 container on x86-64. Peers: PL/Perl (Perl 5.38),
PL/Python (Python 3.12), PL/Tcl (Tcl 8.6). Transactions per second; higher is
better. Treat ±5% as noise (the SPI and array rows are the noisiest).

| Benchmark | PL/Ruby | PL/pgSQL | PL/Perl | PL/Python | PL/Tcl |
|---|---:|---:|---:|---:|---:|
| Call overhead (return argument) | 46,000 | 55,000 | 53,000 | 51,000 | 53,000 |
| String ops (reverse+upper+length) | 34,500 | 36,000 | 36,000 | 36,000 | 35,000 |
| SPI loop over 1,000 rows | 2,600 | 6,600 | 2,400 | 3,800 | 2,500 |
| Array marshaling (int[100] + 1) | 18,600 | 25,000 | 15,900 | 19,600 | 17,600 |

## Reading the numbers

- **Compute is call-overhead-bound, and PL/Ruby is in the pack.** On the string
  workload all five languages sit within a few percent of each other: the
  executor's function-call machinery dominates, not the interpreter. On the
  trivial return-the-argument call, PL/Ruby is ~15% behind PL/pgSQL and
  PL/Perl. That gap is the MRI entry path — every call crosses into the
  interpreter through a protected `rb_eval`/`rb_protect` trampoline so that Ruby
  exceptions become catchable PostgreSQL errors. It is a fixed per-call cost
  that the string workload's actual work already amortizes away.

- **Row iteration is PL/pgSQL's home turf.** Its `FOR ... IN SELECT` loop
  iterates natively without crossing a language boundary per row, so it leads
  the SPI workload by more than 2x. Among the interpreted languages PL/Python
  is fastest here because `plpy.execute` returns one materialized result whose
  rows are read by cached column mapping; PL/Ruby, PL/Perl and PL/Tcl cluster
  together (PL/Ruby marginally ahead of the other two). PL/Ruby's
  `spi_fetch_row` builds a fresh `Hash` per row — one output-function call and
  one String per column — which is the per-cell conversion cost, not anything
  in the loop itself.

- **Array marshaling favors the languages with native array conversion.**
  PL/pgSQL wins by doing `unnest`/`array_agg` in C. Among the scripting
  languages PL/Ruby is second only to PL/Python and comfortably ahead of
  PL/Perl and PL/Tcl: arguments arrive as a native Ruby `Array` and a returned
  `Array` converts straight back, with no text round-trip. PL/Tcl pays to parse
  the array's text form itself (it does not auto-convert array arguments to Tcl
  lists), and PL/Perl trails despite its arrayref conversion.

## Guidance

- For pure computation, use whichever language reads best; the overhead
  differences are small and the string-level work erases them.
- For tight loops over large results, prefer a set-based SQL statement (or
  PL/pgSQL) when the logic allows. When you need Ruby's expressiveness per row,
  `spi_query` with a block or `Cursor#each` keeps memory flat while paying the
  same per-row conversion cost measured here.
- For array- and composite-heavy work, PL/Ruby's native `Array`/`Hash`
  conversion is a genuine strength — the fastest of the interpreted PLs after
  PL/Python, and without the text-parsing tax PL/Tcl carries.

## Notes on method

Each workload is a single prepared-ish statement driven by `pgbench -c 1` for a
fixed wall-clock window; the reported figure is `pgbench`'s own TPS. One client
keeps the comparison about per-call language cost rather than concurrency or
lock behavior. The functions are validated to return identical results across
all five languages before timing (see `bench/setup.sql`); if a language's
extension is absent, `bench/run.sh` reports it as `skipped` rather than failing.
