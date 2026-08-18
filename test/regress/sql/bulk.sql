-- ---------------------------------------------------------------------------
-- Writes at depth, and the cap that bounds them.
--
-- The largest write asserted anywhere else in this tree is fifty rows. The
-- ledger fold, the script encoder and the flush are all shaped by the number
-- of keys in one transaction - the fold builds a plan per key, the encoder
-- writes every plan into one EVALSHA's arguments, and the flush sends it as a
-- single command - and none of that had ever been run deep enough for the
-- difference between fifty and twenty thousand to show.
--
-- The cap is the other half. A load larger than write_max_ops is refused
-- rather than partially applied, which is the right answer and a surprising
-- one to meet for the first time in production; README documents chunking,
-- and this asserts that the refusal a reader is being sent to chunk around
-- actually says so.
-- ---------------------------------------------------------------------------
CREATE SERVER bk_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER bk_srv;

CREATE FOREIGN TABLE bk (
    k text OPTIONS (key 'true'),
    v text
) SERVER bk_srv OPTIONS (tabletype 'string', keyprefix 'bk:');

-- ---------------------------------------------------------------------------
-- The refusal, at the default cap of 10000.
-- ---------------------------------------------------------------------------
INSERT INTO bk SELECT 'bk:' || i, i::text FROM generate_series(1, 10001) i;

-- REFUSED WHOLE. The transaction is buffered and applied at pre-commit, so a
-- refusal at the cap means nothing was sent - which is the property that makes
-- chunking safe advice rather than a gamble on where it stopped.
SELECT count(*) AS keys_after_refusal
FROM valkey_fdw_test_keys('bk_srv', 0, 'bk:');

-- ---------------------------------------------------------------------------
-- The same load, under a cap that admits it.
--
-- A second server rather than an ALTER, so the two caps are both readable in
-- one file and neither test depends on the other having run.
-- ---------------------------------------------------------------------------
CREATE SERVER bk_big FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', write_max_ops '40000');
CREATE USER MAPPING FOR CURRENT_USER SERVER bk_big;

CREATE FOREIGN TABLE bk_deep (
    k text OPTIONS (key 'true'),
    v text
) SERVER bk_big OPTIONS (tabletype 'string', keyprefix 'bd:');

SELECT flushes AS flushes_before FROM valkey_fdw_test_flush_stats() \gset

INSERT INTO bk_deep SELECT 'bd:' || i, i::text FROM generate_series(1, 20000) i;

-- ONE FLUSH for twenty thousand rows, which is the claim the whole write path
-- rests on: the transaction is one unit, not a unit per row or per page.
--
-- flushes, not calls: every transaction calls the pre-commit hook, and a
-- read-only one returns from it early, so `calls` counts the SELECT above too.
SELECT flushes - :flushes_before AS flushes FROM valkey_fdw_test_flush_stats();

-- Spot-checked against the SERVER rather than through the table, because a
-- table agreeing with itself is what a wrong answer does too. The whole
-- keyspace cannot be dumped for comparison - the probe stops at 10000 keys by
-- design - so three keys are read directly instead.
SELECT encode(val_part, 'escape') AS first_on_server
FROM valkey_fdw_test_probe('bk_big', 0, 'GET', 'bd:1');
SELECT encode(val_part, 'escape') AS last_on_server
FROM valkey_fdw_test_probe('bk_big', 0, 'GET', 'bd:20000');

-- Spot-checked at both ends and in the middle, because a count alone would
-- pass an encoder that wrote every plan with the same key.
SELECT k, v FROM bk_deep WHERE k IN ('bd:1', 'bd:10000', 'bd:20000') ORDER BY k;

-- Read back at depth as well: the scan pages through twenty thousand keys, so
-- this is the read path's own deep case and not only the write path's.
SELECT count(*) AS rows_read, count(DISTINCT v) AS distinct_values FROM bk_deep;

-- ---------------------------------------------------------------------------
-- The cap counts OPERATIONS, not rows of any one statement.
--
-- Two statements in one transaction reach it together, which is what makes it
-- a property of the transaction rather than of the INSERT - and what makes
-- "write in smaller transactions" the hint it is.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO bk SELECT 'bk:a' || i, i::text FROM generate_series(1, 6000) i;
INSERT INTO bk SELECT 'bk:b' || i, i::text FROM generate_series(1, 6000) i;
ROLLBACK;

-- ROLLBACK sends nothing, so the keyspace is untouched by all of that.
SELECT count(*) AS keys_after_rollback
FROM valkey_fdw_test_keys('bk_srv', 0, 'bk:');

SELECT num AS keys_removed
FROM valkey_fdw_test_probe('bk_srv', 0, 'DEL', 'bd:1', 'bd:2', 'bd:3');

-- ---------------------------------------------------------------------------
-- A DEEP DELETE, which is the case that was quadratic.
--
-- A DELETE scans the rows it is deleting, and every row it deletes goes into
-- the write buffer that the scan then reads through. The overlay index over
-- that buffer was thrown away and rebuilt whenever the buffer moved - which an
-- append does - so the scan rebuilt an index over every operation so far, once
-- per row. 16000 rows took 25 seconds and 20000 exceeded the command timeout.
--
-- ASSERTED AS A COUNT, not a duration. How long this takes is a fact about the
-- machine, and the same quadratic passes a timing assertion on a fast one. How
-- many times the index was thrown away is a fact about the code: once for the
-- statement, however many rows it touches.
-- ---------------------------------------------------------------------------
SELECT rebuilds AS rebuilds_before, extends AS extends_before
FROM valkey_fdw_test_overlay_stats() \gset

DELETE FROM bk_deep;

SELECT rebuilds - :rebuilds_before <= 2 AS rebuilt_at_most_twice,
       extends  - :extends_before  > 100 AS extended_many_times
FROM valkey_fdw_test_overlay_stats();

SELECT count(*) AS rows_left FROM bk_deep;

DROP SERVER bk_srv CASCADE;
DROP SERVER bk_big CASCADE;
