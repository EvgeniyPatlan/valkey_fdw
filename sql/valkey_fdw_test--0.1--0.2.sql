-- valkey_fdw_test 0.1 -> 0.2
--
-- The three diagnostic entry points added after 0.1 was cut: a guarded
-- FLUSHDB, and the two counters that make a bulk write's index rebuilds and a
-- rescan's batch contexts assertable.
--
-- They were first written straight into valkey_fdw_test--0.1.sql, which is the
-- habit this file exists to replace. Nothing had been released, so editing a
-- cut version in place cost nothing that time; the next time it would silently
-- give two installations of "0.1" different catalogs.

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
