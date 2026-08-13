-- File descriptor accounting.
--
-- Opening a connection per statement and closing none of them on an error
-- path is the failure this counts. Because a raw socket belongs to no
-- ResourceOwner, an aborted statement leaks its descriptor outright and a
-- loop of failing statements exhausts the backend. `opened_total` shows that
-- connections are reused; only counting the backend's actual descriptors
-- shows that none are lost.

CREATE TEMP TABLE fdcount(n int);

-- The backend's own descriptor count, read from procfs. COPY FROM PROGRAM
-- needs the command as a literal, so the pid is interpolated by format().
CREATE FUNCTION backend_fds() RETURNS int
LANGUAGE plpgsql AS $$
DECLARE
    n int;
BEGIN
    DELETE FROM fdcount;
    EXECUTE format('COPY fdcount FROM PROGRAM %L',
                   'ls /proc/' || pg_backend_pid() || '/fd | wc -l');
    SELECT fdcount.n INTO n FROM fdcount LIMIT 1;
    RETURN n;
END $$;

CREATE SERVER leak_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER leak_srv;

-- Nothing listens here, so every attempt fails inside the connect path.
CREATE SERVER leak_badport FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6399', connect_timeout_ms '500');
CREATE USER MAPPING FOR CURRENT_USER SERVER leak_badport;

-- Warm the pool and settle the descriptor count before measuring.
SELECT valkey_fdw_test_pooled_ping('leak_srv') AS warmup;

-- ---------------------------------------------------------------------------
-- Positive control.
--
-- Every assertion below is of the form "the descriptor count did not grow",
-- which is worthless if the count never moves at all. Open connections to
-- eight distinct servers and require the count to rise, proving the
-- measurement is sensitive to exactly the thing a leak would produce.
-- ---------------------------------------------------------------------------
DO $$
DECLARE
    before_fds int;
    after_fds  int;
    i          int;
BEGIN
    before_fds := backend_fds();

    FOR i IN 1..8 LOOP
        EXECUTE format(
            'CREATE SERVER ctl_%s FOREIGN DATA WRAPPER valkey_fdw '
            'OPTIONS (host ''valkey'', port ''6379'')', i);
        EXECUTE format(
            'CREATE USER MAPPING FOR CURRENT_USER SERVER ctl_%s', i);
        PERFORM valkey_fdw_test_pooled_ping(format('ctl_%s', i));
    END LOOP;

    after_fds := backend_fds();

    -- At least one descriptor per connection. Not an exact equality: other
    -- parts of the backend may legitimately open a file in the same window,
    -- and which ones do varies by PostgreSQL version. What must hold is that
    -- all eight connections were counted.
    IF after_fds - before_fds < 8 THEN
        RAISE EXCEPTION
            'descriptor count rose by only % when 8 connections were opened '
            '(% -> %); the leak assertions below would not be sensitive to a '
            'per-connection leak',
            after_fds - before_fds, before_fds, after_fds;
    END IF;
    RAISE NOTICE 'all 8 opened connections were visible in the descriptor count';

    FOR i IN 1..8 LOOP
        EXECUTE format('DROP SERVER ctl_%s CASCADE', i);
    END LOOP;
END $$;

-- ---------------------------------------------------------------------------
-- 2000 aborted subtransactions, each having used the pooled connection.
--
-- Every iteration opens a subtransaction, drives a command, and unwinds. The
-- descriptor count must not move: the connection is reused across all of
-- them, and the aborts release nothing they should not.
-- ---------------------------------------------------------------------------
DO $$
DECLARE
    before_fds int;
    after_fds  int;
    i          int;
BEGIN
    before_fds := backend_fds();

    FOR i IN 1..2000 LOOP
        BEGIN
            PERFORM valkey_fdw_test_pooled_ping('leak_srv');
            RAISE EXCEPTION 'unwind';
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
    END LOOP;

    after_fds := backend_fds();

    IF after_fds > before_fds THEN
        RAISE EXCEPTION
            'descriptor count grew from % to % across 2000 aborted subtransactions',
            before_fds, after_fds;
    END IF;
    RAISE NOTICE '2000 aborted subtransactions leaked no descriptors';
END $$;

-- The connection survived all of it, rather than being reopened each time.
SELECT open, opened_total FROM valkey_fdw_test_pool_stats();

-- ---------------------------------------------------------------------------
-- 500 failed connects.
--
-- This is the path that goes wrong most directly: the connect fails, the
-- error path runs, and the half-built socket is never closed. Here the socket
-- is released before the error is reported, so the count must stay flat.
-- ---------------------------------------------------------------------------
DO $$
DECLARE
    before_fds int;
    after_fds  int;
    failures   int := 0;
    i          int;
BEGIN
    before_fds := backend_fds();

    FOR i IN 1..500 LOOP
        BEGIN
            PERFORM valkey_fdw_test_pooled_ping('leak_badport');
        EXCEPTION WHEN OTHERS THEN
            failures := failures + 1;
        END;
    END LOOP;

    after_fds := backend_fds();

    IF failures <> 500 THEN
        RAISE EXCEPTION 'expected 500 connect failures, saw %', failures;
    END IF;
    IF after_fds > before_fds THEN
        RAISE EXCEPTION
            'descriptor count grew from % to % across 500 failed connects',
            before_fds, after_fds;
    END IF;
    RAISE NOTICE '500 failed connects leaked no descriptors';
END $$;

