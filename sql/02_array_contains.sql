SELECT array_contains(ARRAY[1, 2, 3, 4], 2);
SELECT array_contains(ARRAY['i', 'j', 'k'], 'z');
SELECT array_contains(ARRAY[NULL, 1], NULL);
SELECT array_contains(ARRAY[1, 2], NULL);
SELECT array_contains(ARRAY[]::integer[], 1);
SELECT array_contains(NULL::integer[], 1);