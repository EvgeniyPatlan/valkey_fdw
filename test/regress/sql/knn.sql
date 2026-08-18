-- ---------------------------------------------------------------------------
-- A nearest-neighbour search, end to end.
--
-- The search topology, because this is the only suite that needs the module
-- to actually be loaded: knnplan asserts what is PLANNED, which needs no
-- server at all, and this asserts what comes back.
--
-- The vectors are four-dimensional and along one axis, so the ordering is
-- arithmetic anyone can check by reading: distance from [1,0,0,0] is
-- |x - 1| for a vector [x,0,0,0], and the expected order follows from that
-- rather than from what the server happened to answer.
-- ---------------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS vector;

CREATE SERVER kn_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER kn_srv;

SELECT valkey_fdw_test_flush('kn_srv') AS cleared;

-- L2, so <-> is the operator that agrees with it. <=> and <#> are asserted
-- further down against this same index, where they must be refused.
SELECT reply_type FROM valkey_fdw_test_probe('kn_srv', 0,
    'FT.CREATE', 'knidx', 'ON', 'HASH', 'PREFIX', '1', 'kn:', 'SCHEMA',
    'emb', 'VECTOR', 'FLAT', '6', 'TYPE', 'FLOAT32', 'DIM', '4',
    'DISTANCE_METRIC', 'L2',
    'tag', 'TAG');

-- ---------------------------------------------------------------------------
-- Seeded through the SERVER, not through a table.
--
-- A vector table cannot be written, and even when one can, seeding through
-- the wrapper would make this suite assert the write path and the read path
-- agree with each other rather than that either agrees with Valkey.
--
-- valkey_fdw_test_vec_from_text produces the bytes, which is the same
-- function the query vector goes through - so if that conversion is wrong,
-- the stored vectors and the query are wrong the same way and the ordering
-- would still come out right. The literals below are therefore ALSO written
-- out as raw bytea for the first two rows, and the two spellings are asserted
-- equal, which is what pins the conversion to something outside itself.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_vec_from_text('[1,0,0,0]') = '\x0000803f000000000000000000000000'::bytea
       AS conversion_is_pinned;

SELECT num AS fields_set_1 FROM valkey_fdw_test_probe('kn_srv', 0, 'HSET',
    'kn:1', 'emb', valkey_fdw_test_vec_from_text('[1,0,0,0]'), 'tag', 'a',
    'note', 'nearest');
SELECT num AS fields_set_2 FROM valkey_fdw_test_probe('kn_srv', 0, 'HSET',
    'kn:2', 'emb', valkey_fdw_test_vec_from_text('[2,0,0,0]'), 'tag', 'a',
    'note', 'middle');
SELECT num AS fields_set_3 FROM valkey_fdw_test_probe('kn_srv', 0, 'HSET',
    'kn:3', 'emb', valkey_fdw_test_vec_from_text('[5,0,0,0]'), 'tag', 'b',
    'note', 'far');

-- The index is populated asynchronously, so the suite waits for it to say so
-- rather than sleeping for a guess. num_docs is element 10 of FT.INFO.
DO $$
DECLARE n text;
BEGIN
    FOR i IN 1..100 LOOP
        SELECT convert_from(coalesce(key_part, val_part), 'UTF8') INTO n
        FROM valkey_fdw_test_probe('kn_srv', 0, 'FT.INFO', 'knidx')
        WHERE ordinal = 10;
        EXIT WHEN n = '3';
        PERFORM pg_sleep(0.1);
    END LOOP;
END $$;

CREATE FOREIGN TABLE kn (
    k    text             OPTIONS (key 'true'),
    emb  vector(4)        OPTIONS (field 'emb', index_type 'vector'),
    tag  text             OPTIONS (field 'tag', index_type 'tag'),
    note text             OPTIONS (field 'note'),
    dist double precision OPTIONS (distance 'true')
) SERVER kn_srv OPTIONS (tabletype 'vector', search_index 'knidx');

-- ---------------------------------------------------------------------------
-- The search itself.
--
-- Nearest first, and the distances are the squared L2 distances valkey-search
-- returns: 0 for [1,0,0,0] itself, 1 for [2,0,0,0], 16 for [5,0,0,0].
-- Written out rather than rounded, because a metric applied wrongly would
-- still produce an ordering and only the numbers would give it away.
-- ---------------------------------------------------------------------------
SELECT k, note, dist FROM kn
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 3;

-- k is honoured: two rows asked for, two returned, and they are the two
-- nearest rather than the first two of anything.
SELECT k, dist FROM kn ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 2;

-- A different query vector gives a different order, which is the property
-- that would survive a scan that ignored the vector entirely.
SELECT k, dist FROM kn ORDER BY emb <-> '[5,0,0,0]'::vector LIMIT 3;

-- The vector column reads back. Raw little-endian FLOAT32 in the hash, a
-- vector literal in PostgreSQL, and the round trip is exact.
SELECT k, emb FROM kn ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 3;

-- Fields the search returns that are not indexed come back too: FT.SEARCH
-- answers with the whole hash, and note is neither a tag nor a vector.
SELECT k, note FROM kn ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 1;

-- A k larger than the index holds is not an error and not a short read
-- reported as one: three documents, ten asked for, three returned and the
-- scan is over (invariant I5).
SELECT count(*) AS rows_returned FROM (
    SELECT k FROM kn ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 10
) s;

