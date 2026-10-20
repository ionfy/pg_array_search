SELECT array_index_of(ARRAY[1, 3, 2, 3, 4], 3);
SELECT array_index_of(ARRAY['i', 'j', 'i', 'k'], 'a');
SELECT array_index_of(ARRAY[1, NULL, 2, 3, NULL], NULL);
SELECT array_index_of(ARRAY[1, 2], NULL);
SELECT array_index_of(ARRAY[]::integer[], 1);
SELECT array_index_of(NULL::integer[], 2);
SELECT array_index_of(ARRAY[[1], [2]], 2);