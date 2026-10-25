#!/usr/bin/env perl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 1;

my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_array_search;');

my $large_array = 'ARRAY[' . join(',', (1..10000)) . ']';
my $result_array = '{' . join(',', (5001..10000)) . '}';

my $session = $node->background_psql('postgres');

$session->query_safe("BEGIN;");
my $result = $session->query_safe("SELECT array_filter($large_array, 'x > 5000');");
$session->query_safe("ROLLBACK;");

is($result, $result_array, 'array_filter works correctly in transaction');

$session->quit;

$node->stop;