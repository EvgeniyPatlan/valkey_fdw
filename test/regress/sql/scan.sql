-- Reading rows out of Valkey.

CREATE TEMP TABLE vkout(line text);

-- Drive valkey-cli for fixture setup. Only the tests use this; the FDW
-- itself never shells out.
CREATE FUNCTION vk(cmd text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM vkout;
    EXECUTE format('COPY vkout FROM PROGRAM %L',
                   'valkey-cli -h valkey ' || cmd || ' >/dev/null 2>&1; echo ok');
END $$;

-- There is deliberately no value-returning sibling of vk() here. A shell
-- fixture cannot tell a nil from an empty array, cannot carry a NUL or a TAB,
-- and answers a failed command with a value; valkey_fdw_test_keys and
-- valkey_fdw_test_probe do all three properly and work on the topologies where
-- a plaintext valkey-cli cannot connect at all. The one assertion that used a
-- value-returning shell helper - a DBSIZE comparison - now uses the probe.

-- Only this wrapper's own EXPLAIN lines are kept. Core's "actual rows"
-- formatting changed between supported majors, and an assertion that breaks on
-- that is asserting PostgreSQL's output format rather than anything about
-- Valkey. Defined here rather than beside its first use because several blocks
-- below need it.
CREATE FUNCTION explain_valkey(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
    line text;
BEGIN
    FOR line IN EXECUTE
        'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off, BUFFERS off) ' || q
    LOOP
        IF line LIKE '%Valkey %' THEN
            RETURN NEXT btrim(line);
        END IF;
    END LOOP;
END $$;

-- One numeric property out of those lines, so a test can compare two runs
-- rather than pin a number the server is entitled to change.
CREATE FUNCTION valkey_stat(q text, prop text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    line text;
BEGIN
    FOR line IN SELECT * FROM explain_valkey(q) LOOP
        IF line LIKE prop || ':%' THEN
            RETURN btrim(substring(line from position(':' in line) + 1))::bigint;
        END IF;
    END LOOP;
    RETURN NULL;
END $$;

SELECT vk('FLUSHDB');

CREATE SERVER scan_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', scan_count '100');
CREATE USER MAPPING FOR CURRENT_USER SERVER scan_srv;

-- ---------------------------------------------------------------------------
-- A string table.
-- ---------------------------------------------------------------------------
SELECT vk('SET str:a alpha');
SELECT vk('SET str:b beta');
SELECT vk('SET str:c gamma');

CREATE FOREIGN TABLE strs (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'str:');

SELECT k, v FROM strs ORDER BY k;

-- ---------------------------------------------------------------------------
-- A hash table, one column per field.
-- ---------------------------------------------------------------------------
SELECT vk('HSET doc:1 title Alpha year 2021');
SELECT vk('HSET doc:2 title Beta year 2022');

CREATE FOREIGN TABLE docs (
    id    text OPTIONS (key 'true'),
    title text OPTIONS (field 'title'),
    year  int  OPTIONS (field 'year')
) SERVER scan_srv OPTIONS (tabletype 'hash', keyprefix 'doc:');

SELECT id, title, year FROM docs ORDER BY id;

-- A field absent from a particular key is NULL for that row, not an error.
SELECT vk('HSET doc:3 title Gamma');
SELECT id, title, year FROM docs ORDER BY id;

-- ---------------------------------------------------------------------------
-- Mixed types under one prefix.
--
-- SCAN filters by type on the server, so a string table never sees the hash
-- keys sharing its prefix. Fetching them and discarding the mismatches
-- client-side is how a wrong-type key at a page boundary ends a scan early.
-- ---------------------------------------------------------------------------
SELECT vk('SET mix:s1 one');
SELECT vk('HSET mix:h1 f v');
SELECT vk('SET mix:s2 two');

CREATE FOREIGN TABLE mix_str (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'mix:');
SELECT k, v FROM mix_str ORDER BY k;

-- The two rows above are the same whether the hash key was excluded by the
-- server or fetched and discarded here, so on their own they cannot tell a
-- server-side TYPE filter from a client-side one. The skipped counter can:
-- with TYPE on the SCAN the hash key is never returned, so nothing is
-- discarded and the count is 0. Delete the TYPE arguments from
-- vfdw_scan_build_scan and it reads 1.
SELECT explain_valkey('SELECT count(*) FROM mix_str');

-- ---------------------------------------------------------------------------
-- Multi-batch paging.
--
-- 2500 keys against scan_count 100 means roughly 25 SCAN round trips. Every
-- row must survive the page boundaries. Without a fixture larger than one
-- page the cursor loop is never executed at all, and a broken one is green.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');
DO $$
BEGIN
    PERFORM vk(format('EVAL "for i=1,2500 do server.call(''SET'', ''big:''..i, i) end return 1" 0'));
END $$;

CREATE FOREIGN TABLE bigt (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'big:');

SELECT count(*) AS rows_returned FROM bigt;
SELECT count(DISTINCT k) AS distinct_keys FROM bigt;

-- The rows above are identical at every page size, so nothing in them can tell
-- scan_count 100 from the default 1000: delete the line in vfdw_cmd/vfdw_scan
-- that reads the option, hardcode any COUNT, and this suite would not notice.
-- The round-trip count is the option's only observable consequence. Compared
-- between two servers rather than pinned to a number, because how many
-- iterations a COUNT hint costs is the server's business and changes with the
-- keyspace's internal layout - but a smaller page must always cost strictly
-- more of them.
CREATE SERVER scan_srv_wide FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', scan_count '1000');
CREATE USER MAPPING FOR CURRENT_USER SERVER scan_srv_wide;
CREATE FOREIGN TABLE bigt_wide (k text, v text) SERVER scan_srv_wide
    OPTIONS (keyprefix 'big:');

SELECT count(*) AS rows_at_default_scan_count FROM bigt_wide;
SELECT valkey_stat('SELECT count(*) FROM bigt', 'Valkey Scan Round Trips')
     > valkey_stat('SELECT count(*) FROM bigt_wide', 'Valkey Scan Round Trips')
       AS smaller_pages_cost_more_round_trips;

DROP FOREIGN TABLE bigt_wide;
DROP SERVER scan_srv_wide CASCADE;

-- ---------------------------------------------------------------------------
-- The keyspace resized under an open cursor.
--
-- SCAN's contract is at-least-once, not exactly-once: the cursor is a position
-- in the server's hash table rather than a snapshot, so a table resized while
-- the cursor is open can hand back a key it has already handed back. A reader
-- is told that. What it must never be told is a key that was there the whole
-- time and did not come back at all.
--
-- So the assertion is the DISTINCT count, and deliberately not the row count.
-- A row count would be asserting that the server does not duplicate, which is
-- the opposite of what SCAN promises, and it would go red or green according
-- to a keyspace layout no fixture controls.
--
-- The churn is what makes this able to fail at all. Ten thousand keys arriving
-- and leaving under an open cursor resize the keyspace in both directions, and
-- because they fall outside the table's MATCH they also make most pages come
-- back EMPTY with a non-zero cursor - the state I5 keeps separate from "the
-- scan is finished", and one a fixture whose every page is full never reaches.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');

DO $$
BEGIN
    PERFORM * FROM valkey_fdw_test_probe('scan_srv', 0, 'EVAL',
        'for i=1,600 do server.call("SET", "dup:"..i, i) end return 1', '0');
END $$;

CREATE FOREIGN TABLE dupt (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'dup:');

-- One churn per hundred rows, alternating, so the resizes land at several
-- cursor positions instead of at the single one a fixture happened to pick;
-- which of them a server turns into a duplicate is the server's business.
-- The counter is a sequence because it is read from inside a qual, and a
-- counter in a table would have this statement updating a row it is also
-- reading - a different test, and a flakier one.
CREATE TEMP SEQUENCE churn_seq;

CREATE FUNCTION churn(k text) RETURNS boolean
LANGUAGE plpgsql VOLATILE AS $$
DECLARE
    n bigint := nextval('churn_seq');
BEGIN
    IF n % 100 <> 1 THEN
        RETURN true;
    END IF;

    -- Through the probe rather than the shell helper: vk() reports ok for a
    -- command the server rejected, so a typo in the Lua below would leave the
    -- keyspace unresized and this whole case asserting nothing.
    IF (n / 100) % 2 = 0 THEN
        PERFORM * FROM valkey_fdw_test_probe('scan_srv', 0, 'EVAL',
            'for i=1,10000 do server.call("SET", "pad:"..i, "1") end return 1',
            '0');
    ELSE
        PERFORM * FROM valkey_fdw_test_probe('scan_srv', 0, 'EVAL',
            'for i=1,10000 do server.call("DEL", "pad:"..i) end return 1',
            '0');
    END IF;

    RETURN true;
END $$;

-- The pairing is checked in the same pass and survives a duplicate, because a
-- key returned twice brings its own value twice. A row carrying another key's
-- value is a crossed reply stream, which is a different defect and would pass
-- a count of any kind.
SELECT count(DISTINCT k) AS keys_present_throughout,
       count(*) FILTER (WHERE v <> substr(k, 5)) AS mispaired
  FROM dupt WHERE churn(k);

DROP FOREIGN TABLE dupt;

-- ---------------------------------------------------------------------------
-- Rescan.
--
-- With materialisation off the inner side is re-executed for every outer
-- row, so each pass must return the whole set. The obvious implementation
-- resets only its row counter and replays whichever page it happened to
-- hold.
-- ---------------------------------------------------------------------------
SET enable_material = off;
SET enable_memoize = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;

SELECT vk('FLUSHDB');
SELECT vk('SET r:1 one');
SELECT vk('SET r:2 two');
SELECT vk('SET r:3 three');

CREATE FOREIGN TABLE rt (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'r:');

-- Three outer rows x three inner rows: nine pairs only if every rescan
-- returns the full set.
SELECT count(*) AS nested_loop_pairs
FROM (VALUES (1), (2), (3)) o(n), rt;

-- Two foreign tables on the SAME server and user, joined by a nested loop, so
-- both scans are reading at once. A valkeyContext is one reply stream and
-- replies arrive in send order, so two readers pipelining onto one context
-- take each other's replies. Here that surfaced as "SCAN failed: Valkey
-- answered with a string reply" - the outer scan receiving the inner's GET.
-- Between two string tables the shapes agree and it would instead have
-- returned the wrong value in silence, which is why a reader now holds a
-- connection to itself.
SELECT vk('SET r2:1 one');
SELECT vk('SET r2:2 two');
SELECT vk('SET r2:3 three');

CREATE FOREIGN TABLE rt2 (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'r2:');

-- The three assertions below say something about concurrent scans only while
-- the plan really is a nested loop with a non-materialised inner side.
-- enable_material = off is a cost penalty and not a prohibition, so the
-- planner stays free to insert a Materialize above rt2 whenever it prices
-- better - and PostgreSQL has changed such pricing between majors. If it ever
-- does, rt2 is scanned once, the two scans are never concurrent, and all three
-- counts stay byte-identical while covering nothing. Pinning the plan makes
-- that a diff instead.
EXPLAIN (COSTS off) SELECT count(*) FROM rt, rt2;

SELECT count(*) AS two_table_nested_loop_pairs FROM rt, rt2;
SELECT count(DISTINCT rt.k || '/' || rt2.k) AS distinct_pairs FROM rt, rt2;

-- The values must belong to their own key, not merely be present. A crossed
-- stream that happened to match shapes would pass a count and fail this.
SELECT rt.k, rt.v, rt2.k, rt2.v FROM rt, rt2 ORDER BY rt.k, rt2.k LIMIT 4;

-- ---------------------------------------------------------------------------
-- A subtransaction abort releases the leases IT took, and no others.
--
-- The pool's subxact hook frees leases so an aborted savepoint cannot strand
-- a slot. Freeing every lease instead would hand a slot back while a scan
-- above the savepoint is still reading through it, and the next scan would
-- take that slot and cross the two reply streams - the defect the block above
-- exists for, reintroduced through the cleanup path.
--
-- A cursor is what makes this visible: it holds its lease across statements,
-- so a subtransaction can begin and abort while one scan is still open.
--
-- The assertion is the LEASE COUNT, not the rows. A wrongly released lease
-- corrupts data only if the two readers have commands in flight at the same
-- instant, and between statements a cursor's batch is quiesced - so the
-- obvious test (fetch, abort, scan something else, fetch the rest) returns
-- correct rows either way. That version was written first and proved nothing:
-- removing the guard left it green. Asserting the pool state is what
-- distinguishes the two behaviours.
-- ---------------------------------------------------------------------------
BEGIN;
DECLARE lease_c CURSOR FOR SELECT k, v FROM rt ORDER BY k;
FETCH 1 FROM lease_c;
SELECT leased AS leased_while_cursor_open FROM valkey_fdw_test_pool_stats();

-- A subtransaction that takes no lease of its own, and aborts.
DO $$ BEGIN PERFORM 1/0; EXCEPTION WHEN division_by_zero THEN NULL; END $$;

-- The cursor is still reading, so its slot is still its own.
SELECT leased AS leased_after_unrelated_abort FROM valkey_fdw_test_pool_stats();

FETCH ALL FROM lease_c;
COMMIT;

-- Closing the cursor returns it. pool.sql covers the other half - that a
-- subtransaction which DID take a lease and then errored gets it back - so
-- between the two the guard is pinned in both directions.
SELECT leased AS leased_after_commit FROM valkey_fdw_test_pool_stats();

RESET enable_material;
RESET enable_memoize;
RESET enable_hashjoin;
RESET enable_mergejoin;


-- ---------------------------------------------------------------------------
-- Key equality becomes a single fetch.
--
-- Deciding a qual names this table's key is where qual pushdown goes wrong,
-- and every way it does is silent: indexing the tuple descriptor with an
-- unvalidated varattno, never checking which relation the Var belongs to,
-- matching equality by a hardcoded pg_proc OID, ignoring the commuted form.
-- ---------------------------------------------------------------------------
SELECT vk('SET q:1 one');
SELECT vk('SET q:2 two');
SELECT vk('SET q:3 three');

CREATE FOREIGN TABLE qt (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'q:');

SELECT k, v FROM qt WHERE k = 'q:2';

-- The commuted form must be recognised too.
SELECT k, v FROM qt WHERE 'q:3' = k;

-- A key that does not exist yields nothing rather than an error.
SELECT count(*) AS missing_key_rows FROM qt WHERE k = 'q:nope';

-- A predicate on the value column is not a key lookup, so the scan still
-- covers the keyspace and the executor filters.
SELECT k, v FROM qt WHERE v = 'one';

-- A key outside the table's own keyprefix is not in the table, so it must
-- yield nothing. Fetching it would fill the key column with the very value
-- the recheck compares against, and the row would pass a filter meant to
-- exclude it. r:1 exists, but not under this table's prefix.
SELECT count(*) AS out_of_scope_rows FROM qt WHERE k = 'r:1';

-- ---------------------------------------------------------------------------
-- IN becomes one batch of fetches.
-- ---------------------------------------------------------------------------
SELECT k, v FROM qt WHERE k IN ('q:1', 'q:3') ORDER BY k;

-- Written as = ANY, which is the same node.
SELECT k, v FROM qt WHERE k = ANY (ARRAY['q:2', 'q:3']) ORDER BY k;

-- A repeated key names one row, not two. Fetching it twice would duplicate
-- it, since each fetch produces a row of its own.
SELECT count(*) AS duplicate_key_rows FROM qt WHERE k IN ('q:1', 'q:1');

-- NULL is never equal to anything, so it contributes no key.
SELECT k FROM qt WHERE k IN ('q:1', NULL) ORDER BY k;

-- Keys outside the prefix are dropped from the list, and a list left with
-- nothing returns nothing rather than falling back to a keyspace walk.
SELECT k FROM qt WHERE k IN ('q:2', 'r:1') ORDER BY k;
SELECT count(*) AS all_out_of_scope FROM qt WHERE k IN ('r:1', 'r:2');

-- Missing keys are skipped without ending the batch: the ones after must
-- still appear.
SELECT k FROM qt WHERE k IN ('q:nope', 'q:1', 'q:gone', 'q:3') ORDER BY k;

-- ---------------------------------------------------------------------------
-- LIKE narrows the SCAN pattern.
--
-- The clause stays in the plan and still filters, so this only decides how
-- much of the keyspace is walked to get there.
-- ---------------------------------------------------------------------------
SELECT count(*) AS like_rows FROM qt WHERE k LIKE 'q:%';
SELECT k FROM qt WHERE k LIKE 'q:1%' ORDER BY k;

-- The rows above would be the same with no narrowing at all, so assert the
-- pattern the scan will actually use. 'q:1*' is the LIKE prefix extending the
-- table's own; the others are what the table alone allows.
EXPLAIN (COSTS off) SELECT k FROM qt WHERE k LIKE 'q:1%';
EXPLAIN (COSTS off) SELECT k FROM qt WHERE k LIKE '%:1';
EXPLAIN (COSTS off) SELECT k FROM qt WHERE k ILIKE 'Q:%';
EXPLAIN (COSTS off) SELECT k FROM qt WHERE k LIKE 'r:%';

-- An escaped wildcard is a literal, so it belongs to the prefix.
EXPLAIN (COSTS off) SELECT k FROM qt WHERE k LIKE 'q:1\%a%';

-- A list narrowed to nothing is visible as such.
EXPLAIN (COSTS off) SELECT k FROM qt WHERE k IN ('r:1', 'r:2');

-- A pattern that starts with a wildcard has no usable prefix.
SELECT count(*) AS leading_wildcard FROM qt WHERE k LIKE '%:1';

-- A LIKE prefix that leaves the table's own prefix can match nothing. The
-- scan stays confined to the table and the recheck returns no rows.
SELECT count(*) AS divergent_prefix FROM qt WHERE k LIKE 'r:%';

-- ILIKE is not a byte prefix and must not narrow anything. Q: is not q:, so
-- pushing this down as a prefix would lose the rows it should return.
SELECT count(*) AS ilike_rows FROM qt WHERE k ILIKE 'Q:%';

DROP FOREIGN TABLE qt;

-- ---------------------------------------------------------------------------
-- Collection types.
--
-- A list, set or zset key holds many members, and each becomes a row. That
-- is a more useful shape than one row per key with the whole collection
-- packed into a text[], which must be unnested before a member can be
-- filtered or joined on.
-- ---------------------------------------------------------------------------
SELECT vk('RPUSH lst:a one two three');
SELECT vk('RPUSH lst:b solo');

CREATE FOREIGN TABLE lists (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER scan_srv OPTIONS (tabletype 'list', keyprefix 'lst:');

-- List order is significant and must be preserved within each key.
SELECT k, m FROM lists ORDER BY k;

SELECT vk('SADD st:x a b c');

CREATE FOREIGN TABLE sets (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER scan_srv OPTIONS (tabletype 'set', keyprefix 'st:');

SELECT k, m FROM sets ORDER BY k, m;

SELECT vk('ZADD z:1 1 alpha 2 beta 3 gamma');

CREATE FOREIGN TABLE zsets (
    k text             OPTIONS (key 'true'),
    m text             OPTIONS (member 'true'),
    s double precision OPTIONS (score 'true')
) SERVER scan_srv OPTIONS (tabletype 'zset', keyprefix 'z:');

SELECT k, m, s FROM zsets ORDER BY s;

-- Valkey deletes a collection key when its last element is removed, so this
-- pair does NOT build an empty collection - it builds an absent key, which
-- SCAN TYPE list never returns at all. That is worth its own assertion (the
-- keys around it must still appear), but the comment this replaced claimed it
-- covered the empty-collection reply, and it never could: with no such key in
-- the keyspace the zero-element branch was never executed.
SELECT vk('RPUSH lst:c gone');
SELECT vk('LPOP lst:c');
SELECT vk('RPUSH lst:d after');
SELECT count(*) AS rows_after_deleted_key FROM lists;

-- The empty-collection reply itself. It needs a table that NAMES its keys
-- instead of discovering them, because discovery cannot return a key that is
-- not there: LRANGE over an absent key answers with an empty array, which is
-- not a skippable shape - it is an ordinary container with nothing in it.
--
-- The absent key sits between two that exist, so the count and the counter
-- catch different mistakes. Turning the skip's `continue` in vfdw_scan_fetch
-- into an end-of-scan reads 1 row instead of 2. Deleting
-- vfdw_scan_reply_is_absent leaves 2 rows but reports 0 skipped, because for a
-- LIST an empty reply falls through to the member loop and quietly yields
-- nothing - which is the same silence a hash table turned into a phantom row.
SELECT vk('RPUSH le:1 first');
SELECT vk('RPUSH le:3 third');
SELECT vk('SADD lkset le:1 le:2 le:3');
CREATE FOREIGN TABLE lempty (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER scan_srv OPTIONS (tabletype 'list', keyset 'lkset');
SELECT k, m FROM lempty ORDER BY k;
SELECT explain_valkey('SELECT count(*) FROM lempty');
DROP FOREIGN TABLE lempty;

DROP FOREIGN TABLE lists, sets, zsets;

-- ---------------------------------------------------------------------------
-- A key whose name contains a NUL byte (invariant I3).
--
-- Valkey keys are binary-safe byte strings. Discovery records each key's
-- length alongside its bytes, so anything that shortens the bytes without
-- shortening the length leaves every later use reading past the allocation -
-- and drops the row entirely, which is the visible half.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');
DO $$
BEGIN
    PERFORM vk('EVAL "server.call(''SET'', ''nul:a\0b'', ''v1'') '
               'server.call(''SET'', ''nul:c\0d'', ''v2'') '
               'server.call(''SET'', ''nul:ok'', ''v3'') return 1" 0');
END $$;

-- A text key column cannot hold a NUL byte, so such a key must be refused
-- loudly. Silently returning only the keys that happen to be NUL-free is the
-- failure this replaced: the count read 1 of 3.
CREATE FOREIGN TABLE nult (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'nul:');
SELECT count(*) FROM nult;

-- Declared bytea, the same three keys arrive whole, NUL and all. This is the
-- half that proves the bytes survived discovery rather than merely that the
-- text column rejected them.
CREATE FOREIGN TABLE nulb (k bytea, v text) SERVER scan_srv
    OPTIONS (keyprefix 'nul:');
SELECT count(*) AS nul_keys_returned FROM nulb;
SELECT k, v FROM nulb ORDER BY k;
SELECT length(k) AS key_bytes FROM nulb ORDER BY length(k), k;

-- The other half of the key column's contract: a LOOKUP, on the same table.
-- Discovery and lookup used to be tested on disjoint tables - every equality
-- and IN assertion above runs against a text key column, and nulb was only
-- ever SELECTed without a qual - so the lookup half of binary-key support had
-- never executed, and deleting the pushdown's type handling entirely would
-- have left this file byte-identical.
--
-- A bytea key column holds the key's own bytes. Rendering that Datum for the
-- plan goes through byteaout, which emits the 13-character hex TEXT
-- '\x6e756c3a6f6b' - not the six bytes the read path stored and not the bytes
-- the write path would send. So the lookup names a key that does not exist,
-- and under a keyprefix the refinement drops the hex spelling as out of scope,
-- leaving an empty key list that ends the scan at once: zero rows, no error,
-- for a row a full scan returns. The pushdown therefore refuses a key qual on
-- a binary column and the executor compares bytea to bytea instead.
SELECT k, v FROM nulb WHERE k = '\x6e756c3a6f6b'::bytea;
SELECT k, v FROM nulb WHERE k = '\x6e756c3a610062'::bytea;
SELECT count(*) AS bytea_keys_by_list FROM nulb
    WHERE k IN ('\x6e756c3a6f6b'::bytea, '\x6e756c3a610062'::bytea);

-- The EXPLAIN is what proves the fallback happened, rather than the pushdown
-- having been fixed by accident: asserting only that the rows come back would
-- pass against a pushdown that had learned to carry raw bytes, which is a
-- different change from this one.
EXPLAIN (COSTS off) SELECT * FROM nulb WHERE k = '\x6e756c3a6f6b'::bytea;
EXPLAIN (COSTS off) SELECT * FROM nulb
    WHERE k IN ('\x6e756c3a6f6b'::bytea, '\x6e756c3a610062'::bytea);

DROP FOREIGN TABLE nult, nulb;

-- ---------------------------------------------------------------------------
-- Key columns whose type does not reproduce the key's own bytes.
--
-- The read path decodes a key with the column's INPUT function, so an int4 key
-- column turns the key '007' into 7. A pushdown renders 7 back with the OUTPUT
-- function and fetches the key '7', which is not there - so a full scan
-- returns the row and "WHERE k = 7" returns nothing.
--
-- Checking in(out(c)) == c does not catch this, which is why the guard is on
-- the type and not on the constant: in('7') IS 7, so that check passes and the
-- row is still lost. The condition that actually matters is out(in(K)) == K
-- for every key K the table holds, and plan time cannot enumerate those.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');
SELECT vk('SET 007 seven');
SELECT vk('SET 8 eight');

CREATE FOREIGN TABLE numk (k int, v text) SERVER scan_srv;

-- '007' and '8' are two keys; the first decodes to 7.
SELECT k, v FROM numk ORDER BY k;
SELECT k, v FROM numk WHERE k = 7;
SELECT k, v FROM numk WHERE k = 8;
SELECT count(*) AS int_key_by_list FROM numk WHERE k IN (7, 8);
EXPLAIN (COSTS off) SELECT * FROM numk WHERE k = 7;
DROP FOREIGN TABLE numk;

-- bpchar pads to its declared width, so a char(10) constant naming 'abc' is
-- 'abc' plus seven spaces - a key that does not exist - while the scan returns
-- the key 'abc' and bpchar equality, which ignores trailing spaces, accepts it.
SELECT vk('FLUSHDB');
SELECT vk('SET abc three');
CREATE FOREIGN TABLE padk (k char(10), v text) SERVER scan_srv;
SELECT k, v FROM padk WHERE k = 'abc'::char(10);
EXPLAIN (COSTS off) SELECT * FROM padk WHERE k = 'abc'::char(10);
DROP FOREIGN TABLE padk;

-- ---------------------------------------------------------------------------
-- A non-deterministic collation.
--
-- Under one, two different byte strings compare equal, so the key a constant
-- names is not the only key satisfying the clause. The LIKE path has refused
-- this since it was written; the equality path had the same hole and no guard,
-- and the symptom was that the answer depended on whether the constant folded
-- - a point lookup and no rows for the literal, a keyspace scan and the row
-- for a form the planner could not fold.
--
-- Asserted as the two forms against EACH OTHER rather than as one hardcoded
-- row, so the assertion stays meaningful if someone deletes the guard and
-- re-records: both would then have to change together, and they cannot.
-- ---------------------------------------------------------------------------
CREATE COLLATION vfdw_ci (provider = icu, locale = 'und-u-ks-level2',
                          deterministic = false);
SELECT vk('FLUSHDB');
SELECT vk('SET ndq2 lower');

CREATE FOREIGN TABLE ndt (k text COLLATE vfdw_ci, v text) SERVER scan_srv;
SELECT k, v FROM ndt WHERE k = 'NDQ2';
SELECT k, v FROM ndt WHERE k = (SELECT 'NDQ2'::text);
SELECT (SELECT count(*) FROM ndt WHERE k = 'NDQ2')
     = (SELECT count(*) FROM ndt WHERE k = (SELECT 'NDQ2'::text))
       AS folded_and_unfolded_agree;
EXPLAIN (COSTS off) SELECT * FROM ndt WHERE k = 'NDQ2';
DROP FOREIGN TABLE ndt;
DROP COLLATION vfdw_ci;

-- ---------------------------------------------------------------------------
-- A hash key that is not there (invariant I5).
--
-- GET answers a missing key with a nil and LRANGE/SMEMBERS/ZRANGE with an
-- empty container that yields no rows, but HGETALL answers with an empty MAP,
-- which is neither. So the tuple builder filled the key column with the name
-- that had been asked for and left every mapped field NULL: a row invented for
-- a key that does not exist. EXISTS answered true for any name, an IN list
-- returned one row per name present or not, and a keyset hash table with a
-- stale member returned a phantom row with no qual at all - which ANALYZE then
-- counted.
--
-- The boundary matters as much as the fix: a hash that DOES exist and holds
-- only fields this table does not map answers with a NON-empty reply and must
-- still produce a row with those fields NULL.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');
SELECT vk('HSET h:1 title Alpha');
SELECT vk('HSET h:2 other 1');

CREATE FOREIGN TABLE ht (
    k     text OPTIONS (key 'true'),
    title text OPTIONS (field 'title')
) SERVER scan_srv OPTIONS (tabletype 'hash', keyprefix 'h:');

-- Two keys exist. h:2 holds no field this table maps and is still a row.
SELECT k, title FROM ht ORDER BY k;

SELECT count(*) AS missing_hash_rows FROM ht WHERE k = 'h:nope';
SELECT EXISTS (SELECT 1 FROM ht WHERE k = 'h:zzz') AS exists_for_absent_key;
SELECT k FROM ht WHERE k IN ('h:1', 'h:nope', 'h:gone') ORDER BY k;

-- An absent key is counted as skipped, exactly as an expired or wrong-type one
-- is, rather than silently producing nothing - which is the difference between
-- "the key was not there" and "the wrapper dropped it".
SELECT explain_valkey('SELECT count(*) FROM ht WHERE k = ''h:nope''');

-- With no qual at all: a keyset hash table carrying a member whose key is
-- gone. This is the shape that needed no pushdown and no exotic type to go
-- wrong, and the one ANALYZE inherited.
SELECT vk('SADD hkset h:1 h:gone');
CREATE FOREIGN TABLE hk (
    k     text OPTIONS (key 'true'),
    title text OPTIONS (field 'title')
) SERVER scan_srv OPTIONS (tabletype 'hash', keyset 'hkset');
SELECT count(*) AS keyset_hash_rows FROM hk;
SELECT k, title FROM hk ORDER BY k;

ANALYZE hk;
SELECT reltuples AS hash_keyset_reltuples FROM pg_class WHERE relname = 'hk';

DROP FOREIGN TABLE ht, hk;

-- ---------------------------------------------------------------------------
-- Keyset discovery.
--
-- The table's keys are named in a set of their own, so the keyspace is never
-- walked. SSCAN accepts no TYPE filter, so a member pointing at a key of
-- another type is filtered when its value comes back - and, critically, that
-- must not end the scan.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');
SELECT vk('SET ks:1 one');
SELECT vk('SET ks:2 two');
SELECT vk('SET ks:3 three');
SELECT vk('SADD kset ks:1 ks:2 ks:3');

CREATE FOREIGN TABLE kt (k text, v text) SERVER scan_srv
    OPTIONS (keyset 'kset');

SELECT k, v FROM kt ORDER BY k;

-- A member naming a key that no longer exists is skipped, not fatal - and the
-- members AFTER it still appear, which is the half the fixture used to leave
-- vacuous. Both bad members used to be appended last, so a wrapper that ENDED
-- its scan at the first skippable reply had already emitted every row and
-- reported the same count as a correct one. A good member now follows each bad
-- one, so ending the scan there reads 3 instead of 4 and then 5.
SELECT vk('SADD kset ks:gone');
SELECT vk('SET ks:4 four');
SELECT vk('SADD kset ks:4');
SELECT count(*) AS rows_with_dangling_member FROM kt;

-- A member naming a key of the wrong type is skipped too, and again the
-- members after it still appear.
SELECT vk('HSET ks:hash f v');
SELECT vk('SADD kset ks:hash');
SELECT vk('SET ks:5 five');
SELECT vk('SADD kset ks:5');
SELECT count(*) AS rows_with_wrong_type_member FROM kt;
SELECT k, v FROM kt ORDER BY k;

DROP FOREIGN TABLE kt;

-- ---------------------------------------------------------------------------
-- Singleton tables.
--
-- The table names its one key in its options, so nothing is discovered at
-- all. The row shape is unchanged: a singleton set still yields one row per
-- member, exactly as the same table scoped by prefix would.
-- ---------------------------------------------------------------------------
SELECT vk('SADD tags red green blue');

CREATE FOREIGN TABLE tags (t text) SERVER scan_srv
    OPTIONS (tabletype 'set', singleton_key 'tags');

SELECT t FROM tags ORDER BY t;

-- A key column is optional, and is filled with the fixed key name.
CREATE FOREIGN TABLE tags_k (
    k text OPTIONS (key 'true'),
    t text OPTIONS (member 'true')
) SERVER scan_srv OPTIONS (tabletype 'set', singleton_key 'tags');

SELECT k, t FROM tags_k ORDER BY t;

-- A qual on that key column must not displace the singleton key. Reading
-- some other key here would fill k with the value the recheck tests against,
-- so rows from the wrong key would pass a filter meant to exclude them.
--
-- The key named below has to EXIST for that to be observable: with no key
-- called 'other' in the keyspace, a displaced read returns an empty set and
-- counts 0, which is what a correct wrapper counts too. With it present, a
-- displaced read fills k with 'other' for three members, the recheck admits
-- them, and the count reads 3.
SELECT vk('SADD other x y z');
SELECT count(*) AS rows_for_other_key FROM tags_k WHERE k = 'other';
SELECT count(*) AS rows_for_own_key FROM tags_k WHERE k = 'tags';

-- A singleton string is one row.
SELECT vk('SET motd hello');
CREATE FOREIGN TABLE motd (v text) SERVER scan_srv
    OPTIONS (singleton_key 'motd');
SELECT v FROM motd;

-- A singleton hash reads named fields, same as a prefixed hash table would.
SELECT vk('HSET cfg host localhost port 6379');
CREATE FOREIGN TABLE cfg (
    host text OPTIONS (field 'host'),
    port int  OPTIONS (field 'port')
) SERVER scan_srv OPTIONS (tabletype 'hash', singleton_key 'cfg');
SELECT host, port FROM cfg;

-- A singleton key of the wrong type contributes no rows, the same as a
-- wrong-type key found by a scan. It is counted, not silently dropped.
CREATE FOREIGN TABLE motd_as_set (m text OPTIONS (member 'true'))
    SERVER scan_srv OPTIONS (tabletype 'set', singleton_key 'motd');
SELECT count(*) AS wrong_type_rows FROM motd_as_set;

-- ---------------------------------------------------------------------------
-- EXPLAIN reports which access path was chosen.
--
-- The strategy is decided by the planner and carried in the plan, so it shows
-- without executing anything.
-- ---------------------------------------------------------------------------
EXPLAIN (COSTS off) SELECT * FROM strs;
EXPLAIN (COSTS off) SELECT * FROM strs WHERE k = 'str:a';
EXPLAIN (COSTS off) SELECT * FROM strs WHERE k IN ('str:a', 'str:b');
EXPLAIN (COSTS off) SELECT * FROM strs WHERE k LIKE 'str:a%';
EXPLAIN (COSTS off) SELECT * FROM tags;

-- Under ANALYZE the skipped-key count is a measurement of what the scan
-- discarded, which is otherwise invisible: the rows are simply absent.
--
-- r:1 is recreated here because the FLUSHDB above destroyed it. Without that
-- the "found its key" probe below reported 'Valkey Keys Skipped: 1' - the
-- opposite of what its comment claimed - and was byte-for-byte identical to
-- the r:gone probe it exists to be contrasted against, so the whole Key-Lookup
-- half of the counter was unprotected while reading green.
SELECT vk('SET r:1 one');

-- A lookup that finds its key skips nothing...
SELECT explain_valkey('SELECT * FROM rt WHERE k = ''r:1''');

-- ...while one whose key holds the wrong type reports exactly what it
-- discarded. Without this the two are indistinguishable from the outside:
-- both simply return no rows.
SELECT explain_valkey('SELECT count(*) FROM motd_as_set');
SELECT explain_valkey('SELECT * FROM rt WHERE k = ''r:gone''');

-- ---------------------------------------------------------------------------
-- ANALYZE.
--
-- Without statistics every foreign table plans against the same invented row
-- count, so a join with a Valkey table on one side is chosen from a number
-- nobody measured.
-- ---------------------------------------------------------------------------
SELECT vk('FLUSHDB');
DO $$
BEGIN
    PERFORM vk(format(
        'EVAL "for i=1,700 do server.call(''SET'', ''an:''..i, i) end return 1" 0'));
END $$;

CREATE FOREIGN TABLE an (k text, v text) SERVER scan_srv
    OPTIONS (keyprefix 'an:');

-- What the planner believes, without depending on cost formulas that differ
-- between majors.
CREATE FUNCTION plan_rows(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    j json;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
    RETURN (j -> 0 -> 'Plan' ->> 'Plan Rows')::bigint;
END $$;

-- Before ANALYZE the estimate is the placeholder, whatever the table holds.
SELECT reltuples AS before_analyze FROM pg_class WHERE relname = 'an';
SELECT plan_rows('SELECT * FROM an') AS estimate_before;

ANALYZE an;

-- The sample scan visits every row, so this is a count and not an estimate.
SELECT reltuples AS after_analyze FROM pg_class WHERE relname = 'an';

-- And it reaches the planner: the estimate now tracks the table rather than
-- the constant. This is the whole point of the exercise - an estimate stored
-- in pg_class that never reached a plan would change nothing.
SELECT plan_rows('SELECT * FROM an') AS estimate_after;

-- ANALYZE and a plain SELECT share one producer - vfdw_analyze.c drives
-- vfdw_scan_fetch, which is what the SELECT reads through - so comparing them
-- to each other checks the sampling wrapper and nothing about the producer: a
-- producer that dropped rows would drop them on both sides. The number that
-- can contradict the wrapper is the server's own, so it is asked directly.
SELECT (SELECT count(*) FROM an) = 700 AS scan_agrees_with_sampler;

-- The number that can contradict the wrapper has to come from the server, but
-- it has to be scoped the same way the table is. DBSIZE is not: it counts a
-- whole logical database, so the old form here was really an assertion that
-- the FLUSHDB two blocks up had worked, and it moved whenever another suite
-- left a key behind. This asks the server for exactly the keys this table
-- covers, which is independent of the wrapper and independent of every other
-- suite.
SELECT count(*) AS server_prefix_keys
  FROM valkey_fdw_test_keys('scan_srv', 0, 'an:');
SELECT (SELECT count(*) FROM valkey_fdw_test_keys('scan_srv', 0, 'an:'))
     = (SELECT count(*) FROM an) AS wrapper_agrees_with_the_server;

-- Statistics are collected per column too, so a predicate on the value is
-- estimated from what is there rather than from a default.
SELECT count(*) AS stats_columns FROM pg_stats
WHERE tablename = 'an' AND attname IN ('k', 'v');

-- A table whose keys are gone analyzes to zero rather than failing.
SELECT vk('FLUSHDB');
ANALYZE an;
SELECT reltuples AS after_flush FROM pg_class WHERE relname = 'an';

DROP FOREIGN TABLE an;
DROP FOREIGN TABLE tags, tags_k, motd, cfg, motd_as_set;

SELECT vk('FLUSHDB');
DROP FOREIGN TABLE strs, docs, mix_str, bigt, rt;
DROP SERVER scan_srv CASCADE;
DROP FUNCTION vk(text);
DROP FUNCTION valkey_stat(text, text);
DROP FUNCTION explain_valkey(text);
DROP FUNCTION plan_rows(text);

-- churn is a suite-local helper and pg_regress shares one database across
-- files, so leaving it defined leaks a name into every suite that runs after
-- this one.
DROP FUNCTION churn(text);
