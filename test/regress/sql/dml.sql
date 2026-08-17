-- Writes that actually reach Valkey.
--
-- Every other suite asserts what the wrapper held in memory. This one asserts
-- what the server holds afterwards, read back through the PROBE rather than
-- through the wrapper's own scan: a write checked with the read path proves
-- only that the two halves agree with each other, and they share a mapping
-- layer that could be wrong in the same direction twice.

CREATE SERVER dml_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER dml_srv;

CREATE FOREIGN TABLE d_str (k text OPTIONS (key 'true'), v text)
    SERVER dml_srv OPTIONS (tabletype 'string', keyprefix 'd:');
CREATE FOREIGN TABLE d_hash (k text OPTIONS (key 'true'),
                             a text OPTIONS (field 'alpha'),
                             b text OPTIONS (field 'beta'))
    SERVER dml_srv OPTIONS (tabletype 'hash', keyprefix 'dh:');
CREATE FOREIGN TABLE d_set (k text OPTIONS (key 'true'), m text)
    SERVER dml_srv OPTIONS (tabletype 'set', keyprefix 'ds:');
CREATE FOREIGN TABLE d_zset (k text OPTIONS (key 'true'),
                             m text OPTIONS (member 'true'),
                             s text OPTIONS (score 'true'))
    SERVER dml_srv OPTIONS (tabletype 'zset', keyprefix 'dz:');
CREATE FOREIGN TABLE d_list (k text OPTIONS (key 'true'), m text)
    SERVER dml_srv OPTIONS (tabletype 'list', keyprefix 'dl:');
CREATE FOREIGN TABLE d_bin (k text OPTIONS (key 'true'), v bytea)
    SERVER dml_srv OPTIONS (tabletype 'string', keyprefix 'db:');

CREATE FUNCTION d_err(sql text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE sql;
    RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ': ' || SQLERRM;
END $$;

-- ---------------------------------------------------------------------------
-- COMMIT applies; ROLLBACK applies nothing.
--
-- The headline of the whole phase, and the first step at which it can fail.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO d_str VALUES ('d:a', 'one'), ('d:b', 'two');
COMMIT;

SELECT convert_from(key,'UTF8') AS key, keytype
FROM valkey_fdw_test_keys('dml_srv', 0, 'd:') ORDER BY 1;
SELECT convert_from(val_part,'UTF8') AS a_value
FROM valkey_fdw_test_probe('dml_srv', 0, 'GET', 'd:a');

BEGIN;
INSERT INTO d_str VALUES ('d:rolled', 'never');
ROLLBACK;
SELECT count(*) AS rolled_back_key_absent
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'd:rolled') WHERE num = 1;

-- An error anywhere in the transaction discards the whole unit, including the
-- statements that had already succeeded.
BEGIN;
INSERT INTO d_str VALUES ('d:partial1', 'x');
SELECT d_err($$INSERT INTO d_str VALUES (NULL, 'y')$$) AS mid_transaction_error;
ROLLBACK;
SELECT count(*) AS nothing_from_failed_transaction
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'd:partial1') WHERE num = 1;

-- ---------------------------------------------------------------------------
-- Every shape lands with the right Valkey type and the right contents.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO d_hash VALUES ('dh:h', 'av', 'bv');
INSERT INTO d_set  VALUES ('ds:s', 'm1'), ('ds:s', 'm2');
INSERT INTO d_zset VALUES ('dz:z', 'zm', '1.5');
INSERT INTO d_list VALUES ('dl:l', 'first'), ('dl:l', 'second');
COMMIT;

SELECT convert_from(key,'UTF8') AS key, keytype
FROM valkey_fdw_test_keys('dml_srv', 0, 'd') ORDER BY 1;

