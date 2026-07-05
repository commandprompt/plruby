# PL/Ruby - Ruby as a procedural language for PostgreSQL
#
# PGXS-based build.  Requires:
#   * PostgreSQL server development files (pg_config on PATH, or set PG_CONFIG)
#   * Ruby (>= 3.0) built as a shared library, with development headers
#     (ruby on PATH, or set RUBY)
#
# Build & install:
#   make
#   sudo make install
#   (in a database) CREATE EXTENSION plruby;
#
# Run the regression tests against a running server:
#   make installcheck

MODULE_big = plruby
OBJS = plruby.o plruby_io.o plruby_spi.o

EXTENSION = plruby
DATA = plruby--2.0.sql

# Ruby compile/link flags, discovered via RbConfig.
RUBY ?= ruby
RUBY_ARCHHDRDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["rubyarchhdrdir"]')
RUBY_HDRDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["rubyhdrdir"]')
RUBY_LIBDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["libdir"]')
RUBY_ARCHLIBDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["archlibdir"]')
# LIBRUBYARG_SHARED links against the shared libruby; add its directories so the
# loader can find it at build and load time.
RUBY_LIBARG := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBYARG_SHARED"]')
RUBY_LIBS := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["LIBS"]')

PG_CPPFLAGS = -I$(RUBY_ARCHHDRDIR) -I$(RUBY_HDRDIR)
SHLIB_LINK = -L$(RUBY_LIBDIR) -L$(RUBY_ARCHLIBDIR) $(RUBY_LIBARG) $(RUBY_LIBS)

# Regression tests.  "init" installs the extension; keep it first.
REGRESS = init base types numspecial bytea composite encoding datetime jsonb misc variadic shared trigger trigger2 spi raise errors sqlstate cargs pseudo srf out varnames validator prepare cursor hostile compat txn evttrig subxact modules quote require cookbook stdio startproc

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
