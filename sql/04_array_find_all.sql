SELECT array_find_all(ARRAY[1, 3, 2, 3, 4], 3);
SELECT array_find_all(ARRAY['i', 'j', 'k'], 'a');
SELECT array_find_all(ARRAY[NULL, 1, 2, 3, NULL], NULL);
SELECT array_find_all(ARRAY[1, 2], NULL);
SELECT array_find_all(ARRAY[]::integer[], 1);
SELECT array_find_all(NULL::integer[], 2);
SELECT array_find_all(ARRAY[[1], [2]], 1);