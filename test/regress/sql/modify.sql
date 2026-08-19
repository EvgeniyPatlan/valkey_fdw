-- The modify callback surface: row identity, the refusal gates, and EXPLAIN.
--
-- Nothing here reaches Valkey with a write. Every assertion is about what the
-- statement produced in the buffer, what it refused, or what it printed.

CREATE SERVER md_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER md_srv;

-- A second server on the same host. Same reachable Valkey, different
-- serverid: the refusal is about the unit, not about reachability.
CREATE SERVER md_srv2 FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER md_srv2;

CREATE SERVER md_repl_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', prefer_replica 'true');
CREATE USER MAPPING FOR CURRENT_USER SERVER md_repl_srv;

CREATE FOREIGN TABLE md_str (k text OPTIONS (key 'true'), v text)
    SERVER md_srv OPTIONS (tabletype 'string', keyprefix 'md:');
CREATE FOREIGN TABLE md_str2 (k text OPTIONS (key 'true'), v text)
    SERVER md_srv2 OPTIONS (tabletype 'string', keyprefix 'md:');
CREATE FOREIGN TABLE md_str_db3 (k text OPTIONS (key 'true'), v text)
    SERVER md_srv OPTIONS (tabletype 'string', keyprefix 'md:', database '3');
CREATE FOREIGN TABLE md_repl_t (k text OPTIONS (key 'true'), v text)
    SERVER md_repl_srv OPTIONS (tabletype 'string', keyprefix 'md:');
CREATE FOREIGN TABLE md_pfx (k text OPTIONS (key 'true'), v text)
    SERVER md_srv OPTIONS (tabletype 'string', keyprefix 'mdp:');
CREATE FOREIGN TABLE md_ro (k text OPTIONS (key 'true'), v text)
    SERVER md_srv OPTIONS (tabletype 'string', keyprefix 'md:', readonly 'true');
CREATE FOREIGN TABLE md_hash (k text OPTIONS (key 'true'),
                             a text OPTIONS (field 'alpha'),
                             b text OPTIONS (field 'beta'))
    SERVER md_srv OPTIONS (tabletype 'hash', keyprefix 'mdh:');
CREATE FOREIGN TABLE md_set (k text OPTIONS (key 'true'), m text)
    SERVER md_srv OPTIONS (tabletype 'set', keyprefix 'mds:');
CREATE FOREIGN TABLE md_zset (k text OPTIONS (key 'true'),
                             m text OPTIONS (member 'true'),
                             s text OPTIONS (score 'true'))
    SERVER md_srv OPTIONS (tabletype 'zset', keyprefix 'mdz:');
CREATE FOREIGN TABLE md_list (k text OPTIONS (key 'true'), m text)
    SERVER md_srv OPTIONS (tabletype 'list', keyprefix 'mdl:');
CREATE FOREIGN TABLE md_single (v text)
    SERVER md_srv OPTIONS (tabletype 'string', singleton_key 'md:only');

