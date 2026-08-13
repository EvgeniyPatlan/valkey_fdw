-- The connection and I/O layer.
--
-- Every assertion here pins a specific way this layer can break. It is
-- reached through the diagnostic functions rather than through a scan,
-- because it is built before any scan path exists and its error handling is
-- the part most likely to rot untested.

-- A connection can be opened, driven and closed with no blocking libvalkey
-- call anywhere in the path.
SELECT valkey_fdw_test_ping('valkey', 6379, 5000, 5000) AS ping;

-- ---------------------------------------------------------------------------
-- Pipelining.
--
-- The naive shape is one round trip per row. 500 commands in flight, with
-- the replies matched back to their requests in order, is the primitive that
-- replaces it.
-- ---------------------------------------------------------------------------
CREATE SERVER io_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER io_srv;

SELECT valkey_fdw_test_pipeline('io_srv', 500) = 500
    AS all_500_pipelined_values_matched;

-- A deliberately small pipeline_batch forces the auto-flush path: 500
-- commands cannot all sit in the output buffer at once, so the batch must
-- flush and refill repeatedly while still pairing every reply correctly.
CREATE SERVER io_srv_tiny FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', pipeline_batch '7');
CREATE USER MAPPING FOR CURRENT_USER SERVER io_srv_tiny;

SELECT valkey_fdw_test_pipeline('io_srv_tiny', 500) = 500
    AS matched_with_pipeline_batch_7;

-- That number is 500 at every depth, so on its own it says pipelining works
-- and nothing about the configured bound: delete the line that reads the
-- option, hardcode any depth, and the assertion above does not move. The flush
-- count is the bound's only observable consequence.
--
-- 500 commands at 7 in flight is 71 flushes; at the default 256 it is 1, since
-- the auto-flush fires once and the rest go out with the drain. Both numbers
-- are exact - they are the wrapper's own arithmetic, not the server's - so
-- they are asserted as numbers and not as an inequality.
SELECT valkey_fdw_test_batch_flushes('io_srv_tiny', 500)
    AS flushes_at_batch_7;
SELECT valkey_fdw_test_batch_flushes('io_srv', 500)
    AS flushes_at_default_batch;
SELECT valkey_fdw_test_batch_flushes('io_srv_tiny', 500)
     > valkey_fdw_test_batch_flushes('io_srv', 500)
       AS a_smaller_batch_flushes_more_often;

-- ---------------------------------------------------------------------------
-- Binary safety.
--
-- Valkey values are byte strings; PostgreSQL text is not. Handing reply->str
-- to C string APIs silently truncates any value containing a zero byte at
-- that byte. These must round-trip whole.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_binary('valkey', 6379, '\x610062'::bytea) = '\x610062'::bytea
    AS embedded_nul_survives;

SELECT length(valkey_fdw_test_binary('valkey', 6379, '\x00000000'::bytea)) AS all_nul_length;

-- Bytes that are not valid UTF-8 must come back unchanged rather than being
-- mangled or rejected on the way through.
SELECT valkey_fdw_test_binary('valkey', 6379, '\xfffefd'::bytea) = '\xfffefd'::bytea
    AS invalid_utf8_survives;

-- A value spanning many reads exercises the incremental reader rather than a
-- single recv.
SELECT length(valkey_fdw_test_binary('valkey', 6379,
              decode(repeat('4142', 200000), 'hex'))) AS large_value_length;

-- ---------------------------------------------------------------------------
-- Connection failures.
--
-- Each must arrive as a specific, classified error with the underlying cause
-- in DETAIL - and, critically, must not read the freed context to build that
-- message, which is the standing hazard on every one of the connect paths.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_ping('valkey', 6399, 2000, 2000);

-- An unroutable address: the connect deadline must fire rather than hanging
-- for the kernel's own much longer timeout.
SELECT valkey_fdw_test_ping('10.255.255.1', 6379, 300, 300);

