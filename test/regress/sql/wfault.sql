-- The write path under injected faults.
--
-- Section 8 names exactly four outcomes and gives each its own SQLSTATE and
-- its own sentence. Three of them are unreachable without breaking the
-- transport on purpose, which is what this suite is for.
--
-- The property that matters most is NEGATIVE and is asserted after every case:
-- no fault ever produces a false "applied", and none produces the partial
-- application of outcome 4. A wrapper that reported success on a broken
-- connection would pass every other suite in this tree.

-- On this topology 'valkey' resolves to the PROXY and 'valkey-upstream' to
-- the real server, so a table on wf_srv is reached through the thing that is
-- about to misbehave.
CREATE SERVER wf_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379',
             connect_timeout_ms '2000', command_timeout_ms '3000');
CREATE USER MAPPING FOR CURRENT_USER SERVER wf_srv;

CREATE FOREIGN TABLE wf (k text OPTIONS (key 'true'), v text)
    SERVER wf_srv OPTIONS (tabletype 'string', keyprefix 'wf:');

-- A second server reaching the SAME Valkey directly, so the keyspace can be
-- inspected after a fault without going through the proxy that is currently
-- armed to break things.
CREATE SERVER wf_direct FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-upstream', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER wf_direct;

CREATE TEMP TABLE wf_ctl(reply text);

