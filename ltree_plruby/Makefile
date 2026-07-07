# ltree_plruby - transform between ltree and Ruby Arrays for PL/Ruby
#
# Build after (or alongside) the plruby extension in the parent directory;
# the transform requires the ltree and plruby extensions at CREATE
# EXTENSION time.

MODULE_big = ltree_plruby
OBJS = ltree_plruby.o

EXTENSION = ltree_plruby
DATA = ltree_plruby--1.0.sql

# Ruby compile/link flags, discovered via RbConfig (same as ../Makefile).
RUBY ?= ruby
RUBY_ARCHHDRDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["rubyarchhdrdir"]')
RUBY_HDRDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["rubyhdrdir"]')
RUBY_LIBDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["libdir"]')
RUBY_ARCHLIBDIR := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["archlibdir"]')
RUBY_LIBARG := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["LIBRUBYARG_SHARED"]')
RUBY_LIBS := $(shell $(RUBY) -rrbconfig -e 'print RbConfig::CONFIG["LIBS"]')

PG_CPPFLAGS = -I$(RUBY_ARCHHDRDIR) -I$(RUBY_HDRDIR)
SHLIB_LINK = -L$(RUBY_LIBDIR) -L$(RUBY_ARCHLIBDIR) $(RUBY_LIBARG) $(RUBY_LIBS)

REGRESS = ltree_plruby

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
