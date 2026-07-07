# Why PL/Ruby embeds CRuby (MRI), not mruby

PL/Ruby links the full CRuby (MRI) interpreter via `libruby`. A recurring
question is why it does not use [mruby](https://github.com/mruby/mruby), the
lightweight, embeddable Ruby designed for exactly this kind of host-embedding
job. This document records the reasoning.

## Short answer

mruby optimizes for a small, isolated, compile-time-fixed footprint — great for
apps, games, and devices. PL/Ruby instead wants *real* Ruby in the database: the
complete standard library, `require`, gems, and full language semantics. Those
are precisely what mruby trades away. For PL/Ruby the compatibility cost
outweighs mruby's isolation benefit, so MRI is the right base.

## History

PL/Ruby dates to roughly 2000–2001 (Guy Decoux's original). mruby was not
announced until 2012 and did not mature for years after. mruby was never a live
alternative when the extension's design was set, and the entire C integration —
type mapping, SPI glue, trigger dispatch — was written against MRI's C API.

## Capabilities that would be lost under mruby

The items below are grounded in what PL/Ruby's own code relies on today.

### Would break core PL/Ruby features

- **Full standard library via `require`.** PL/Ruby deliberately keeps RubyGems
  enabled and runs the full MRI startup path (`ruby_options`) so that functions
  can `require 'csv'`, `'bigdecimal'`, `'base64'`, `'json'`, `'set'`, `'digest'`,
  and so on (see the initialization comment in `plruby.c`). mruby has no runtime
  stdlib `require`: every library must be a C-compiled *mrbgem* baked into the
  build, and the large majority of Ruby's stdlib has no mrbgem. A function that
  does `require 'csv'` simply stops working.

- **`BigDecimal` — lossless numerics.** PL/Ruby maps PostgreSQL `NUMERIC` to
  `BigDecimal` / decimal text (see `jsonb_plruby/jsonb_plruby.c`) so that
  arbitrary-precision values survive the round trip without float rounding — one
  of the areas where PL/Ruby is ahead of PL/Python. mruby core has no
  `BigDecimal` (only an integer bignum gem); you would regress to `Float` and
  silently corrupt monetary and scientific values.

- **The full Encoding framework.** `ltree_plruby` and `jsonb_plruby` use
  `rb_enc_str_new` / `ruby/encoding.h` to tag strings with the database encoding
  (UTF-8 vs. others). mruby's encoding support is minimal and lacks MRI's
  `Encoding` object model, so correct multi-encoding handling would regress.

- **`Rational`, `Complex`, and the full `Numeric` tower.** These ship in MRI
  core and are absent from mruby.

### Would force a rewrite of the extension internals

- **The C API itself.** Every line of glue is MRI: `VALUE`, `rb_define_method`,
  `rb_funcall`, `rb_protect`, `rb_eval_string_protect`. mruby's
  `mrb_state` / `mrb_value` model is different throughout, so the switch is a
  ground-up rewrite rather than a swap.

- **Runtime source compilation.** PL/Ruby compiles each stored function by
  evaluating Ruby source at define/call time (`rb_eval_string_protect`). mruby
  can load strings via the `mruby-eval` mrbgem, but full runtime
  metaprogramming — `binding`, complete `instance_eval`/`eval` semantics with
  local-variable capture, `TracePoint`, `ObjectSpace` enumeration — is partial
  or absent.

### Smaller or situational losses

`Marshal` (absent — affects serializing objects into `$_SHARED` / `$_SD`),
refinements, `Fiber`, keyword-argument fidelity, `ObjectSpace`, `TracePoint`,
and the entire installable-gem ecosystem (mruby is compile-time mrbgems only, a
small fraction of RubyGems).

## The one point in mruby's favor

MRI has a single, process-global VM: it initializes once per process, cannot be
cleanly unloaded, and Ractors do not cleanly fit this embedding. mruby's
`mrb_state` is fully isolated and re-entrant, so per-backend (or even per-call)
interpreters would be cleaner and would sidestep some of MRI's global-state
awkwardness. If someone were designing an embeddable Ruby PL from scratch today
and were willing to give up stdlib and gem compatibility, mruby would be a
defensible choice.

For PL/Ruby specifically, though, that compatibility is the whole point:
choosing mruby would turn "real Ruby in the database" into a restricted
Ruby-like DSL, surrendering the lossless-numeric and full-stdlib advantages that
currently put PL/Ruby ahead of PL/Python.