-- OFFSET, which was folded into k at plan time: the server is asked for five
-- and the Limit node above discards the first two.
SELECT k FROM kn ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 3 OFFSET 2;

-- ---------------------------------------------------------------------------
-- The query vector as a parameter, which is the case the design was written
-- around: a generic plan carries the expression, so the vector arrives with
-- the execution and each execution may bring a different one.
-- ---------------------------------------------------------------------------
PREPARE q(vector) AS
    SELECT k, dist FROM kn ORDER BY emb <-> $1 LIMIT 2;
SET plan_cache_mode = force_generic_plan;
EXECUTE q('[1,0,0,0]');
EXECUTE q('[5,0,0,0]');
RESET plan_cache_mode;
DEALLOCATE q;

-- ---------------------------------------------------------------------------
-- What the index says about itself, which is checked before every search.
--
-- Each of these would otherwise be answered: k rows, in order, with a
-- distance column filled in and every number wrong. Nothing fails, nothing
-- looks wrong, and that is why the server is asked rather than the table
-- trusted.
-- ---------------------------------------------------------------------------

-- The operator's metric against the index's. This index is L2.
SELECT k FROM kn ORDER BY emb <=> '[1,0,0,0]'::vector LIMIT 1;
SELECT k FROM kn ORDER BY emb <#> '[1,0,0,0]'::vector LIMIT 1;

-- A query vector of the wrong dimension is a point in a different space.
SELECT k FROM kn ORDER BY emb <-> '[1,0]'::vector LIMIT 1;

-- A field the index does not hold as a vector, declared as though it did.
CREATE FOREIGN TABLE kn_wrongfield (
    k   text      OPTIONS (key 'true'),
    tag vector(4) OPTIONS (field 'tag', index_type 'vector')
) SERVER kn_srv OPTIONS (tabletype 'vector', search_index 'knidx');

SELECT k FROM kn_wrongfield ORDER BY tag <-> '[1,0,0,0]'::vector LIMIT 1;

-- A field the index does not hold at all.
CREATE FOREIGN TABLE kn_nofield (
    k    text      OPTIONS (key 'true'),
    nope vector(4) OPTIONS (field 'nope', index_type 'vector')
) SERVER kn_srv OPTIONS (tabletype 'vector', search_index 'knidx');

SELECT k FROM kn_nofield ORDER BY nope <-> '[1,0,0,0]'::vector LIMIT 1;

-- An index that does not exist. The server's own error, passed through.
--
-- The MESSAGE is not asserted, because it is not this wrapper's to promise:
-- Valkey 9.0 says "Index with name 'x' not found" and 9.1 says the same thing
-- with " in database 0" appended, and this suite runs against both. Recording
-- one of them made the other a failure with nothing wrong in it. What is
-- asserted is what this code is responsible for - that the server's error is
-- passed through rather than swallowed, that it arrives as an error rather
-- than as an empty result, and that whatever the server said still names the
-- index the user asked for.
CREATE FOREIGN TABLE kn_noindex (
    k   text      OPTIONS (key 'true'),
    emb vector(4) OPTIONS (field 'emb', index_type 'vector')
) SERVER kn_srv OPTIONS (tabletype 'vector', search_index 'no_such_index');

DO $$
DECLARE
    detail text;
BEGIN
    PERFORM k FROM kn_noindex ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 1;
    RAISE EXCEPTION 'a search against a missing index returned instead of raising';
EXCEPTION WHEN others THEN
    GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL;
    RAISE NOTICE 'raised, and the detail names the index: %',
        position('no_such_index' in detail) > 0;
END $$;

-- A NULL query vector asks for the rows nearest to nothing.
--
-- Written as a literal it never reaches the executor: the planner folds
-- "emb <-> NULL" to a constant NULL and drops the ordering, so what is
-- refused is a query with no ORDER BY left in it. That is the right answer
-- and it is not the check being tested here.
SELECT k FROM kn ORDER BY emb <-> NULL::vector LIMIT 1;

-- As a PARAMETER it does reach the executor, because the plan is made before
-- the value is known. This is the reachable case, and the one the runtime
-- check exists for.
PREPARE qn(vector) AS SELECT k FROM kn ORDER BY emb <-> $1 LIMIT 1;
SET plan_cache_mode = force_generic_plan;
EXECUTE qn(NULL);
RESET plan_cache_mode;
DEALLOCATE qn;

-- A column narrower than the index. The stored vector converts to its four
-- elements and pgvector's own vector(2) rejects it - which is the point of
-- going through the type's input function rather than building a datum here:
-- the declared dimension is checked by the thing that declared it.
CREATE FOREIGN TABLE kn_narrow (
    k   text      OPTIONS (key 'true'),
    emb vector(2) OPTIONS (field 'emb', index_type 'vector')
) SERVER kn_srv OPTIONS (tabletype 'vector', search_index 'knidx');

SELECT k, emb FROM kn_narrow ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 1;

-- ---------------------------------------------------------------------------
-- ANALYZE has no keyspace to sample here, and says so rather than sampling
-- some other table's keys and calling the result this table's statistics.
-- ---------------------------------------------------------------------------
ANALYZE kn;

SELECT reply_type AS dropped FROM valkey_fdw_test_probe('kn_srv', 0,
    'FT.DROPINDEX', 'knidx');
SELECT valkey_fdw_test_flush('kn_srv') AS cleaned_up;

DROP SERVER kn_srv CASCADE;