-- ---------------------------------------------------------------------------
-- Timeouts and interruptibility.
-- ---------------------------------------------------------------------------

-- BLPOP parks this client on the server for 30 seconds. Our own command
-- deadline is 500ms, so the deadline must win. Reaching this error at all
-- proves control returned to us mid-wait: a blocking libvalkey call would
-- have sat in recv() for the full 30 seconds.
SELECT valkey_fdw_test_block('valkey', 6379, 30, 500);

-- The same wait, interrupted by PostgreSQL rather than by our deadline.
-- statement_timeout is delivered by setting the process latch, so this only
-- fires promptly if the wait is a WaitLatchOrSocket and not a bare recv().
-- The elapsed-time check is what makes this a real assertion: without latch
-- wakeup the statement would run the full 30 seconds and still error.
SET statement_timeout = '2s';
DO $$
DECLARE
    started timestamptz;
    elapsed interval;
BEGIN
    started := clock_timestamp();
    BEGIN
        PERFORM valkey_fdw_test_block('valkey', 6379, 30, 60000);
        RAISE EXCEPTION 'expected the statement timeout to fire';
    EXCEPTION WHEN query_canceled THEN
        elapsed := clock_timestamp() - started;
    END;

    IF elapsed IS NULL THEN
        RAISE EXCEPTION 'statement was not cancelled';
    ELSIF elapsed > interval '10 seconds' THEN
        RAISE EXCEPTION
            'cancellation took %, so the wait was not interruptible', elapsed;
    END IF;

    RAISE NOTICE 'a 30s Valkey wait was cancelled by statement_timeout in under 10s';
END $$;

RESET statement_timeout;

