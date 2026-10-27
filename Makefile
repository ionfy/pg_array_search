MODULES = pg_array_search
EXTENSION = pg_array_search
DATA = pg_array_search--1.0.sql

REGRESS = 01_pg_array_search       \
			02_array_contains      \
			03_array_contains_all  \
			04_array_find_all      \
			05_array_filter        \
			06_array_reject        \
			07_array_partition     \
			08_array_index_of      \
			09_array_last_index_of \
			10_predicate

TAP_TESTS = t/001_basic.pl t/002_large.pl t/003_transaction.pl

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_array_search
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
