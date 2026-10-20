\echo Use "CREATE EXTENSION pg_array_search" to load this file. \quit

CREATE FUNCTION array_contains(anyarray, anyelement)
        RETURNS boolean
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_contains_all(anyarray, anyarray)
        RETURNS boolean
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_find_all(anyarray, anyelement)
        RETURNS integer[]
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_filter(anyarray, text)
        RETURNS anyarray
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_reject(anyarray, text)
        RETURNS anyarray
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_partition(anyarray, text)
        RETURNS anyarray
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_index_of(anyarray, anyelement)
        RETURNS integer
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;

CREATE FUNCTION array_last_index_of(anyarray, anyelement)
        RETURNS integer
        AS 'MODULE_PATHNAME'
        LANGUAGE C IMMUTABLE;