SELECT convert_from(key_part,'UTF8') AS field, convert_from(val_part,'UTF8') AS value
FROM valkey_fdw_test_probe_pairs('dml_srv', 0, 'HGETALL', 'dh:h') ORDER BY 1;
SELECT convert_from(val_part,'UTF8') AS member
FROM valkey_fdw_test_probe('dml_srv', 0, 'SMEMBERS', 'ds:s') ORDER BY 1;
SELECT dbl AS score
FROM valkey_fdw_test_probe('dml_srv', 0, 'ZSCORE', 'dz:z', 'zm');
-- A list keeps insertion order, which is the only thing distinguishing RPUSH
-- from LPUSH and is invisible to a set-like assertion.
SELECT convert_from(val_part,'UTF8') AS element
FROM valkey_fdw_test_probe('dml_srv', 0, 'LRANGE', 'dl:l', '0', '-1');

-- ---------------------------------------------------------------------------
-- Bytes survive: an embedded NUL and invalid UTF-8 both round-trip.
--
-- This is the assertion the whole binary-safe command layer exists for, and
-- the first point at which it is tested through a real write.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO d_bin VALUES ('db:nul', '\x61006200ff'::bytea);
COMMIT;
SELECT val_part = '\x61006200ff'::bytea AS bytes_survived_the_write,
       length(val_part) AS len
FROM valkey_fdw_test_probe('dml_srv', 0, 'GET', 'db:nul');

-- ---------------------------------------------------------------------------
-- UPDATE and DELETE, and a rename that keeps what the table does not map.
--
-- The rename is the case a DEL-then-SET rewrite gets wrong: the extra hash
-- field belongs to nobody's column and must still be there afterwards.
-- ---------------------------------------------------------------------------
SELECT count(*) FROM valkey_fdw_test_probe('dml_srv', 0,
       'HSET', 'dh:ren', 'alpha', 'a', 'beta', 'b', 'unmapped', 'keep me');

BEGIN;
UPDATE d_hash SET k = 'dh:renamed' WHERE k = 'dh:ren';
COMMIT;

SELECT convert_from(key_part,'UTF8') AS field, convert_from(val_part,'UTF8') AS value
FROM valkey_fdw_test_probe_pairs('dml_srv', 0, 'HGETALL', 'dh:renamed') ORDER BY 1;
SELECT count(*) AS old_key_gone
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'dh:ren') WHERE num = 1;

BEGIN;
UPDATE d_str SET v = 'ONE' WHERE k = 'd:a';
DELETE FROM d_str WHERE k = 'd:b';
COMMIT;
SELECT convert_from(val_part,'UTF8') AS updated
FROM valkey_fdw_test_probe('dml_srv', 0, 'GET', 'd:a');
SELECT count(*) AS deleted_key_gone
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'd:b') WHERE num = 1;

-- ---------------------------------------------------------------------------
-- The refusals, at COMMIT, with the session still usable afterwards.
--
-- Each is a phase-1 sentinel, so each may say "no changes were applied"
-- without qualification - and the keyspace assertion after it is what makes
-- that claim testable rather than merely asserted in a message.
-- ---------------------------------------------------------------------------
SELECT count(*) AS keys_before
FROM valkey_fdw_test_keys('dml_srv', 0, 'd');

-- A key this transaction creates that already exists: 23505.
BEGIN;
INSERT INTO d_str VALUES ('d:a', 'again');
COMMIT;

-- A key that holds another Valkey type: 42804. Seeded under this table's own
-- prefix, so the collision is one a user could actually hit rather than one
-- the keyprefix rule would have refused first.
SELECT count(*) FROM valkey_fdw_test_probe('dml_srv', 0, 'HSET', 'd:typ', 'f', 'v');
BEGIN;
INSERT INTO d_str VALUES ('d:typ', 'not a hash');
COMMIT;

-- An UPDATE of a key removed by someone else after the row was read: 40001.
BEGIN;
UPDATE d_str SET v = 'z' WHERE k = 'd:a';
SELECT count(*) FROM valkey_fdw_test_probe('dml_srv', 0, 'DEL', 'd:a');
COMMIT;