-- ---------------------------------------------------------------------------
-- The reader's element bound.
--
-- A multi-bulk header declares how many elements follow before any of them
-- arrive, so an unbounded reader commits to whatever the far end claims. This
-- is the one bound libvalkey offers as a setting; the byte ceiling in the
-- section after it is the same argument one level down, where the library
-- offers nothing and the allocator has to carry it.
--
-- Configuring the bound is what makes it testable at all - set it below an
-- ordinary reply and watch the same read succeed under the default.
-- ---------------------------------------------------------------------------
CREATE TEMP TABLE limout(line text);
CREATE FUNCTION lim_vk(cmd text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM limout;
    EXECUTE format('COPY limout FROM PROGRAM %L',
                   'valkey-cli -h valkey ' || cmd || ' >/dev/null 2>&1; echo ok');
END $$;

-- A list of 500 members, read under a 100-element ceiling.
DO $$
BEGIN
    PERFORM lim_vk(format(
        'EVAL "for i=1,500 do server.call(''RPUSH'', ''vfdw:lim'', i) end return 1" 0'));
END $$;

CREATE SERVER io_srv_capped FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', max_reply_elements '100');
CREATE USER MAPPING FOR CURRENT_USER SERVER io_srv_capped;
CREATE FOREIGN TABLE lim_capped (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER io_srv_capped OPTIONS (tabletype 'list', keyprefix 'vfdw:lim');

SELECT count(*) FROM lim_capped;

-- The same list under the default ceiling reads in full, so what failed above
-- was the bound and not the data.
CREATE FOREIGN TABLE lim_default (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER io_srv OPTIONS (tabletype 'list', keyprefix 'vfdw:lim');
SELECT count(*) AS members_under_default_bound FROM lim_default;

-- The bound is validated like any other option rather than silently clamped.
CREATE SERVER io_srv_bad FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', max_reply_elements '0');

SELECT lim_vk('DEL vfdw:lim');
DROP FOREIGN TABLE lim_capped, lim_default;
DROP SERVER io_srv_capped CASCADE;
DROP FUNCTION lim_vk(text);

-- ---------------------------------------------------------------------------
-- The reader's byte ceiling.
--
-- The element bound above cannot reach this case: one bulk string is one
-- element, and it is allocated from the length its own header declares. So a
-- peer answering GET with a gigabyte header is refused by nothing libvalkey
-- offers - and under tls 'false' or tls_verify 'none' that peer need not be
-- the server the operator configured. A ceiling in the library's allocator is
-- enforced instead: it returns NULL, which libvalkey turns into its own OOM
-- and which arrives here as 53200.
--
-- The ceiling is FIXED in the shipped code, because the allocator hook is
-- process-global and a per-server option let any caller holding USAGE lower
-- it for every other server in the backend. Moving it is therefore confined
-- to valkey_fdw_test, which is superuser-only, and this suite puts it back.
--
-- Asserted as a SQLSTATE and not as message text: the wording belongs to
-- libvalkey, the code is what a program branches on. Reading the same value
-- back under the default ceiling is what says the ceiling refused it and not
-- the data.
-- ---------------------------------------------------------------------------
CREATE FUNCTION io_reply_sqlstate(srv text, k bytea) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    n bigint;
BEGIN
    SELECT sum(length(val_part)) INTO n
      FROM valkey_fdw_test_probe(srv, 0, 'GET'::bytea, k);
    RETURN 'read ' || n || ' bytes';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

-- A quarter of a megabyte, written through the default ceiling so that the
-- fixture is not itself what the bound is being tested against.
SELECT valkey_fdw_test_poke('io_srv', 'vfdw:big',
                            decode(repeat('61', 262144), 'hex'));

SELECT valkey_fdw_test_reply_ceiling(65536) AS ceiling_lowered;

SELECT io_reply_sqlstate('io_srv', 'vfdw:big') AS quarter_mb_under_64k;

-- Put it back, and read the same value again. The second read is what says
-- the ceiling refused the first one and not the data, and restoring it is
-- this section's own responsibility: the setting is process-global and
-- outlives the statement, exactly like the thing it moves.
SELECT valkey_fdw_test_reply_ceiling(0) AS ceiling_restored;

SELECT io_reply_sqlstate('io_srv', 'vfdw:big') AS quarter_mb_under_default;

SELECT valkey_fdw_test_poke('io_srv', 'vfdw:big');
DROP FUNCTION io_reply_sqlstate(text, bytea);

-- ---------------------------------------------------------------------------
-- A pipeline far larger than the socket buffers between us and the server.
--
-- The flush waits only for the socket to become writable and never drains
-- replies while it waits, which looks like a deadlock: fill the server's
-- output buffer, the server stops reading, our writes never complete. It is
-- not one. Valkey ships client-output-buffer-limit "normal 0 0 0" - no limit
-- at all for ordinary clients - so the server buffers replies in its own
-- memory and keeps reading. An operator who sets a finite limit does not get
-- the deadlock either: Valkey closes such a client rather than stalling it,
-- which arrives here as a connection error.
--
-- So this asserts the thing that is actually true and was never covered: a
-- pipeline two orders of magnitude past the earlier ones completes, rather
-- than ending at the command deadline.
-- ---------------------------------------------------------------------------
-- Both deadlines have to move, and only one of them is PostgreSQL's.
--
-- statement_timeout bounds the statement; command_timeout_ms bounds the wait
-- for one Valkey reply, and it is the one this vector is written against - so
-- leaving it at its 30s default asserts the opposite of what the paragraph
-- above claims, and does it only on machines slow enough to notice. Under the
-- coverage build every command carries instrumentation, which is exactly such
-- a machine.
--
-- The number is a ceiling and not a budget: nothing here is timing anything,
-- and a run that needs two minutes has still proved that the pipeline drains
-- rather than deadlocks.
ALTER SERVER io_srv OPTIONS (ADD command_timeout_ms '300000');
SET statement_timeout = '600s';
SELECT valkey_fdw_test_pipeline('io_srv', 200000) = 200000 AS large_pipeline_completed;
ALTER SERVER io_srv OPTIONS (DROP command_timeout_ms);
RESET statement_timeout;

DROP SERVER io_srv CASCADE;
DROP SERVER io_srv_tiny CASCADE;
