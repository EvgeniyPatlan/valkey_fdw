-- TLS transport and certificate verification.
--
-- The interesting assertion is the wrong-hostname one. libvalkey 0.5.0 wires
-- its server_name into SNI only and never calls X509_VERIFY_PARAM_set1_host,
-- so a certificate signed by a trusted CA but issued for a different host
-- passes its checks. valkey_fdw builds its own SSL_CTX precisely so that
-- tls_verify = 'full' means what it says.
--
-- valkey-othername resolves to the same server as valkey; the server
-- certificate names only 'valkey'.

-- A correctly named peer, fully verified.
CREATE SERVER tls_full FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', tls 'true',
             tls_ca_file '/tls/ca.crt', tls_verify 'full');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_full;
SELECT valkey_fdw_test_pooled_ping('tls_full') AS verified_full;

-- The same server reached under a name its certificate does not carry.
-- Chain validation passes; identity checking must not.
CREATE SERVER tls_wronghost FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-othername', port '6379', tls 'true',
             tls_ca_file '/tls/ca.crt', tls_verify 'full');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_wronghost;
SELECT valkey_fdw_test_pooled_ping('tls_wronghost');

-- Under 'ca' the chain is still checked but the name is not, so the same
-- connection succeeds. The contrast with the previous statement is the
-- entire point of having two modes.
CREATE SERVER tls_ca FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-othername', port '6379', tls 'true',
             tls_ca_file '/tls/ca.crt', tls_verify 'ca');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_ca;
SELECT valkey_fdw_test_pooled_ping('tls_ca') AS name_unchecked_under_ca;

-- No trust anchor: the system store does not contain our test CA, so a
-- verified connection must fail rather than fall back to trusting anything.
CREATE SERVER tls_nocafile FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', tls 'true', tls_verify 'full');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_nocafile;
SELECT valkey_fdw_test_pooled_ping('tls_nocafile');

-- Verification off: connects regardless. Encrypted, but unauthenticated.
CREATE SERVER tls_none FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-othername', port '6379', tls 'true',
             tls_verify 'none');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_none;
SELECT valkey_fdw_test_pooled_ping('tls_none') AS unverified;

-- Plaintext against a TLS-only server: the plain port is disabled entirely,
-- so this must fail to connect rather than quietly succeed in the clear.
CREATE SERVER tls_plaintext FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_plaintext;
SELECT valkey_fdw_test_pooled_ping('tls_plaintext');

-- ---------------------------------------------------------------------------
-- An expired certificate.
--
-- valkey-expired serves a certificate issued for its own hostname by the same
-- CA, whose validity window closed a month ago. Chain and identity are both
-- in order, so the only thing left to reject it for is the dates.
--
-- The rejection now names the check that made it. It did not: OpenSSL's queue
-- says "certificate verify failed" for a wrong hostname, an untrusted CA and
-- an expired certificate alike, so all four negative cases in this file
-- recorded the identical three lines and nothing here could tell an expiry
-- regression from a broken CA load. Compare the DETAIL below with the wrong
-- hostname and missing-trust-anchor cases above: no two of the three may be
-- the same string.
--
-- Expiry is part of chain validation, so 'ca' must refuse it just as 'full'
-- does. Only turning verification off entirely gets a connection.
-- ---------------------------------------------------------------------------
CREATE SERVER tls_expired_full FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-expired', port '6379', tls 'true',
             tls_ca_file '/tls/ca.crt', tls_verify 'full');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_expired_full;
SELECT valkey_fdw_test_pooled_ping('tls_expired_full');

CREATE SERVER tls_expired_ca FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-expired', port '6379', tls 'true',
             tls_ca_file '/tls/ca.crt', tls_verify 'ca');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_expired_ca;
SELECT valkey_fdw_test_pooled_ping('tls_expired_ca');

CREATE SERVER tls_expired_none FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey-expired', port '6379', tls 'true',
             tls_verify 'none');
CREATE USER MAPPING FOR CURRENT_USER SERVER tls_expired_none;
SELECT valkey_fdw_test_pooled_ping('tls_expired_none') AS expired_unverified;

-- ---------------------------------------------------------------------------
-- The three rejections are three different messages, asserted as such.
--
-- Recording them separately above already makes a collapse show up as a diff,
-- but only to someone reading three widely separated blocks. This states the
-- property directly: wrong host, untrusted CA and expired certificate must
-- produce three distinct DETAILs and three distinct HINTs.
--
-- The HINT half is the newer half and was the last thing here still saying the
-- same words to everyone. "Check tls_ca_file, and that the server certificate
-- names the host" is advice about the two things that are CORRECT when a
-- certificate has merely expired, so it sent the reader to re-examine exactly
-- what was not wrong.
-- ---------------------------------------------------------------------------
CREATE FUNCTION tls_failure(srv text, OUT detail text, OUT hint text)
LANGUAGE plpgsql AS $$
BEGIN
    BEGIN
        PERFORM valkey_fdw_test_pooled_ping(srv);
        detail := 'no failure'; hint := 'no failure';
    EXCEPTION WHEN OTHERS THEN
        GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL,
                                hint   = PG_EXCEPTION_HINT;
    END;
END $$;

DO $$
DECLARE
    wronghost  record;
    noanchor   record;
    expired    record;
BEGIN
    SELECT * INTO wronghost FROM tls_failure('tls_wronghost');
    SELECT * INTO noanchor  FROM tls_failure('tls_nocafile');
    SELECT * INTO expired   FROM tls_failure('tls_expired_full');

    IF wronghost.detail = noanchor.detail
       OR wronghost.detail = expired.detail
       OR noanchor.detail = expired.detail THEN
        RAISE EXCEPTION
            'two of the three rejections gave the same DETAIL, so this file '
            'cannot tell them apart: wrong host [%], no anchor [%], expired [%]',
            wronghost.detail, noanchor.detail, expired.detail;
    END IF;

    IF wronghost.hint = noanchor.hint
       OR wronghost.hint = expired.hint
       OR noanchor.hint = expired.hint THEN
        RAISE EXCEPTION
            'two of the three rejections gave the same HINT: wrong host [%], '
            'no anchor [%], expired [%]',
            wronghost.hint, noanchor.hint, expired.hint;
    END IF;

    RAISE NOTICE 'wrong host, untrusted CA and expired each report their own DETAIL and HINT';
END $$;

DROP FUNCTION tls_failure(text);

DROP SERVER tls_expired_full CASCADE;
DROP SERVER tls_expired_ca CASCADE;
DROP SERVER tls_expired_none CASCADE;

DROP SERVER tls_full CASCADE;
DROP SERVER tls_wronghost CASCADE;
DROP SERVER tls_ca CASCADE;
DROP SERVER tls_nocafile CASCADE;
DROP SERVER tls_none CASCADE;
DROP SERVER tls_plaintext CASCADE;
