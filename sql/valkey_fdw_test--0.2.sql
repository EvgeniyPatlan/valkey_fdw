/* valkey_fdw_test--0.1.sql */

\echo Use "CREATE EXTENSION valkey_fdw_test" to load this file. \quit

/*
 * The diagnostics the suites drive, in an extension of their own.
 *
 * They are here rather than in valkey_fdw--0.1.sql because PostgreSQL grants
 * EXECUTE on a new function to PUBLIC, and every function below reaches a
 * Valkey server. Most reach it through the pool, so with the wrapper's own
 * credentials and against whatever the server object points at; three of them
 * - valkey_fdw_test_ping, _binary and _block - take a host and a port from
 * their caller, so they reach an ARBITRARY host on an arbitrary port, from the
 * database server's network position, for any role that can log in. Carrying
 * that in the wrapper's own extension would put it on every production
 * install, whether or not a test was ever run against it.
 *
 * Compiling them out behind a build flag was the other way to get there and
 * was rejected. It would make the binary a user installs a different binary
 * from the one the suites ran against, and every claim this project makes
 * rests on those being one object. So the code stays in the library and only
 * the catalog entry moves: valkey_fdw_test.control names $libdir/valkey_fdw,
 * the same shared object valkey_fdw itself loads, and the tested library is
 * the shipped library.
 *
 * That control file marks this extension superuser-only. Each CREATE FUNCTION
 * below is still followed by a REVOKE, because the control file governs who
 * may install the extension and the grants govern who may call what it
 * installed - a superuser installing the diagnostics on a shared database must
 * not thereby hand PUBLIC a dialler that takes a host and a port.
 */

/*
 * Diagnostic entry points into the connection and I/O layers.
 *
 * These are how the fault-injection, timeout and cancellation suites reach
 * the code below the planner. They are not part of the FDW's supported
 * surface and may change or disappear between releases.
 */
