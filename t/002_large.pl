#!/usr/bin/env perl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 8;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_array_search;');

my $large_array = 'ARRAY[' . join(',', (1..10000)) . ']';

my $result = $node->safe_psql('postgres',
    "SELECT array_contains($large_array, 5000);"
);
is($result, 't', 'array_contains works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_contains_all($large_array, ARRAY[10,500,5000]);"
);
is($result, 't', 'array_contains_all works correctly');

my $large_array_find_all = 'ARRAY[5000,' . join(',', (1..10000)) . ', 5000]';
$result = $node->safe_psql('postgres',
    "SELECT array_find_all($large_array_find_all, 5000);"
);
is($result, '{1,5001,10002}', 'array_find_all works correctly');

my $result_array = '{' . join(',', (5001..10000)) . '}';
$result = $node->safe_psql('postgres',
    "SELECT array_filter($large_array, 'x > 5000');"
);
is($result, $result_array, 'array_filter works correctly');

$result_array = '{' . join(',', (1..5000)) . '}';
$result = $node->safe_psql('postgres',
    "SELECT array_reject($large_array, 'x > 5000');"
);
is($result, $result_array, 'array_reject works correctly');

my $result_array_ok = '{' . join(',', (5001..10000)) . '}';
my $result_array_no = '{' . join(',', (1..5000)) . '}';
$result = $node->safe_psql('postgres',
    "SELECT array_partition($large_array, 'x > 5000');"
);
is($result, '{'.$result_array_ok.','.$result_array_no.'}', 'array_partition works correctly');

my $large_array_index = 'ARRAY[' . join(',', (1..10000)) . ', 5000]';
$result = $node->safe_psql('postgres',
    "SELECT array_index_of($large_array_index, 5000);"
);
is($result, '5000', 'array_index_of works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_last_index_of($large_array_index, 5000);"
);
is($result, '10001', 'array_last_index_of works correctly');

$node->stop;