-- Nothing above changed the keyspace beyond the DEL the test itself issued.
SELECT count(*) AS keys_after
FROM valkey_fdw_test_keys('dml_srv', 0, 'd');

-- The session is still usable: every refusal above left the connection in a
-- state the pool discarded rather than reused at an unknown reply offset.
BEGIN;
INSERT INTO d_str VALUES ('d:after', 'still works');
COMMIT;
SELECT convert_from(val_part,'UTF8') AS after_refusals
FROM valkey_fdw_test_probe('dml_srv', 0, 'GET', 'd:after');

-- ---------------------------------------------------------------------------
-- The script is loaded once per connection, not once per transaction.
--
-- The steady state is a bare EVALSHA. A SCRIPT LOAD on every flush would work
-- and would cost a round trip's worth of the script's own bytes every time,
-- which is exactly the kind of regression nothing else here would notice.
-- ---------------------------------------------------------------------------
SELECT flushes AS flushes_before FROM valkey_fdw_test_flush_stats() \gset

DO $$
BEGIN
    FOR i IN 1..20 LOOP
        EXECUTE format('INSERT INTO d_str VALUES (%L, %L)', 'd:loop'||i, 'v');
    END LOOP;
END $$;

SELECT flushes > :flushes_before AS flushed_again,
       retries AS retries_should_be_zero
FROM valkey_fdw_test_flush_stats();
SELECT count(*) AS loop_keys_present
FROM valkey_fdw_test_keys('dml_srv', 0, 'd:loop');


-- ---------------------------------------------------------------------------
-- COPY FROM.
--
-- It reaches ExecForeignInsert through BeginForeignInsert, which brings no
-- plan and no fdw_private - so everything BeginForeignModify reads from the
-- plan has to be derived from the relation. That is sound only because this
-- path is INSERT: no junk row-identity columns, and every mapped column is
-- assigned by definition. While the callback was unregistered COPY was
-- refused outright, because CopyFrom passed no state to dereference.
-- ---------------------------------------------------------------------------
BEGIN;
COPY d_str (k, v) FROM STDIN;
d:c1	one
d:c2	two
d:c3	three
\.
COMMIT;
SELECT convert_from(key,'UTF8') AS key
FROM valkey_fdw_test_keys('dml_srv', 0, 'd:c') ORDER BY 1;
SELECT convert_from(val_part,'UTF8') AS c2_value
FROM valkey_fdw_test_probe('dml_srv', 0, 'GET', 'd:c2');

-- COPY is part of the transaction like any other write.
BEGIN;
COPY d_str (k, v) FROM STDIN;
d:crolled	never
\.
ROLLBACK;
SELECT count(*) AS rolled_back_copy_absent
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'd:crolled') WHERE num = 1;

-- One bad row aborts the whole COPY: the rows before it are buffered and go
-- with the transaction, which is the only behaviour consistent with the unit
-- being atomic.
BEGIN;
COPY d_str (k, v) FROM STDIN;
d:cok	x
notaprefix	y
\.
ROLLBACK;
SELECT count(*) AS partial_copy_absent
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'd:cok') WHERE num = 1;

-- COPY into a readonly table is refused by core, from IsForeignRelUpdatable,
-- before the callback is reached.
CREATE FOREIGN TABLE d_ro (k text OPTIONS (key 'true'), v text)
    SERVER dml_srv OPTIONS (tabletype 'string', keyprefix 'd:', readonly 'true');
BEGIN;
COPY d_ro (k, v) FROM STDIN;
d:ro	x
\.
ROLLBACK;
DROP FOREIGN TABLE d_ro;

-- A batch larger than one row is what GetForeignModifyBatchSize enables, and
-- COPY is the only caller that uses it. Every row must land: a batch loop
-- that returned a short count would drop rows with no error anywhere.
BEGIN;
COPY d_str (k, v) FROM STDIN;
d:b1	1
d:b2	2
d:b3	3
d:b4	4
d:b5	5
\.
SELECT live_ops AS buffered_by_copy FROM valkey_fdw_test_wbuf_stats();
COMMIT;
SELECT count(*) AS batch_keys_present
FROM valkey_fdw_test_keys('dml_srv', 0, 'd:b');

