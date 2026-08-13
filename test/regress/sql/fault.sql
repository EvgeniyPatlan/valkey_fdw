-- Error paths, driven by the fault-injection proxy.
--
-- These paths are unreachable from a well-behaved server, which is exactly
-- why such code rots: nothing ever executes it. The proxy sits between
-- PostgreSQL and Valkey and misbehaves on request.
--
-- Runs only on the `fault` topology, where 'valkey' resolves to the proxy.

CREATE TEMP TABLE fault_ctl(reply text);

-- Arm a one-shot rule. The proxy clears it as soon as it fires, so each
-- assertion below starts from a clean server.
CREATE FUNCTION fault_arm(rule text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
    DELETE FROM fault_ctl;
    EXECUTE format(
        'COPY fault_ctl FROM PROGRAM %L',
        'printf ' || quote_literal(rule || E'\n') || ' | nc -q1 valkey 6390');
    SELECT reply INTO r FROM fault_ctl LIMIT 1;
    RETURN r;
END $$;

-- How many rules have actually fired.
--
-- The proxy answers "+OK" to every arm, whether or not the rule ever matches
-- a command. A rule that silently never fires leaves the statement below it
-- succeeding, and `harness.sh record` then bakes that success in as expected
-- output - a test that asserts nothing, forever. Counting is what makes the
-- difference between "the fault was injected and handled" and "the fault was
-- never injected" visible, and this suite has shipped both.
CREATE FUNCTION fault_fired() RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
    r := fault_arm('stats');
    RETURN (regexp_match(r, 'faults_fired=([0-9]+)'))[1]::bigint;
END $$;

-- The proxy counts across its whole lifetime, and the container outlives any
-- one run, so only the change across this suite means anything. An absolute
-- count would be right on a fresh container and wrong on the second run.
CREATE TEMP TABLE fault_base AS SELECT fault_fired() AS n;

-- The proxy is reachable and passes normal traffic through untouched.
SELECT fault_arm('ping') AS control_channel;
SELECT valkey_fdw_test_ping('valkey', 6379, 5000, 5000) AS ping_through_proxy;

-- ---------------------------------------------------------------------------
-- The connection dies before the command reaches the server.
--
-- Must surface as a classified connection failure. Building that message
-- must not read the context after it has been released, which is the
-- mistake a connection-error path makes most readily.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm drop_before GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x41'::bytea);

-- The rule is one-shot, so the next attempt must succeed. This is what
-- proves the failure above was injected rather than a broken connection
-- layer.
SELECT valkey_fdw_test_binary('valkey', 6379, '\x41'::bytea) = '\x41'::bytea
    AS recovered_after_drop;

-- ---------------------------------------------------------------------------
-- The connection dies after the command is sent but before the reply.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm drop_after GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x42'::bytea);

-- ---------------------------------------------------------------------------
-- A malformed RESP frame must be a protocol error, not a parser crash.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm garbage GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x43'::bytea);

-- ---------------------------------------------------------------------------
-- A well-formed reply of the wrong shape must be caught by the caller's
-- expectations rather than dereferenced blindly.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm wrong_type GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x44'::bytea);

-- ---------------------------------------------------------------------------
-- A frame whose declared length exceeds what follows: the reader must wait,
-- then hit our deadline, rather than reading past the buffer.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm truncate GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x45'::bytea);

-- ---------------------------------------------------------------------------
-- The connection dies partway through a reply.
--
-- Distinct from drop_after: a well-formed frame has begun arriving and then
-- stops. The half-read bytes must not become a short value - a reply is only
-- a reply once it is complete, and returning what arrived would silently
-- corrupt the row rather than fail.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm drop_mid GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x46'::bytea);

SELECT valkey_fdw_test_binary('valkey', 6379, '\x46'::bytea) = '\x46'::bytea
    AS recovered_after_drop_mid;

-- ---------------------------------------------------------------------------
-- A reply that declares a multi-gigabyte length.
--
-- libvalkey offers no way to refuse a bulk length, so nothing rejects the
-- claim itself. What matters is that nothing acts on it either: the declared
-- size costs no memory until the bytes arrive, and they never do, so this
-- ends at the command deadline rather than in an allocation the size of the
-- claim. A reader that sized a buffer from the header would not get here.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm huge GET') AS armed;
SELECT valkey_fdw_test_binary('valkey', 6379, '\x47'::bytea);

SELECT valkey_fdw_test_binary('valkey', 6379, '\x47'::bytea) = '\x47'::bytea
    AS recovered_after_huge;

-- ---------------------------------------------------------------------------
-- A slow reply must be cut off by the command deadline.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm delay 3000 PING') AS armed;
SELECT valkey_fdw_test_ping('valkey', 6379, 5000, 500);

-- ---------------------------------------------------------------------------
-- A SCAN reply whose TOP level is right and whose ELEMENTS are not.
--
-- vfdw_reply_expect validates the outermost type and nothing inside it, so an
-- integer where the cursor belongs, or an integer among the keys, used to be
-- read as (str, len) = (NULL, 0). pnstrdup(NULL, 0) and memcpy(dst, NULL, 0)
-- are undefined by the standard however benign glibc makes them, and the empty
-- cursor or empty key that resulted was then sent back to the server as the
-- next page request.
--
-- The proxy is what makes this reachable: wrong_type replaces the whole frame
-- and so can only exercise a top-level check. No well-behaved server sends
-- these, which is exactly the point - "libvalkey would not hand me that" is an
-- assumption about the far end and not about the library, which parses
-- faithfully whatever arrives.
-- ---------------------------------------------------------------------------
CREATE SERVER fault_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER fault_srv;
CREATE FOREIGN TABLE fault_t (k text, v text) SERVER fault_srv
    OPTIONS (keyprefix 'vfdw:fault:');

SELECT fault_arm('arm bad_cursor SCAN') AS armed;
SELECT count(*) FROM fault_t;

SELECT fault_arm('arm bad_key SCAN') AS armed;
SELECT count(*) FROM fault_t;

-- One-shot, so the scan works again: what was refused was the frame, not the
-- table.
SELECT count(*) AS rows_after_bad_elements FROM fault_t;

-- ---------------------------------------------------------------------------
-- A VALUE FETCH THE SERVER REFUSES.
--
-- SCAN and the value fetch are separate commands carrying separate
-- permissions, so a user granted the first and denied the second over part of
-- the keyspace is an ordinary ACL rather than a broken server. The refusal
-- then arrives per key, in the middle of a page, wearing the same reply type
-- as the WRONGTYPE that genuinely means "not a row of this table" - and a
-- scan that treats the whole error class as skippable answers with a result
-- set one row short and no error at all. That is the same silent truncation
-- reached through the value fetch instead of through the cursor, and it is
-- the failure this wrapper defines itself against.
--
-- Keys are seeded first so that the short answer would be a plausible one:
-- over an empty keyspace the scan has nothing to drop, and returning nothing
-- would prove nothing.
-- ---------------------------------------------------------------------------
SELECT count(*) AS seeded
  FROM valkey_fdw_test_probe('fault_srv', 0, 'MSET',
                             'vfdw:fault:1', 'one', 'vfdw:fault:2', 'two',
                             'vfdw:fault:3', 'three', 'vfdw:fault:4', 'four');

SELECT count(*) AS rows_before_the_denial FROM fault_t;

-- The SQLSTATE is what an application switches on, and 42501 is the whole
-- point of it: "you may not read this" rather than "there was nothing there".
SELECT fault_arm('arm noperm GET') AS armed;
SELECT count(*) FROM fault_t;
\echo noperm sqlstate: :SQLSTATE

-- One-shot, so the table reads normally again and every row is still there:
-- what was refused was the fetch, not the data.
SELECT count(*) AS rows_after_the_denial FROM fault_t;

SELECT num AS cleaned
  FROM valkey_fdw_test_probe('fault_srv', 0, 'DEL',
                             'vfdw:fault:1', 'vfdw:fault:2',
                             'vfdw:fault:3', 'vfdw:fault:4');

DROP FOREIGN TABLE fault_t;
DROP SERVER fault_srv CASCADE;

-- ---------------------------------------------------------------------------
-- An error reply whose text is not valid in the server encoding.
--
-- Valkey echoes the client's own arguments back inside some error messages
-- ("unknown command 'X', with args beginning with..."), and this wrapper
-- deliberately sends binary-safe keys, so a server error can carry bytes that
-- are not valid UTF-8. An ERROR is one of the few places a value reaches both
-- the client and the server log without passing through the read path's
-- pg_verifymbstr, so the bytes are checked before they become errdetail and
-- replaced by a count when they fail. Delete that check and this statement
-- emits raw 0xff 0xfe into the regression output.
--
-- The message says which encoding rejected it, so the substitution reads as a
-- decision rather than as data having gone missing.
-- ---------------------------------------------------------------------------
SELECT fault_arm('arm bad_utf8 PING') AS armed;
SELECT valkey_fdw_test_ping('valkey', 6379, 5000, 5000);

-- After every injected fault the layer is still usable.
SELECT valkey_fdw_test_ping('valkey', 6379, 5000, 5000) AS still_healthy;

-- ---------------------------------------------------------------------------
-- An interrupted SELECT must not leave its reply for the next statement.
--
-- vfdw_conn_command marks the connection in-conversation around every setup
-- command, so a SELECT cut short by a cancel or a timeout leaves the flag set
-- and the transaction callback discards the connection. Before that, SELECT
-- was sent after vfdw_get_connection had already cleared the flag: the pool
-- believed the connection was quiescent, kept it with an unread +OK pending,
-- and handed that +OK to the next statement as its first reply.
--
-- The fix has been in the tree since it was written and was correct by
-- construction only - nothing had ever interrupted a SELECT. This does.
--
-- SELECT is only sent when the logical database actually changes, so the probe
-- call on database 0 is what makes the scan below issue one. Without it the
-- pooled connection is already on database 3 and the armed rule never fires -
-- which the rules_fired count at the end of this file would catch.
-- ---------------------------------------------------------------------------
CREATE SERVER dbsel_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', command_timeout_ms '20000');
CREATE USER MAPPING FOR CURRENT_USER SERVER dbsel_srv;

SELECT count(*) FROM valkey_fdw_test_probe('dbsel_srv', 3, 'SET',
                                           'dbsel:k', 'on database three');

CREATE FOREIGN TABLE dbsel_t (
    k text OPTIONS (key 'true'),
    v text
) SERVER dbsel_srv OPTIONS (tabletype 'string', singleton_key 'dbsel:k',
                            database '3');

SELECT v AS baseline FROM dbsel_t;

-- Put the pooled connection back on database 0, so the next scan must issue
-- SELECT 3 and the armed rule has something to match.
SELECT reply_type FROM valkey_fdw_test_probe('dbsel_srv', 0, 'PING');

SELECT fault_arm('arm delay 3000 SELECT') AS armed;
SET statement_timeout = '500ms';
SELECT v FROM dbsel_t;
RESET statement_timeout;

-- The statement that matters. A connection kept with an unread +OK would hand
-- it over here as this scan's first reply.
SELECT v AS after_an_interrupted_select FROM dbsel_t;

SELECT num AS cleaned
  FROM valkey_fdw_test_probe('dbsel_srv', 3, 'DEL', 'dbsel:k');
DROP FOREIGN TABLE dbsel_t;
DROP SERVER dbsel_srv CASCADE;


-- Every rule armed above matched a command and was consumed. Thirteen were
-- armed to inject a fault; if any had failed to match, its statement would
-- have quietly succeeded and this count would be short.
SELECT fault_fired() - (SELECT n FROM fault_base) AS rules_fired;

-- And the counter tracks firing, not arming. A rule on a verb this suite
-- never sends must leave it untouched - otherwise the assertion above would
-- pass whether or not anything was actually injected.
SELECT fault_arm('arm drop_before ZADD') AS armed_but_unmatched;
SELECT valkey_fdw_test_ping('valkey', 6379, 5000, 5000) AS ping_unaffected;
SELECT fault_fired() - (SELECT n FROM fault_base) AS unchanged_by_an_unmatched_rule;
SELECT fault_arm('clear') AS cleared;

DROP FUNCTION fault_fired();
DROP FUNCTION fault_arm(text);
