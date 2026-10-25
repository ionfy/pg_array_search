#!/usr/bin/env perl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 9;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_array_search;');

my $result = $node->safe_psql('postgres',
    "SELECT extname FROM pg_extension WHERE extname = 'pg_array_search';"
);
is($result, 'pg_array_search', 'Extension is installed');

$result = $node->safe_psql('postgres',
    "SELECT array_contains(ARRAY[1,2,3], 2);"
);
is($result, 't', 'array_contains works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_contains_all(ARRAY[1,2,3], ARRAY[1,2]);"
);
is($result, 't', 'array_contains_all works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_find_all(ARRAY[1,2,3,2,1], 2);"
);
is($result, '{2,4}', 'array_find_all works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_filter(ARRAY[1,2,3,2,1], 'x > 2');"
);
is($result, '{3}', 'array_filter works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_reject(ARRAY[1,2,3,2,1], 'x > 2');"
);
is($result, '{1,2,2,1}', 'array_reject works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_partition(ARRAY[1,2,3,2,1], 'x > 2');"
);
is($result, '{{3,NULL,NULL,NULL},{1,2,2,1}}', 'array_partition works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_index_of(ARRAY[1,2,3,2,1], 2);"
);
is($result, '2', 'array_index_of works correctly');

$result = $node->safe_psql('postgres',
    "SELECT array_last_index_of(ARRAY[1,2,3,2,1], 2);"
);
is($result, '4', 'array_last_index_of works correctly');

$node->stop;