-- The proxy's control port, reached exactly as fault.sql reaches it. Rules
-- are one-shot: the proxy clears each as it fires.
CREATE FUNCTION fault_arm(rule text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
    DELETE FROM wf_ctl;
    EXECUTE format(
        'COPY wf_ctl FROM PROGRAM %L',
        'printf ' || quote_literal(rule || E'\n') || ' | nc -q1 valkey 6390');
    SELECT reply INTO r FROM wf_ctl LIMIT 1;
    RETURN r;
END $$;

-- Each COMMIT below is written bare rather than wrapped in a helper: plpgsql
-- cannot COMMIT inside a function called from a query, so a wrapper would
-- report its own refusal instead of the flush's. The recorded output then
-- carries the real message, which is what a user actually sees.

-- Whether the key exists, read directly rather than through the proxy.
CREATE FUNCTION wf_exists(key text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
    SELECT num INTO n FROM valkey_fdw_test_probe('wf_direct', 0, 'EXISTS', key::bytea);
    RETURN n = 1;
END $$;

SELECT valkey_fdw_test_flush('wf_direct') AS cleared;

-- Each faulted COMMIT below is followed by \echo :SQLSTATE, because an
-- application switches on the code and 08007 is the whole point of outcome 3
-- - it is the "go and look" signal. Before the flush re-classified transport
-- failures these arrived as 08006 and XX000, which no caller would recognise,
-- and the prose alone would not have shown the difference.
--
-- :SQLSTATE rather than \set VERBOSITY verbose: verbose also prints
-- LOCATION with a file and line number, which moves on every edit to
-- vfdw_flush.c and would make this file a change detector.

-- ---------------------------------------------------------------------------
-- The control channel works, and an unarmed proxy applies writes normally.
--
-- Without this the whole suite could pass by never reaching Valkey at all.
-- ---------------------------------------------------------------------------
SELECT fault_arm('clear') AS cleared_rules;

BEGIN;
INSERT INTO wf VALUES ('wf:ok', 'applied');
COMMIT;
SELECT wf_exists('wf:ok') AS unarmed_write_applied;

-- ---------------------------------------------------------------------------
-- The connection dies after the program was sent: OUTCOME 3, indeterminate.
--
-- 08007 transaction_resolution_unknown, and the message must NOT claim the
-- write did not happen - the server may well have run it before the socket
-- closed. This is the case where an honest wrapper has to say it does not
-- know, and the temptation to report something more definite is exactly what
-- the SQLSTATE exists to resist.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm drop_after EVALSHA') AS armed;

BEGIN;
INSERT INTO wf VALUES ('wf:drop', 'x');
COMMIT;
\echo drop sqlstate: :SQLSTATE

SELECT fault_arm('clear') AS cleared_rules;

-- The session survives, and the pool did not keep a connection whose reply
-- offset is unknown: the next write works.
BEGIN;
INSERT INTO wf VALUES ('wf:after_drop', 'y');
COMMIT;
SELECT wf_exists('wf:after_drop') AS usable_after_drop;

-- ---------------------------------------------------------------------------
-- A malformed frame in reply to the program: outcome 3 again.
--
-- Different cause, same honest answer. Reported as indeterminate rather than
-- as a protocol error, because by the time the frame arrives the program has
-- already run.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm garbage EVALSHA') AS armed;

BEGIN;
INSERT INTO wf VALUES ('wf:garbage', 'x');
COMMIT;
\echo garbage sqlstate: :SQLSTATE

SELECT fault_arm('clear') AS cleared_rules;

BEGIN;
INSERT INTO wf VALUES ('wf:after_garbage', 'y');
COMMIT;
SELECT wf_exists('wf:after_garbage') AS usable_after_garbage;

-- ---------------------------------------------------------------------------
-- A well-formed reply of the wrong shape.
--
-- vfdw_reply_expect catches it rather than dereferencing it. This is the case
-- that a mask built from raw type constants got wrong: it rejected the
-- program's own SUCCESS reply after the write had landed, which is the one
-- outcome this design exists to prevent.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm wrong_type EVALSHA') AS armed;

BEGIN;
INSERT INTO wf VALUES ('wf:shape', 'x');
COMMIT;
\echo shape sqlstate: :SQLSTATE

SELECT fault_arm('clear') AS cleared_rules;

-- ---------------------------------------------------------------------------
-- The command deadline expires while the program is running.
--
-- statement_timeout cannot reach a pre-commit flush - core disables it before
-- CommitTransactionCommand - so command_timeout_ms is the only bound that
-- applies, and this is what proves it applies there at all.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm delay 8000 EVALSHA') AS armed;

BEGIN;
INSERT INTO wf VALUES ('wf:slow', 'x');
COMMIT;
\echo slow sqlstate: :SQLSTATE

SELECT fault_arm('clear') AS cleared_rules;

BEGIN;
INSERT INTO wf VALUES ('wf:after_delay', 'y');
COMMIT;
SELECT wf_exists('wf:after_delay') AS usable_after_deadline;

-- ---------------------------------------------------------------------------
-- A fault on SCRIPT LOAD is reported as itself, not as the NOSCRIPT it causes.
--
-- The two commands are pipelined, so a failed load necessarily makes the
-- EVALSHA behind it answer NOSCRIPT. Reporting that instead would tell the
-- user the script is missing when the real problem is that loading it was
-- refused - and it would be reported as indeterminate when in fact nothing
-- ran at all.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm wrong_type SCRIPT') AS armed;

BEGIN;
INSERT INTO wf VALUES ('wf:load', 'x');
COMMIT;
\echo load sqlstate: :SQLSTATE

SELECT fault_arm('clear') AS cleared_rules;

-- ---------------------------------------------------------------------------
-- THE DURABILITY BOUNDARY, DEMONSTRATED.
--
-- Three of the five faulted transactions left their key IN VALKEY while
-- PostgreSQL rolled back. That is not a defect and this suite must not assert
-- it away: the fault was injected on the REPLY path, so the server had
-- already run the program. README's boundary says exactly this - "committed
-- in PostgreSQL implies accepted by Valkey" is one-directional, and here is
-- the converse failing.
--
-- The first version of this block asserted every key was absent. That was an
-- expectation contradicting the design, and recording it would have pinned a
-- wrong answer as expected output for good.
--
-- What IS asserted: the outcome was reported as unknown rather than as
-- success, and the transaction aborted in PostgreSQL either way.
-- ---------------------------------------------------------------------------
SELECT k, wf_exists(k) AS in_valkey_despite_rollback
FROM unnest(ARRAY['wf:drop','wf:garbage','wf:shape','wf:slow','wf:load']) AS k
ORDER BY k;

-- And they ARE visible to a later SELECT, because a later SELECT reads
-- Valkey and Valkey has them. Stated rather than asserted away: this is the
-- user-visible face of the boundary, and it is the number a reader of this
-- suite most needs to see. The second draft of this block claimed the
-- opposite - that none would be visible - which was wrong twice over, since
-- it contradicted both the design and the row above it.
SELECT count(*) AS rows_visible_after_rollback
FROM wf WHERE k IN ('wf:drop','wf:garbage','wf:shape','wf:slow','wf:load');

-- The writes that were meant to succeed did.
SELECT k, wf_exists(k) AS present
FROM unnest(ARRAY['wf:ok','wf:after_drop','wf:after_garbage','wf:after_delay']) AS k
ORDER BY k;

-- OUTCOME 4 never happened. A partially applied unit is defined by what the
-- server TOLD us - VFDW1 INTERNAL - not by what survived, so its absence is
-- what this asserts. pg_regress records every error above verbatim, and none
-- of them carries that message; a run that produced one would diff here.
SELECT NOT EXISTS (
    SELECT 1 FROM wf WHERE v = 'VFDW1 INTERNAL'
) AS no_partial_application_reported;

SELECT fault_arm('clear') AS final_clear;
SELECT valkey_fdw_test_flush('wf_direct') AS cleaned_up;

DROP FOREIGN TABLE wf;
DROP FUNCTION wf_exists(text), fault_arm(text);
DROP USER MAPPING FOR CURRENT_USER SERVER wf_srv;
DROP USER MAPPING FOR CURRENT_USER SERVER wf_direct;
DROP SERVER wf_srv;
DROP SERVER wf_direct;
