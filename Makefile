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
			09_array_last_index_of

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