-- ---------------------------------------------------------------------------
-- DELETE ... RETURNING returns the row that was deleted (W1).
--
-- It used to return one row of NULLs: core adds its own wholerow junk column
-- for a foreign-table DELETE only when row triggers exist, and a deferred
-- write has no remote RETURNING to fetch the old row from - so core stored an
-- all-null tuple, the count was right and every column was empty. A plausible
-- empty result is the failure this project refuses everywhere else, so the
-- row is now captured locally by asking for a wholerow at plan time.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_poke('dml_srv', 'd:ret1', 'returned');

BEGIN;
DELETE FROM d_str WHERE k = 'd:ret1' RETURNING k, v;
COMMIT;
SELECT count(*) AS deleted_for_real
FROM valkey_fdw_test_probe('dml_srv', 0, 'EXISTS', 'd:ret1') WHERE num = 1;

-- ---------------------------------------------------------------------------
-- An UPDATE may clear every mapped hash field; an INSERT may not (W5).
--
-- Clearing every mapped field means HDEL them, which is a legitimate request
-- and had no way to be expressed. The unmapped field survives, which is the
-- whole reason this is not simply a DELETE. For INSERT the refusal stands: a
-- row setting none of the mapped fields would create nothing while reporting
-- one row inserted.
-- ---------------------------------------------------------------------------
SELECT count(*) FROM valkey_fdw_test_probe('dml_srv', 0,
       'HSET', 'dh:clear', 'alpha', 'a', 'beta', 'b', 'unmapped', 'survives');

BEGIN;
UPDATE d_hash SET a = NULL, b = NULL WHERE k = 'dh:clear';
COMMIT;
SELECT convert_from(key_part,'UTF8') AS field, convert_from(val_part,'UTF8') AS value
FROM valkey_fdw_test_probe_pairs('dml_srv', 0, 'HGETALL', 'dh:clear') ORDER BY 1;

SELECT d_err($$INSERT INTO d_hash VALUES ('dh:none', NULL, NULL)$$)
    AS insert_all_null_still_refused;

-- ---------------------------------------------------------------------------
-- JOINED DML: UPDATE ... FROM and DELETE ... USING.
--
-- Neither appeared anywhere in this tree, which left one line unguarded:
-- vfdw_rowid_add_one builds the junk row-identity Var with makeVar(rtindex,
-- attno, atttypid, atttypmod, attcollation, 0). In a single-relation
-- statement the target is the only range table entry, so an rtindex taken
-- from the wrong place is still right by accident, and a collation dropped
-- from the Var is never compared against anything. A join makes both wrong
-- answers reachable: the target is no longer entry one, and the junk column
-- is compared with a column from another relation.
--
-- The key column is text, so its collation is not the invalid one that an
-- uncollatable type would carry - a Var built with InvalidOid here would be
-- a collation mismatch the planner reports rather than something silently
-- returning the wrong rows.
-- ---------------------------------------------------------------------------
CREATE TABLE d_local (k text, want text);
INSERT INTO d_local VALUES ('dh:j1', 'updated'), ('dh:j2', 'deleted');

INSERT INTO d_hash VALUES ('dh:j1', 'a1', 'b1'), ('dh:j2', 'a2', 'b2');

-- The target is the FOREIGN table and the joined relation is local, which is
-- the direction that exercises the callback: the junk Var belongs to the
-- foreign side and has to name it correctly among two entries.
UPDATE d_hash SET a = d_local.want
FROM d_local WHERE d_hash.k = d_local.k AND d_local.want = 'updated';

SELECT k, a, b FROM d_hash WHERE k IN ('dh:j1', 'dh:j2') ORDER BY k;