CREATE FUNCTION md_err(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    d text;
BEGIN
    EXECUTE sql;
    RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
    RETURN SQLSTATE || ': ' || SQLERRM ||
           CASE WHEN coalesce(d,'') <> '' THEN E'\n  DETAIL: ' || d ELSE '' END;
END $$;

-- Only this wrapper's own EXPLAIN lines. Core's formatting differs across
-- supported majors, and an assertion that breaks on that is asserting
-- PostgreSQL's output format rather than anything about Valkey.
CREATE FUNCTION md_explain(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
    line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP
        IF line LIKE '%Valkey %' THEN
            RETURN NEXT btrim(line);
        END IF;
    END LOOP;
END $$;

-- Fixture rows, written through the probe rather than through the FDW.
SELECT valkey_fdw_test_poke('md_srv', 'md:seed1', 'a');
SELECT valkey_fdw_test_poke('md_srv', 'md:seed2', 'b');
SELECT valkey_fdw_test_poke('md_srv', 'md:seed3', 'c');
SELECT valkey_fdw_test_poke('md_srv', 'md:only', 'the one row');
SELECT count(*) FROM valkey_fdw_test_probe('md_srv', 0, 'RPUSH', 'mdl:one', 'x');
SELECT count(*) FROM valkey_fdw_test_probe('md_srv', 0, 'SADD', 'mds:one', 'm1');
SELECT count(*) FROM valkey_fdw_test_probe('md_srv', 0, 'ZADD', 'mdz:one', '1.5', 'zm');

-- ---------------------------------------------------------------------------
-- Every Exec* callback returns a non-NULL slot.
--
-- The cheap detector for a silent mistake: returning NULL means "no row" to
-- core, so es_processed stops incrementing, AFTER triggers stop firing and
-- RETURNING goes empty. The RETURNING rows are the observable: pg_regress
-- runs psql quietly and does not record command tags, so "INSERT 0 0" would
-- pass unnoticed and only the empty result set gives this teeth.
--
-- DELETE ... RETURNING reports one row of NULLs, and that is honest rather
-- than a defect in this test: the old row is not available in the delete
-- callback (core adds its wholerow junk column only when row triggers exist)
-- and a deferred write has no remote RETURNING to fetch it from.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO md_str VALUES ('md:n1','v1') RETURNING k, v;
UPDATE md_str SET v = 'v2' WHERE k = 'md:seed1' RETURNING k, v;
DELETE FROM md_str WHERE k = 'md:seed2' RETURNING k;
SELECT live_ops FROM valkey_fdw_test_wbuf_stats();
SELECT ordinal, op, convert_from(key,'UTF8') AS key
FROM valkey_fdw_test_wbuf_dump() ORDER BY ordinal;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- The junk row-identity name carries the attnum.
--
-- The naming scheme's only non-tautological test. AddForeignUpdateTargets and
-- BeginForeignModify necessarily use the same builder, so any test comparing
-- one against the other passes whatever the scheme is. The behavioural
-- property is the collision: add_row_identity_var raises "conflicting uses of
-- row-identity name" when one name maps to two different Vars, and it takes an
-- early-return path for a single-result-relation UPDATE - so only an inherited
-- or partitioned target can produce it.
-- ---------------------------------------------------------------------------
CREATE TABLE md_parent (k text, v text, tag text) PARTITION BY LIST (tag);

-- k is attnum 1 here...
CREATE FOREIGN TABLE md_c1 (k text OPTIONS (key 'true'),
                            v text OPTIONS (field 'v'),
                            tag text OPTIONS (field 'tag'))
    SERVER md_srv OPTIONS (tabletype 'hash', keyprefix 'mdc1:');
-- ...and attnum 3 here. Attached rather than declared PARTITION OF, because
-- PARTITION OF inherits the parent's column order and the attnums would match.
CREATE FOREIGN TABLE md_c2 (v text OPTIONS (field 'v'),
                            tag text OPTIONS (field 'tag'),
                            k text OPTIONS (key 'true'))
    SERVER md_srv OPTIONS (tabletype 'hash', keyprefix 'mdc2:');

ALTER TABLE md_parent ATTACH PARTITION md_c1 FOR VALUES IN ('a');
ALTER TABLE md_parent ATTACH PARTITION md_c2 FOR VALUES IN ('b');

-- One row in each partition, so the UPDATE resolves and reads both children's
-- junk columns rather than merely planning them.
SELECT count(*) FROM valkey_fdw_test_probe('md_srv', 0,
    'HSET', 'mdc1:r1', 'v', '1', 'tag', 'a');
SELECT count(*) FROM valkey_fdw_test_probe('md_srv', 0,
    'HSET', 'mdc2:r2', 'v', '2', 'tag', 'b');

BEGIN;
UPDATE md_parent SET v = 'x' WHERE tag IN ('a','b');
SELECT live_ops FROM valkey_fdw_test_wbuf_stats();
SELECT op, convert_from(key,'UTF8') AS key, convert_from(oldkey,'UTF8') AS oldkey
FROM valkey_fdw_test_wbuf_dump() ORDER BY 2;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- UPDATE reads the old identity from planSlot's junk column, not from slot.
--
-- A key rename is the case that tells them apart: the old key comes from the
-- junk column and the new one from the rebuilt row. If the callback read the
-- new key for both, oldkey would equal key and the flush would emit a create
-- where it should emit a RENAME.
-- ---------------------------------------------------------------------------
BEGIN;
UPDATE md_str SET k = 'md:renamed', v = 'v9' WHERE k = 'md:seed3';
SELECT op,
       convert_from(key,'UTF8')    AS newkey,
       convert_from(oldkey,'UTF8') AS oldkey
FROM valkey_fdw_test_wbuf_dump();
ROLLBACK;

-- A set row's identity is (key, member), and both halves travel. A zset row
-- additionally carries its score through the write direction's grammar check.
BEGIN;
DELETE FROM md_set WHERE k = 'mds:one';
SELECT op, convert_from(key,'UTF8') AS key, convert_from(member,'UTF8') AS member
FROM valkey_fdw_test_wbuf_dump();
ROLLBACK;

BEGIN;
INSERT INTO md_zset VALUES ('mdz:one', 'zm2', '2.5');
UPDATE md_zset SET s = '3.5' WHERE k = 'mdz:one';
SELECT ordinal, op, convert_from(key,'UTF8') AS key,
       convert_from(member,'UTF8') AS member
FROM valkey_fdw_test_wbuf_dump() ORDER BY ordinal;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- The PAYLOAD travels, not just the identity.
--
-- Everything above reads key and member. value, score and oldmember are the
-- three fields nothing looked at, and all three were confirmed by mutation to
-- be stubbable to an empty string with the whole suite still green - the write
-- direction's entire point silently absent. They live only in the buffer until
-- S6 sends them, so a column apiece here is the only thing that can see them.
--
-- Rendered through convert_from so a NUL or a truncation shows up as different
-- text rather than as an equal-looking bytea prefix.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO md_str VALUES ('md:p1', 'the payload');
UPDATE md_str SET v = 'replaced' WHERE k = 'md:seed1';
SELECT ordinal, op, convert_from(key,'UTF8') AS key,
       convert_from(value,'UTF8') AS value
FROM valkey_fdw_test_wbuf_dump() ORDER BY ordinal;
ROLLBACK;

-- A zset carries a score on both INSERT and UPDATE. Until now the score was
-- observed only through its refusals (NaN, infinity), never through a value
-- that was accepted.
BEGIN;
INSERT INTO md_zset VALUES ('mdz:one', 'zm3', '4.25');
UPDATE md_zset SET s = '-7' WHERE k = 'mdz:one';
SELECT ordinal, op, convert_from(member,'UTF8') AS member,
       convert_from(score,'UTF8') AS score
FROM valkey_fdw_test_wbuf_dump() ORDER BY ordinal;
ROLLBACK;

-- Renaming a MEMBER is to a zset what renaming a key is to a string, and it is
-- the only statement that makes oldmember differ from member. Without it the
-- member half of vfdw_modify_read_old is dead: deleting the block that fills
-- oldmember changed nothing any suite could see.
BEGIN;
UPDATE md_zset SET m = 'zm9' WHERE k = 'mdz:one';
SELECT op, convert_from(member,'UTF8')    AS member,
           convert_from(oldmember,'UTF8') AS oldmember
FROM valkey_fdw_test_wbuf_dump();
ROLLBACK;

-- ---------------------------------------------------------------------------
-- Every operation carries the hash slot of its own key.
--
-- Nothing on the flush path reads op->hashslot: slot agreement is decided
-- independently by vfdw_flush_cluster_slot, which walks the buffer itself. So
-- this is an obligation no behaviour depends on yet - which is exactly why it
-- needs an assertion now. Setting it to a constant would be invisible until
-- the routing that needs it arrives, and by then the mistake is old.
--
-- Compared against the slot function rather than to literals, so the expected
-- file does not encode CRC16 values that only this project can check.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO md_str VALUES ('md:s1', 'a');
INSERT INTO md_str VALUES ('md:s2', 'b');
INSERT INTO md_hash VALUES ('mdh:s3', 'x', 'y');
SELECT bool_and(hashslot = valkey_fdw_test_keyslot(key)) AS slot_matches_key,
       count(DISTINCT hashslot) > 1                      AS slots_differ
FROM valkey_fdw_test_wbuf_dump();
ROLLBACK;

-- ---------------------------------------------------------------------------
-- A singleton table whose key column is absent altogether.
--
-- map->keyattno is InvalidAttrNumber here, so the key render must test it
-- before resolving the column; indexing unconditionally is a read of cols[-1].
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO md_single VALUES ('v');
SELECT convert_from(key,'UTF8') AS singleton_key FROM valkey_fdw_test_wbuf_dump();
UPDATE md_single SET v = 'w';
DELETE FROM md_single;
SELECT live_ops FROM valkey_fdw_test_wbuf_stats();
SELECT ordinal, op, convert_from(key,'UTF8') AS key
FROM valkey_fdw_test_wbuf_dump() ORDER BY ordinal;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- A list accepts INSERT and DELETE and refuses UPDATE, and the accepted two
-- really do buffer now that the exec callbacks exist.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO md_list VALUES ('mdl:one', 'y');
DELETE FROM md_list WHERE k = 'mdl:one';
SELECT ordinal, op, convert_from(key,'UTF8') AS key,
       convert_from(member,'UTF8') AS member
FROM valkey_fdw_test_wbuf_dump() ORDER BY ordinal;
ROLLBACK;

SELECT md_err($$UPDATE md_list SET m = 'x'$$) AS list_update;
-- The mask core reports, which is what an application sees. CmdType puts
-- UPDATE at bit 2, INSERT at bit 3 and DELETE at bit 4, so a fully writable
-- table is 28 and a list is 24.
SELECT c.relname, t.is_insertable_into,
       pg_relation_is_updatable(c.oid, false) AS updatable_mask
FROM pg_class c
JOIN information_schema.tables t
  ON t.table_name = c.relname AND t.table_schema = 'public'
WHERE c.relname IN ('md_str', 'md_list', 'md_ro')
ORDER BY 1;

-- ---------------------------------------------------------------------------
-- The refusal gates, with their exact messages.
--
-- One row each, through a WHEN OTHERS handler, so a wrong-but-plausible
-- SQLSTATE fails the comparison. The message text is recorded because several
-- of these share a SQLSTATE and only the text tells them apart.
-- ---------------------------------------------------------------------------
SELECT md_err($$INSERT INTO md_str VALUES ('md:oc','v') ON CONFLICT DO NOTHING$$)
    AS on_conflict_nothing;
SELECT md_err($$INSERT INTO md_str VALUES ('md:oc','v')
                ON CONFLICT (k) DO UPDATE SET v = 'x'$$) AS on_conflict_update;
-- MERGE arrived in PostgreSQL 15. Before it the grammar refuses the statement
-- and no wrapper is reached at all, so pinning the message would assert the
-- age of the server rather than anything about Valkey. Each major is held to
-- the refusal it can actually produce - core's own 0A000 for a foreign table
-- where MERGE exists, a syntax error where it does not - and both print the
-- same thing, which is that the write did not happen.
SELECT CASE WHEN current_setting('server_version_num')::int >= 150000
            THEN md_err($$MERGE INTO md_str t USING (SELECT 1 AS x) s
                             ON t.k = 'md:nope'
                           WHEN NOT MATCHED THEN INSERT (k,v)
                                VALUES ('md:m','v')$$)
                 LIKE '0A000: cannot execute MERGE on relation "md_str"%' ||
                      'This operation is not supported for foreign tables.'
            ELSE md_err($$MERGE INTO md_str t USING (SELECT 1 AS x) s
                             ON t.k = 'md:nope'
                           WHEN NOT MATCHED THEN INSERT (k,v)
                                VALUES ('md:m','v')$$)
                 LIKE '42601: syntax error at or near "MERGE"%'
       END AS merge_refused;
SELECT md_err($$TRUNCATE md_str$$) AS truncate_stmt;
SELECT md_err($$INSERT INTO md_str VALUES (NULL,'v')$$) AS null_key;
SELECT md_err($$INSERT INTO md_pfx VALUES ('other:1','v')$$) AS out_of_prefix;
SELECT md_err($$INSERT INTO md_zset VALUES ('mdz:z','m','NaN')$$) AS nan_score;
SELECT md_err($$INSERT INTO md_hash VALUES ('mdh:h',NULL,NULL)$$) AS all_fields_null;
SELECT md_err($$INSERT INTO md_ro VALUES ('md:ro','v')$$) AS readonly_insert;
SELECT md_err($$INSERT INTO md_repl_t VALUES ('md:r','v')$$) AS prefer_replica_write;
SELECT count(*) >= 0 AS prefer_replica_read_ok FROM md_repl_t;

-- Two servers, and two databases, in one transaction.
BEGIN;
INSERT INTO md_str VALUES ('md:s1','v');
SELECT md_err($$INSERT INTO md_str2 VALUES ('md:s2','v')$$) AS second_server;
SELECT md_err($$INSERT INTO md_str_db3 VALUES ('md:s3','v')$$) AS second_database;
SELECT live_ops FROM valkey_fdw_test_wbuf_stats();
ROLLBACK;

-- Two user mappings in one transaction. The second write runs as another role
-- through a SECURITY DEFINER function, which is the only way to change
-- GetUserId() at the point BeginForeignModify runs.
CREATE ROLE md_other;
GRANT USAGE ON FOREIGN SERVER md_srv TO md_other;
CREATE USER MAPPING FOR md_other SERVER md_srv OPTIONS (password_required 'false');
GRANT SELECT, INSERT ON md_str TO md_other;
CREATE FUNCTION md_as_other(kk text) RETURNS void
LANGUAGE plpgsql SECURITY DEFINER AS $$
BEGIN
    INSERT INTO md_str VALUES (kk, 'v');
END $$;
ALTER FUNCTION md_as_other(text) OWNER TO md_other;

BEGIN;
INSERT INTO md_str VALUES ('md:u1','v');
-- The role the regression run happens to use is not stable across machines,
-- so it is masked out; the DETAIL still shows that the two roles differ.
SELECT replace(md_err($$SELECT md_as_other('md:u2')$$), current_user, '<runner>')
    AS second_user_mapping;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- readonly added under a cached generic plan.
--
-- A guard against a future change that moves the check out of
-- PlanForeignModify. No mutation of this step's code distinguishes it from the
-- plain readonly test, because the relcache invalidation forces a replan and
-- the plan-time check then fires; it is not counted as covering anything.
-- ---------------------------------------------------------------------------
PREPARE md_ro_p(text) AS INSERT INTO md_str VALUES ($1, 'v');

BEGIN;
EXECUTE md_ro_p('md:p1');
EXECUTE md_ro_p('md:p2');
EXECUTE md_ro_p('md:p3');
EXECUTE md_ro_p('md:p4');
EXECUTE md_ro_p('md:p5');
EXECUTE md_ro_p('md:p6');
SELECT live_ops FROM valkey_fdw_test_wbuf_stats();
ROLLBACK;

ALTER FOREIGN TABLE md_str OPTIONS (ADD readonly 'true');
EXECUTE md_ro_p('md:p7');
ALTER FOREIGN TABLE md_str OPTIONS (DROP readonly);
DEALLOCATE md_ro_p;

-- ---------------------------------------------------------------------------
-- COPY FROM.
--
-- BeginForeignInsert is not registered yet (implementation step S8), so COPY
-- and tuple routing reach the insert callback with a NULL FDW state. Refused
-- there rather than dereferenced. The planSlot == NULL path this would
-- otherwise exercise therefore stays untested until S8.
-- ---------------------------------------------------------------------------
COPY md_str (k, v) FROM stdin;
md:copy1	a
\.

-- readonly on the COPY path is enforced by core, from IsForeignRelUpdatable,
-- before the callback is reached at all.
COPY md_ro (k, v) FROM stdin;
md:copyro	a
\.

-- Tuple routing into a foreign partition takes the same path.
SELECT md_err($$INSERT INTO md_parent VALUES ('mdc1:r','x','a')$$) AS routed_insert;

-- ---------------------------------------------------------------------------
-- EXPLAIN.
--
-- Prints from fdw_private only; there is no execution state under an EXPLAIN
-- without ANALYZE. The command shape with its literals redacted is
-- deliberately absent: it needs the ledger's fold and the script encoder,
-- which land with S4 and S5.
-- ---------------------------------------------------------------------------
SELECT * FROM md_explain($$INSERT INTO md_str VALUES ('md:e1','v')$$);
SELECT * FROM md_explain($$UPDATE md_str SET v = 'x' WHERE k = 'md:seed1'$$);
SELECT * FROM md_explain($$DELETE FROM md_str WHERE k = 'md:seed1'$$);
SELECT * FROM md_explain($$DELETE FROM md_zset WHERE k = 'mdz:z'$$);
SELECT * FROM md_explain($$UPDATE md_single SET v = 'x'$$);

-- EXPLAIN must not bind the transaction's unit. Without the
-- EXEC_FLAG_EXPLAIN_ONLY early return in BeginForeignModify, the EXPLAIN below
-- would bind database 0 against a transaction already bound to database 3 and
-- raise 0A000.
BEGIN;
INSERT INTO md_str_db3 VALUES ('md:x1','v');
SELECT * FROM md_explain($$INSERT INTO md_str VALUES ('md:x2','v')$$);
SELECT live_ops, database FROM valkey_fdw_test_wbuf_stats();
ROLLBACK;

-- ---------------------------------------------------------------------------
DROP FUNCTION md_as_other(text);
DROP USER MAPPING FOR md_other SERVER md_srv;
REVOKE ALL ON md_str FROM md_other;
REVOKE ALL ON FOREIGN SERVER md_srv FROM md_other;
DROP ROLE md_other;
