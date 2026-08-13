/*-------------------------------------------------------------------------
 *
 * vfdw_tls.h
 *		TLS transport for valkey_fdw.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_tls.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_TLS_H
#define VFDW_TLS_H

#include "vfdw.h"

#include <valkey/valkey.h>

#include "vfdw_option.h"

/*
 * Negotiate TLS on an already-connected socket.
 *
 * Note that the handshake is not necessarily finished when this returns: on
 * a non-blocking context libvalkey installs the TLS function table and lets
 * SSL_connect complete during subsequent reads and writes. Callers must
 * force a round trip afterwards if they want certificate failures reported
 * as TLS problems rather than as command failures.
 */
extern void vfdw_tls_attach(valkeyContext *conn, const VfdwServerOptions *opts);

/*
 * Take the most recent OpenSSL error, or NULL if the queue is empty.
 *
 * A TLS failure surfaces through libvalkey as a generic I/O error whose
 * errstr is derived from errno - and errno is 0 when the cause was
 * certificate verification, so the message reads "Success". The real reason
 * is only in OpenSSL's queue.
 */
extern char *vfdw_tls_take_error(void);

/*
 * Why the peer certificate was rejected, or NULL if it was not.
 *
 * OpenSSL's queue says only "certificate verify failed", which is the same
 * string for a wrong hostname, an untrusted CA and an expired certificate. A
 * message carrying only that cannot say which check refused the connection,
 * and neither can a test asserting it: an expiry regression and a broken CA
 * load produce identical output. Points into OpenSSL's static table, so it is
 * never freed; taken rather than read, so one handshake's reason cannot be
 * reported against the next.
 *
 * *hint receives advice matching the check that refused, and is set on every
 * call including the one that returns NULL. A single hint for every failure
 * was still misdirection: it named tls_ca_file and the hostname, which are
 * exactly what is CORRECT when a certificate has only expired.
 */
extern const char *vfdw_tls_verify_reason(const char **hint);

#endif							/* VFDW_TLS_H */