CREATE FUNCTION valkey_fdw_test_ping(
    host text, port int, connect_timeout_ms int, command_timeout_ms int)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_ping(text, int, int, int) FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_pipeline(
    server_name text, depth int)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_pipeline(text, int) FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_binary(
    host text, port int, payload bytea)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_binary(text, int, bytea) FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_block(
    host text, port int, block_seconds int, command_timeout_ms int)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_block(text, int, int, int) FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_pooled_ping(server_name text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_pooled_ping(text) FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_pool_stats(
    OUT open int, OUT opened_total bigint, OUT leased int)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_pool_stats() FROM PUBLIC;

/*
 * Write or delete one key through the pooled connection, so a suite can build
 * its own fixture on a topology where no other fixture helper exists. A NULL
 * value deletes.
 */
CREATE FUNCTION valkey_fdw_test_probe_child_guard()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_probe_child_guard() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_probe_child_guard()
IS 'Exercise the aggregate arity guard, which no wire frame can reach';

CREATE FUNCTION valkey_fdw_test_resp3(server_name text)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_resp3(text) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_resp3(text)
IS 'Whether the pooled connection to this server negotiated RESP3';

CREATE FUNCTION valkey_fdw_test_poke(
    server_name text, key bytea, value bytea DEFAULT NULL)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_poke(text, bytea, bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_poke(text, bytea, bytea)
IS 'SET one key through the pool, or DEL it when the value is NULL';

CREATE FUNCTION valkey_fdw_test_batch_flushes(server_name text, depth int)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_batch_flushes(text, int) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_batch_flushes(text, int)
IS 'How many times a batch of depth commands pushed its output buffer';

/*
 * Keyspace probes.
 *
 * These are how the write suites assert what actually landed in Valkey. They
 * reach the server through the pool - so they inherit AUTH, TLS, the logical
 * database and the configured timeouts, rather than needing a plaintext port
 * and an anonymous login - but they touch none of the mapping, scan, row,
 * value or overlay code. That is the point: a write asserted through the
 * FDW's own read path proves only that the two halves agree with each other.
 *
 * One limit worth knowing before writing an assertion: on a cluster,
 * valkey_fdw_test_keys reports the keys in the slots served by the node it is
 * connected to, not the whole keyspace.
 *
 * Everything is bytea. A text argument could not construct a key with an
 * embedded NUL, and a text result could not carry one back.
 */

CREATE FUNCTION valkey_fdw_test_probe(
    server_name text,
    database    int,
    VARIADIC    args bytea[],
    OUT reply_type text,
    OUT ordinal    int,
    OUT key_part   bytea,
    OUT val_part   bytea,
    OUT num        bigint,
    OUT dbl        float8)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_probe(text, int, bytea[]) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_probe(text, int, bytea[])
IS 'Run one Valkey command through the pool and return its reply as typed rows, INCLUDING an error reply, which is returned as data rather than raised - unlike valkey_fdw_test_keys';

CREATE FUNCTION valkey_fdw_test_probe_pairs(
    server_name text,
    database    int,
    VARIADIC    args bytea[],
    OUT reply_type text,
    OUT ordinal    int,
    OUT key_part   bytea,
    OUT val_part   bytea,
    OUT num        bigint,
    OUT dbl        float8)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_probe_pairs(text, int, bytea[]) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_probe_pairs(text, int, bytea[])
IS 'As valkey_fdw_test_probe, but pairs an even-length aggregate reply so RESP2 and RESP3 render alike';

CREATE FUNCTION valkey_fdw_test_keys(
    server_name text,
    database    int,
    prefix      bytea,
    OUT key     bytea,
    OUT keytype text)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_keys(text, int, bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_keys(text, int, bytea)
IS 'Every key under a prefix, with its Valkey type, driven to cursor 0. RAISES on an error reply - unlike valkey_fdw_test_probe, because a key dump has no row shape for one and NULLs would read as an empty keyspace';

/*
 * Value-handling diagnostics.
 *
 * Every one of these takes and returns bytea rather than text: the properties
 * they exist to assert are byte-exact, and an embedded NUL, an invalidly
 * encoded byte and the difference between an empty value and a NULL one all
 * vanish if a probe hands its result through a text type.
 */

CREATE FUNCTION valkey_fdw_test_val_out(anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_val_out(anyelement) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_val_out(anyelement)
IS 'Bytes this Datum would put on the wire; NULL only for a NULL input';

CREATE FUNCTION valkey_fdw_test_val_roundtrip(payload bytea, typ regtype)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_val_roundtrip(bytea, regtype) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_val_roundtrip(bytea, regtype)
IS 'Bytes in through the read direction and back out through the write one';

CREATE FUNCTION valkey_fdw_test_build_key(
    key bytea,
    keyprefix text DEFAULT NULL,
    keyset text DEFAULT NULL,
    singleton_key text DEFAULT NULL,
    has_key_column boolean DEFAULT true)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_build_key(bytea, text, text, text, boolean) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_build_key(bytea, text, text, text, boolean)
IS 'The Valkey key one row would write, or the refusal that stops it';

CREATE FUNCTION valkey_fdw_test_key_why(
    key bytea,
    keyprefix text DEFAULT NULL,
    keyset text DEFAULT NULL,
    singleton_key text DEFAULT NULL,
    has_key_column boolean DEFAULT true)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_key_why(bytea, text, text, text, boolean) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_key_why(bytea, text, text, text, boolean)
IS 'Which key rule fired, without raising: three of them share SQLSTATE 23514';

/*
 * One column through vfdw_val_from_slot, which is the entry point a statement
 * calls. Everything else here reaches a layer below it, so without this the
 * kind dispatch, the per-role NULL rules and the score routing have no caller.
 */
CREATE FUNCTION valkey_fdw_test_val_slot(
    payload bytea,
    typ regtype,
    kind text,
    keyprefix text DEFAULT NULL,
    keyset text DEFAULT NULL,
    singleton_key text DEFAULT NULL)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_val_slot(bytea, regtype, text, text, text, text) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_val_slot(bytea, regtype, text, text, text, text)
IS 'Bytes one slot value would put on the wire, through the production dispatch';

CREATE FUNCTION valkey_fdw_test_val_retain(a bytea, b bytea)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_val_retain(bytea, bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_val_retain(bytea, bytea)
IS 'A''s retained bytes after the per-row scratch was reset and B rendered';

CREATE FUNCTION valkey_fdw_test_all_fields_null()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_all_fields_null() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_all_fields_null()
IS 'The refusal a hash row with no mapped field set would meet';

CREATE FUNCTION valkey_fdw_test_score(score bytea, do_raise boolean DEFAULT false)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C CALLED ON NULL INPUT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_score(bytea, boolean) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_score(bytea, boolean)
IS 'Verdict on a rendered zset score, or the error a zset write would raise';

CREATE FUNCTION valkey_fdw_test_crc16(bytea)
RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
REVOKE ALL ON FUNCTION valkey_fdw_test_crc16(bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_crc16(bytea)
IS 'CRC-16/XMODEM of these bytes, before the 14-bit slot mask';

CREATE FUNCTION valkey_fdw_test_cluster_map(
    server_name text,
    OUT slot_start int,
    OUT slot_end   int,
    OUT host       text,
    OUT port       int)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_cluster_map(text) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_cluster_map(text)
IS 'The slot map this backend holds, one row per contiguous range';

CREATE FUNCTION valkey_fdw_test_cluster_route(
    server_name text,
    key         bytea,
    OUT slot    int,
    OUT host    text,
    OUT port    int)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_cluster_route(text, bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_cluster_route(text, bytea)
IS 'Where this wrapper would send a command for this key; host NULL when unclaimed';

CREATE FUNCTION valkey_fdw_test_cluster_nodes(
    server_name    text,
    OUT host       text,
    OUT port       int,
    OUT reachable  boolean,
    OUT myself     text)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_cluster_nodes(text) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_cluster_nodes(text)
IS 'Reach every primary through a pooled per-node connection and ask its CLUSTER MYID';

CREATE FUNCTION valkey_fdw_test_keyslot(bytea)
RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
REVOKE ALL ON FUNCTION valkey_fdw_test_keyslot(bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_keyslot(bytea)
IS 'Cluster hash slot of this key, to compare against CLUSTER KEYSLOT';

CREATE FUNCTION valkey_fdw_test_hashtag(bytea)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
REVOKE ALL ON FUNCTION valkey_fdw_test_hashtag(bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_hashtag(bytea)
IS 'The substring of this key that its slot is computed over';

/*
 * Vector conversion, both directions.
 *
 * bytea on the Valkey side of both signatures, because a vector containing
 * 0.0f contains four NUL bytes and a text-typed probe could neither accept
 * nor return one.
 */
CREATE FUNCTION valkey_fdw_test_vec_to_text(bytea)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
REVOKE ALL ON FUNCTION valkey_fdw_test_vec_to_text(bytea) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_vec_to_text(bytea)
IS 'Raw little-endian FLOAT32 as a vector literal';

CREATE FUNCTION valkey_fdw_test_vec_from_text(text)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
REVOKE ALL ON FUNCTION valkey_fdw_test_vec_from_text(text) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_vec_from_text(text)
IS 'A vector literal as raw little-endian FLOAT32';

/*
 * Deferred write buffer diagnostics.
 *
 * These read the buffer; they never allocate in it, so a suite can compare
 * alloc_bytes before and after without the observation moving the number.
 *
 * Keys, members and field values are bytea for the same reason the probes
 * are: an embedded NUL and an invalidly encoded byte are exactly what the
 * write path has to carry, and both vanish through text.
 */
CREATE FUNCTION valkey_fdw_test_wbuf_stats(
    OUT live_ops    int,
    OUT alloc_bytes bigint,
    OUT generation  bigint,
    OUT bound       boolean,
    OUT serverid    oid,
    OUT umid        oid,
    OUT database    int,
    OUT max_ops     int,
    OUT max_bytes   bigint)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_wbuf_stats() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_wbuf_stats()
IS 'Live operation count, monotonic byte count and the bound Valkey unit';

CREATE FUNCTION valkey_fdw_test_wbuf_dump(
    OUT ordinal     int,
    OUT relid       oid,
    OUT op          text,
    OUT nest_level  int,
    OUT cid         bigint,
    OUT hashslot    int,
    OUT key         bytea,
    OUT oldkey      bytea,
    OUT member      bytea,
    OUT oldmember   bytea,
    OUT value       bytea,
    OUT score       bytea,
    OUT nfields     int,
    OUT tuple_natts int,
    OUT tuple_probe text)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_wbuf_dump() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_wbuf_dump()
IS 'Every buffered write operation in order, including its stored HeapTuple';

CREATE FUNCTION valkey_fdw_test_wbuf_fields(
    OUT ordinal   int,
    OUT field_no  int,
    OUT name      bytea,
    OUT value     bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_wbuf_fields() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_wbuf_fields()
IS 'Hash field names and values an operation holds; NULL value means HDEL';

CREATE FUNCTION valkey_fdw_test_wbuf_targets(
    OUT ordinal int,
    OUT kind    text,
    OUT attnum  int)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_wbuf_targets() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_wbuf_targets()
IS 'Columns each buffered operation was planned against; INSERT targets all, UPDATE only those assigned';

CREATE FUNCTION valkey_fdw_test_flush_stats(
    OUT calls         bigint,
    OUT empty_returns bigint,
    OUT flushes       bigint,
    OUT retries       bigint)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_flush_stats() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_flush_stats()
IS 'Backend-lifetime counters distinguishing "flush returned early" from "flush never ran"';

/*
 * The folded ledger: what COMMIT would tell the server, one row per check and
 * per action. Names rather than opcode numbers, and args as bytea, so a key
 * carrying a NUL renders as itself.
 */
CREATE FUNCTION valkey_fdw_test_ledger(
    OUT plan_no  int,
    OUT key      bytea,
    OUT tabletype text,
    OUT require  text,
    OUT state    text,
    OUT kind     text,
    OUT step_no  int,
    OUT op       text,
    OUT args     bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_ledger() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_ledger()
IS 'The write buffer folded into one plan per Valkey key';

/* The server-side program, its identity, and how its replies are read. */
CREATE FUNCTION valkey_fdw_test_script() RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_script() FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_script_sha1() RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_script_sha1() FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_script_classify(
    reply bytea, OUT verdict text, OUT detail text)
RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_script_classify(bytea) FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_opcodes(
    OUT kind text, OUT op_no int, OUT name text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT IMMUTABLE;
REVOKE ALL ON FUNCTION valkey_fdw_test_opcodes() FROM PUBLIC;

CREATE FUNCTION valkey_fdw_test_script_program(
    OUT ordinal int, OUT kind text, OUT value bytea)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_script_program() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_script_program()
IS 'The EVALSHA the current ledger encodes to, one row per RESP element';

CREATE FUNCTION valkey_fdw_test_reply_ceiling(bytes bigint)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION valkey_fdw_test_reply_ceiling(bigint) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_reply_ceiling(bigint)
IS 'Lower the libvalkey allocation ceiling for this backend; 0 restores the built-in value';

-- ---------------------------------------------------------------------------
-- Erase this server's keyspace, and refuse to erase anything else.
--
-- The fixtures need a keyspace they can empty between cases. FLUSHDB does
-- that, and does it to whatever server the connection happens to reach - which
-- is a fine instruction to give a container this harness started and a
-- catastrophic one to give anything else. test/bench/*.sh and the TAP test
-- take their host from $VALKEY_HOST, so "anything else" is one environment
-- variable away, and the failure is silent and total.
--
-- The guard is a mark the harness plants on the server it created, using
-- `docker exec` into a container it started by name - which is the one way to
-- know a server is disposable that cannot be spoofed by pointing a hostname
-- somewhere else. No mark, no flush.
--
-- The mark lives in database 15 and the flush empties database 0, so the two
-- do not touch. That matters because not every flush in this tree comes
-- through here - scan.sql still empties the keyspace with valkey-cli - and a
-- mark kept beside the data would be erased by the first of those, making
-- every guarded flush afterwards refuse a server that is perfectly disposable.
CREATE FUNCTION valkey_fdw_test_flush(server_name text)
RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    marked bool;
BEGIN
    SELECT count(*) > 0 INTO marked
    FROM valkey_fdw_test_probe(server_name, 15, 'GET', 'valkey_fdw:disposable')
    WHERE val_part = 'yes'::bytea;

    IF NOT marked THEN
        RAISE EXCEPTION 'refusing to FLUSHDB a Valkey this harness did not create'
            USING DETAIL = 'The key valkey_fdw:disposable is not set on server "'
                           || server_name || '".',
                  HINT = 'Bring a topology up with scripts/harness.sh, which '
                         'marks the container it starts. Nothing else is safe '
                         'to erase.';
    END IF;

    PERFORM valkey_fdw_test_probe(server_name, 0, 'FLUSHDB');
    RETURN 'flushed';
END $$;

REVOKE ALL ON FUNCTION valkey_fdw_test_flush(text) FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_flush(text)
IS 'FLUSHDB, but only on a server this harness marked as disposable';

CREATE FUNCTION valkey_fdw_test_overlay_stats(
    OUT rebuilds bigint,
    OUT extends  bigint)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_overlay_stats() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_overlay_stats()
IS 'How many times the overlay index was rebuilt from scratch, and how many times extended in place';

CREATE FUNCTION valkey_fdw_test_leak_stats(
    OUT scan_batch_contexts bigint,
    OUT scan_batch_resets   bigint,
    OUT flush_batches_open  bigint,
    OUT flush_batches_close bigint)
RETURNS record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;
REVOKE ALL ON FUNCTION valkey_fdw_test_leak_stats() FROM PUBLIC;

COMMENT ON FUNCTION valkey_fdw_test_leak_stats()
IS 'Scan batch contexts against rescan resets, and flush batches opened against closed - two pairings that must stay balanced';