-- RETURNING through a join, because a DELETE has no new row and its old row
-- is captured by the whole-row Var this same callback asks for.
DELETE FROM d_hash USING d_local
WHERE d_hash.k = d_local.k AND d_local.want = 'deleted'
RETURNING d_hash.k, d_hash.a;

SELECT k, a FROM d_hash WHERE k IN ('dh:j1', 'dh:j2') ORDER BY k;

-- A join that matches nothing changes nothing, and says so rather than
-- deleting the table: a row-identity Var naming the wrong range table entry
-- is one of the ways a WHERE that should match nothing matches everything.
DELETE FROM d_hash USING d_local
WHERE d_hash.k = d_local.k AND d_local.want = 'no such row';

SELECT count(*) AS survivors FROM d_hash WHERE k IN ('dh:j1', 'dh:j2');

-- THE TARGET IS NOT RANGE TABLE ENTRY ONE.
--
-- Everything above still leaves rtindex unguarded, because for a plain UPDATE
-- or DELETE the result relation is the first entry: hardcoding 1 there is
-- wrong and right at the same time. Under inheritance it is not. The parent
-- takes entry one and each child is appended after it, so the junk Var for
-- this foreign child has to name the child's own index - and naming the
-- parent's instead is a row identity pointing at another relation entirely.
CREATE TABLE d_parent (k text, a text, b text);
ALTER FOREIGN TABLE d_hash INHERIT d_parent;

UPDATE d_parent SET a = 'inherited' WHERE k = 'dh:j1';
SELECT k, a, b FROM d_hash WHERE k = 'dh:j1';

DELETE FROM d_parent WHERE k = 'dh:j1' RETURNING k, a;
SELECT count(*) AS gone FROM d_hash WHERE k = 'dh:j1';

ALTER FOREIGN TABLE d_hash NO INHERIT d_parent;
DROP TABLE d_parent;

DELETE FROM d_hash WHERE k = 'dh:j1';
DROP TABLE d_local;

-- ---------------------------------------------------------------------------
-- DELETE WITH NO WHERE, on a table that spans several keys.
--
-- Scan-everything-then-delete-everything was never run. Every DELETE asserted
-- here names a key, so the path where the scan supplies the rows and each one
-- becomes a delete - the ledger folding as many plans as the keyspace holds -
-- had no test at all. It is also the statement a user is most likely to run by
-- accident, which is the other reason to know what it does.
-- ---------------------------------------------------------------------------
INSERT INTO d_hash VALUES ('dh:w1', 'a1', 'b1'), ('dh:w2', 'a2', 'b2'),
                          ('dh:w3', 'a3', 'b3');

SELECT count(*) AS before_delete FROM d_hash;

DELETE FROM d_hash;

SELECT count(*) AS after_delete FROM d_hash;

-- And the keys are gone from the server, not merely invisible to this table:
-- a hash whose every field is removed is a key Valkey does not keep, so the
-- two statements have to agree.
SELECT count(*) AS keys_left
FROM valkey_fdw_test_keys('dml_srv', 0, 'dh:');

-- Repeating it on an empty table is not an error and writes nothing.
DELETE FROM d_hash;
SELECT count(*) AS still_empty FROM d_hash;

-- ---------------------------------------------------------------------------
-- An empty transaction costs nothing.
-- ---------------------------------------------------------------------------
SELECT calls AS calls_before, empty_returns AS empty_before
FROM valkey_fdw_test_flush_stats() \gset

BEGIN;
SELECT count(*) >= 0 AS read_only FROM d_str;
COMMIT;

SELECT calls > :calls_before          AS flush_was_called,
       empty_returns > :empty_before  AS returned_early
FROM valkey_fdw_test_flush_stats();

DROP FOREIGN TABLE d_str, d_hash, d_set, d_zset, d_list, d_bin;
DROP FUNCTION d_err(text);
DROP USER MAPPING FOR CURRENT_USER SERVER dml_srv;
DROP SERVER dml_srv;
