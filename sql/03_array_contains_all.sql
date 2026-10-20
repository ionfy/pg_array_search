SELECT array_contains_all(ARRAY[1, 2, 3, 4], ARRAY[1, 2]);
SELECT array_contains_all(ARRAY['i', 'j', 'k'], ARRAY['i', 'a']);
SELECT array_contains_all(ARRAY[1, 2, 3, NULL], ARRAY[1, NULL]);
SELECT array_contains_all(ARRAY[1, 2], ARRAY[1, NULL]);
SELECT array_contains_all(ARRAY[]::integer[], ARRAY[1, 2]);
SELECT array_contains_all(NULL::integer[], ARRAY[1, 2]);