-- ---------------------------------------------------------------------------
-- 500 commands the SERVER rejects, on an established connection.
--
-- The connection is open and healthy; each command comes back as an error
-- reply. The pool must neither leak the connection nor discard it needlessly.
--
-- The block that stood here PINGed successfully 500 times and then raised
-- 'unwind' by hand: not one command was rejected by anything, and it was
-- character-for-character the 2000-iteration block above with the count
-- changed, so any leak it could have caught that block already caught four
-- times over. An error REPLY arriving on a healthy connection is a different
-- path - the reply is taken, classified and reported while the connection
-- stays quiescent - and it was the one this file claimed to cover and did not.
--
-- WRONGTYPE against a key of the wrong type, because it is an error the server
-- generates from a well-formed command: an unknown verb would be refused by
-- the command table before anything touched a key.
--
-- What is observed is the CLASSIFICATION, not the row count. `count(*) = 0`
-- stood here and was worth nothing: zero rows is also what a scan produces if
-- the error reply is mistaken for an empty collection, if the command is never
-- sent, or if the table simply cannot return anything. EXPLAIN ANALYZE's
-- "Valkey Keys Skipped" says which of those happened - the key was reached,
-- its reply was taken, and the reply was recognised as carrying no row. Drop
-- VALKEY_REPLY_ERROR from vfdw_scan_reply_is_skippable and the count falls to
-- 0 while the row count stays at 0.
--
-- leak_str reads the same key with the right type, so every iteration proves
-- the stream is still aligned after the error reply was consumed, and that
-- zero rows above is a property of the reply rather than of the fixture.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_poke('leak_srv', 'vfdw:leak:str'::bytea,
                            'a string'::bytea) AS fixture;

CREATE FOREIGN TABLE leak_wrongtype (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER leak_srv OPTIONS (tabletype 'list', singleton_key 'vfdw:leak:str');

CREATE FOREIGN TABLE leak_str (
    k text OPTIONS (key 'true'),
    v text
) SERVER leak_srv OPTIONS (tabletype 'string', singleton_key 'vfdw:leak:str');

-- Only this wrapper's own EXPLAIN lines. The Foreign Scan line carries the
-- row count, which PostgreSQL 18 prints as 0.00 where 16 and 17 print 0, and
-- pinning that would make this file assert the server's formatting rather
-- than the wrapper's behaviour.
CREATE FUNCTION leak_valkey_lines(q text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
    line text;
BEGIN
    FOR line IN EXECUTE
        'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
        || q
    LOOP
        IF line LIKE '%Valkey %' THEN
            RETURN NEXT btrim(line);
        END IF;
    END LOOP;
END $$;

-- The shape of one iteration, recorded once so the loop below can assert
-- against it without printing 500 plans.
SELECT * FROM leak_valkey_lines('SELECT * FROM leak_wrongtype');

DO $$
DECLARE
    before_fds     int;
    after_fds      int;
    before_opened  bigint;
    after_opened   bigint;
    rejected       int := 0;
    i              int;
    v              text;
    skipped        int;
    plan_line      text;
BEGIN
    before_fds := backend_fds();
    SELECT opened_total INTO before_opened FROM valkey_fdw_test_pool_stats();

    FOR i IN 1..500 LOOP
        skipped := NULL;
        FOR plan_line IN
            SELECT * FROM leak_valkey_lines('SELECT * FROM leak_wrongtype')
        LOOP
            IF plan_line LIKE '%Valkey Keys Skipped:%' THEN
                skipped := substring(plan_line
                                     from 'Valkey Keys Skipped: ([0-9]+)')::int;
            END IF;
        END LOOP;

        IF skipped IS NULL THEN
            RAISE EXCEPTION
                'iteration %: the plan carried no Valkey Keys Skipped line', i;
        END IF;
        IF skipped <> 1 THEN
            RAISE EXCEPTION
                'iteration %: LRANGE on a string key was counted as % skipped '
                'keys, not 1; the WRONGTYPE reply was not classified as '
                'carrying no row', i, skipped;
        END IF;

        -- Same key, right type, same connection: the reply stream is aligned
        -- and this table shape does return rows when there is one to return.
        SELECT leak_str.v INTO v FROM leak_str;
        IF v IS DISTINCT FROM 'a string' THEN
            RAISE EXCEPTION
                'iteration %: the connection returned % after an error reply, '
                'not the value the key holds', i, coalesce(quote_literal(v),
                                                           'no row');
        END IF;

        rejected := rejected + 1;
    END LOOP;

    after_fds := backend_fds();
    SELECT opened_total INTO after_opened FROM valkey_fdw_test_pool_stats();

    IF rejected <> 500 THEN
        RAISE EXCEPTION 'expected 500 rejected commands, saw %', rejected;
    END IF;
    IF after_fds > before_fds THEN
        RAISE EXCEPTION 'descriptor count grew from % to %',
            before_fds, after_fds;
    END IF;
    -- A connection discarded and rebuilt after each error would keep the
    -- descriptor count flat while doing exactly what the pool must not do.
    IF after_opened <> before_opened THEN
        RAISE EXCEPTION
            'the pool opened % connections across 500 error replies; an error '
            'reply on a healthy connection must not cost the connection',
            after_opened - before_opened;
    END IF;
    RAISE NOTICE '500 server-rejected commands leaked no descriptors and cost no reconnect';
END $$;

SELECT valkey_fdw_test_pooled_ping('leak_srv') AS still_usable;

SELECT valkey_fdw_test_poke('leak_srv', 'vfdw:leak:str'::bytea, NULL) AS cleaned;
DROP FOREIGN TABLE leak_wrongtype;
DROP FOREIGN TABLE leak_str;

DROP FUNCTION leak_valkey_lines(text);
DROP FUNCTION backend_fds();
DROP SERVER leak_srv CASCADE;
DROP SERVER leak_badport CASCADE;
