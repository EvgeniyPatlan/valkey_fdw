/*-------------------------------------------------------------------------
 *
 * vfdw_io.h
 *		Interruptible I/O against Valkey.
 *
 * Invariant I6: every wait is interruptible. valkey_fdw never calls a
 * libvalkey function that can block. Sockets are non-blocking, bytes are
 * moved with valkeyBufferRead/valkeyBufferWrite, replies are parsed with
 * valkeyGetReplyFromReader, and the waiting in between goes through
 * WaitLatchOrSocket over the connection socket and MyLatch. That is what
 * makes a scan cancellable with Ctrl-C or pg_cancel_backend, and what stops
 * a stalled server from pinning a backend forever.
 *
 * The design is verified end to end by test/spike/v1_nonblocking_io.c.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_io.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_IO_H
#define VFDW_IO_H

#include "vfdw.h"

#include <valkey/valkey.h>

#include "utils/timestamp.h"

/*
 * Absolute deadline for an operation, or 0 for "no deadline". Deadlines are
 * absolute rather than per-call timeouts so that a multi-round-trip operation
 * cannot extend its own budget one wait at a time.
 */
extern TimestampTz vfdw_io_deadline(int timeout_ms);

/*
 * Open a non-blocking connection and complete the connect.
 *
 * unix_path, when non-NULL, takes precedence over host and port. On failure
 * this reports the error and does not return; the half-built context is
 * released first, because at that point nothing else has a reference to it.
 */
extern valkeyContext *vfdw_io_connect(const char *host, int port,
									  const char *unix_path,
									  int connect_timeout_ms);

/*
 * Bound what a single reply may cost us, so that a hostile or accidentally
 * enormous reply cannot exhaust backend memory before we ever see it.
 */
extern void vfdw_io_set_limits(valkeyContext *conn, size_t maxbuf,
							   long maxelements);

/*
 * The ceiling on any single libvalkey allocation.
 *
 * Fixed rather than configurable, and vfdw_io.c says why at length: the hook
 * is process-global, so a per-server value let an unprivileged caller lower it
 * for every other server in the backend.
 *
 * The bound the reader itself offers counts elements, not bytes, so this is
 * the only thing standing between a backend and a bulk string whose length
 * the far end chose. It is coarse - one ceiling for the whole library, and
 * approximate in size - and vfdw_io.c states exactly how. Installing it is
 * idempotent and happens on the first connection open; there is one value and
 * nothing a statement can reach moves it.
 */
#define VFDW_MAX_REPLY_BYTES ((size_t) 268435456)

extern void vfdw_io_install_alloc_ceiling(void);
extern void vfdw_io_set_alloc_ceiling_for_test(int64 bytes);

/* Drain the output buffer. */
extern void vfdw_io_flush(valkeyContext *conn, TimestampTz deadline);

/*
 * Return the next reply, waiting as needed. Ownership passes to the caller,
 * which must freeReplyObject() it. A server error reply is returned rather
 * than raised: callers that handle MOVED, ASK or NOSCRIPT themselves need to
 * inspect it first.
 */
extern valkeyReply *vfdw_io_get_reply(valkeyContext *conn, TimestampTz deadline);

#endif							/* VFDW_IO_H */
