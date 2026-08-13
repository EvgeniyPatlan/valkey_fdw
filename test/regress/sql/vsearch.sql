-- Phase 5 spike: what valkey-search actually answers.
--
-- Not a feature suite. Nothing in the FDW reads a vector yet; this exists to
-- record the reply SHAPES the design depends on, because Phase 4 designed in
-- a fallback that was then deleted unwritten when the harness turned out not
-- to be able to reach it. An hour of asking the server would have saved it.

CREATE SERVER vs FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER vs;

SELECT valkey_fdw_test_probe('vs', 0, 'FLUSHDB') IS NOT NULL AS cleared;

-- Is the module even loaded, and under what name?
SELECT convert_from(coalesce(key_part, val_part), 'UTF8') AS module_line
FROM valkey_fdw_test_probe('vs', 0, 'MODULE', 'LIST')
WHERE coalesce(key_part, val_part) IS NOT NULL;

-- Create an index over hash keys with a 4-dimensional FLAT vector.
SELECT reply_type, convert_from(coalesce(val_part, key_part), 'UTF8') AS reply
FROM valkey_fdw_test_probe('vs', 0, 'FT.CREATE', 'idx', 'ON', 'HASH',
        'PREFIX', '1', 'v:', 'SCHEMA',
        'emb', 'VECTOR', 'FLAT', '6', 'TYPE', 'FLOAT32', 'DIM', '4',
        'DISTANCE_METRIC', 'L2',
        'tag', 'TAG');

-- Three vectors, written as raw little-endian FLOAT32 - which is the first
-- fact worth pinning: a vector is BINARY and will contain NULs, so every
-- length in this path has to travel with its data (invariant I3).
SELECT valkey_fdw_test_probe('vs', 0, 'HSET', 'v:1',
        'emb', '\x0000803f0000000000000000 00000000'::bytea, 'tag', 'a')
       IS NOT NULL AS seeded_1;
SELECT valkey_fdw_test_probe('vs', 0, 'HSET', 'v:2',
        'emb', '\x000000400000000000000000 00000000'::bytea, 'tag', 'a')
       IS NOT NULL AS seeded_2;
SELECT valkey_fdw_test_probe('vs', 0, 'HSET', 'v:3',
        'emb', '\x000040400000000000000000 00000000'::bytea, 'tag', 'b')
       IS NOT NULL AS seeded_3;

SELECT pg_sleep(1);

-- The shape that matters: KNN over the whole index.
SELECT ordinal, reply_type,
       convert_from(coalesce(key_part, val_part), 'UTF8') AS part
FROM valkey_fdw_test_probe('vs', 0, 'FT.SEARCH', 'idx',
        '*=>[KNN 2 @emb $q AS dist]',
        'PARAMS', '2', 'q', '\x0000803f000000000000000000000000'::bytea,
        'DIALECT', '2')
ORDER BY ordinal;

-- And with a pre-filter, which is the case that cannot be done locally.
SELECT ordinal, reply_type,
       convert_from(coalesce(key_part, val_part), 'UTF8') AS part
FROM valkey_fdw_test_probe('vs', 0, 'FT.SEARCH', 'idx',
        '@tag:{b}=>[KNN 2 @emb $q AS dist]',
        'PARAMS', '2', 'q', '\x0000803f000000000000000000000000'::bytea,
        'DIALECT', '2')
ORDER BY ordinal;

SELECT valkey_fdw_test_probe('vs', 0, 'FT.DROPINDEX', 'idx') IS NOT NULL AS dropped;
SELECT valkey_fdw_test_probe('vs', 0, 'FLUSHDB') IS NOT NULL AS cleaned_up;

DROP USER MAPPING FOR CURRENT_USER SERVER vs;
DROP SERVER vs;
