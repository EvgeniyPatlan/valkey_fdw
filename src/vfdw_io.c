/*-------------------------------------------------------------------------
 *
 * vfdw_io.c
 *		Interruptible I/O against Valkey.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_io.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_io.h"

#include <sys/socket.h>

#include "miscadmin.h"
#include "storage/latch.h"
#if PG_VERSION_NUM >= 180000
#include "storage/waiteventset.h"
#endif
#include "utils/builtins.h"
#include "utils/wait_event.h"

#include "vfdw_error.h"

TimestampTz
vfdw_io_deadline(int timeout_ms)
{
	if (timeout_ms <= 0)
		return 0;
	return TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms);
}

/*
 * Report a failure while we are still the only owner of the connection.
 *
 * Ordering matters and is the whole point: copy the message, then release,
 * then report. Reporting first and releasing after is unreachable code after
 * a longjmp; releasing first and reporting after reads freed memory. Only
 * connect-time callers may use this - once a connection is handed to the
 * pool it belongs to the pool, and the transaction callback is the only thing
 * that closes it.
 */
static void
vfdw_io_fail_and_close(valkeyContext *conn, const char *what)
{
	char	   *detail = pstrdup(conn->errstr);
	int			sqlerrcode = vfdw_errcode_for_context(conn);

	valkeyFree(conn);

	vfdw_ereport(ERROR, sqlerrcode, what, detail);
	pg_unreachable();
}

/*
 * Wait for the socket to become readable or writable.
 *
 * WaitLatchOrSocket rather than a bare poll(): it wakes on MyLatch, so a
 * query cancellation or a shutdown request interrupts the wait immediately,
 * and it exits on postmaster death rather than lingering as an orphan.
 */
static void
vfdw_io_wait(valkeyContext *conn, int socket_event, TimestampTz deadline,
			 const char *what)
{
	long		timeout_ms = -1;
	int			wait_events;
	int			rc;

	CHECK_FOR_INTERRUPTS();

	if (deadline != 0)
	{
		long		remaining;

		remaining = (long) ((deadline - GetCurrentTimestamp()) / 1000);
		if (remaining <= 0)
			vfdw_ereport(ERROR, ERRCODE_CONNECTION_FAILURE,
						 "timed out communicating with Valkey", what);
		timeout_ms = remaining;
	}

	wait_events = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | socket_event;
	if (timeout_ms >= 0)
		wait_events |= WL_TIMEOUT;

	rc = WaitLatchOrSocket(MyLatch, wait_events, conn->fd, timeout_ms,
						   PG_WAIT_EXTENSION);

	ResetLatch(MyLatch);

	/*
	 * The latch may have been set by a cancel or terminate request. Acting on
	 * it here is what makes a long scan abortable; without this the backend
	 * would spin until the server answered.
	 */
	CHECK_FOR_INTERRUPTS();

	if (rc & WL_TIMEOUT)
		vfdw_ereport(ERROR, ERRCODE_CONNECTION_FAILURE,
					 "timed out communicating with Valkey", what);
}

/*
 * Finish a connect that is still in flight.
 *
 * A non-blocking connect returns before the handshake completes, so
 * writability plus a zero SO_ERROR is what actually establishes success.
 * Split out so the caller stays readable and so the failure paths - each of
 * which must release the half-built socket - are visible together.
 */
static void
vfdw_io_complete_connect(valkeyContext *conn, TimestampTz deadline)
{
	int			soerr = 0;
	socklen_t	soerrlen = sizeof(soerr);

	PG_TRY();
	{
		vfdw_io_wait(conn, WL_SOCKET_WRITEABLE, deadline,
					 "establishing a connection");
	}
	PG_CATCH();
	{
		/* Still sole owner, so the socket has to be closed here or it leaks. */
		valkeyFree(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &soerr, &soerrlen) != 0)
		vfdw_io_fail_and_close(conn, "could not read connection status");

	if (soerr != 0)
	{
		char	   *detail = pstrdup(strerror(soerr));

		valkeyFree(conn);
		vfdw_ereport(ERROR, ERRCODE_CONNECTION_FAILURE,
					 "could not connect to Valkey", detail);
	}

	/*
	 * The socket is connected, so any error state libvalkey recorded while
	 * the connect was still in flight is stale. Clearing it lets the data
	 * path start from a clean slate; leaving it set would make the first
	 * valkeyBufferWrite fail its "return early when the context has seen an
	 * error" check.
	 */
	conn->err = 0;
	conn->errstr[0] = '\0';
}

valkeyContext *
vfdw_io_connect(const char *host, int port, const char *unix_path,
				int connect_timeout_ms)
{
	valkeyOptions opts;
	struct timeval connect_timeout;
	valkeyContext *conn;
	TimestampTz deadline;

	/*
	 * Here rather than anywhere that reads options: this is the one function
	 * every connection goes through, including the diagnostic entry points
	 * that are handed a host and a port and never look at a server at all.
	 */
	vfdw_io_install_alloc_ceiling();

	memset(&opts, 0, sizeof(opts));

	if (unix_path != NULL)
		VALKEY_OPTIONS_SET_UNIX(&opts, unix_path);
	else
		VALKEY_OPTIONS_SET_TCP(&opts, host, port);

	/*
	 * Non-blocking is not an optimisation here, it is a correctness
	 * requirement: libvalkey only treats EWOULDBLOCK as benign when
	 * VALKEY_BLOCK is clear (net.c, valkeyNetRead/valkeyNetWrite). A blocking
	 * context would turn every would-block into a hard I/O error.
	 */
	opts.options |= VALKEY_OPT_NONBLOCK;

	connect_timeout.tv_sec = connect_timeout_ms / 1000;
	connect_timeout.tv_usec = (connect_timeout_ms % 1000) * 1000;
	opts.connect_timeout = &connect_timeout;

	conn = valkeyConnectWithOptions(&opts);
	if (conn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory allocating a Valkey connection")));

	/*
	 * An immediate hard failure - an unresolvable host, say - leaves no usable
	 * socket. Anything else is a connect in progress, which the writability
	 * wait below resolves.
	 */
	if (conn->err && conn->fd == VALKEY_INVALID_FD)
		vfdw_io_fail_and_close(conn, "could not connect to Valkey");

	deadline = vfdw_io_deadline(connect_timeout_ms);
	vfdw_io_complete_connect(conn, deadline);

	return conn;
}

/*
 * The ceiling on one libvalkey allocation.
 *
 * Carries the built-in value from the start. Nothing can be refused on behalf
 * of a ceiling nobody set, because the wrappers this governs are unreachable
 * until valkeySetAllocators has installed them.
 */
static size_t vfdw_alloc_ceiling = VFDW_MAX_REPLY_BYTES;

/*
 * malloc and not palloc, throughout.
 *
 * libvalkey memory outlives the MemoryContext any one call runs in - a pooled
 * connection spans transactions, and freeReplyObject can run from a memory
 * context reset callback - so it has to come from the allocator libvalkey
 * itself hands it back to. Refusing is a return of NULL and nothing else: the
 * library records the failure and unwinds its own half-built objects, and
 * invariant I1 leaves the connection to the transaction callback that
 * discards it.
 */
/*
 * One place decides, and every wrapper asks it.
 *
 * Four independent comparisons would be the obvious spelling and are the wrong
 * one, for a reason that has nothing to do with duplication. A reply large
 * enough to matter is built in stages - the read buffer grows, then the bulk
 * string is taken from its declared length - so two different wrappers each
 * refuse it, and removing the ceiling from either one on its own changes
 * nothing that a suite can see. That is a defect NO SINGLE EDIT CAN
 * REINTRODUCE, which sounds like a virtue and is not: scripts/mutate.py proves
 * an assertion still asserts by putting the defect back, so a bound spread
 * over four comparisons is a bound the manifest cannot express, and io.sql
 * would go on passing whether the ceiling worked or not.
 *
 * Both were tried and both stayed green. Hence one predicate: it is the thing
 * the manifest mutates, and the thing every wrapper consults.
 */
static bool
vfdw_alloc_refused(size_t size)
{
	return size > vfdw_alloc_ceiling;
}

static void *
vfdw_alloc_malloc(size_t size)
{
	if (vfdw_alloc_refused(size))
		return NULL;
	return malloc(size);
}

static void *
vfdw_alloc_calloc(size_t nmemb, size_t size)
{
	/*
	 * Two questions, and only the second is the ceiling. The product is what
	 * would overflow, so its own overflow is ruled out first - against
	 * SIZE_MAX, which is arithmetic and not policy - and the ceiling is then
	 * asked about a product that is known to be representable.
	 */
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;
	if (vfdw_alloc_refused(nmemb * size))
		return NULL;
	return calloc(nmemb, size);
}

static void *
vfdw_alloc_realloc(void *ptr, size_t size)
{
	if (vfdw_alloc_refused(size))
		return NULL;
	return realloc(ptr, size);
}

static char *
vfdw_alloc_strdup(const char *str)
{
	/*
	 * Built on the malloc wrapper rather than calling strdup, so there is one
	 * place the ceiling is applied and no second allocator to keep in step.
	 * This is also the one wrapper that measures its argument, and the one
	 * whose argument is a C string by contract - libvalkey duplicates host
	 * names and option strings, never reply bytes - so strlen here does not
	 * meet invariant I3.
	 */
	size_t		size = strlen(str) + 1;
	char	   *copy = vfdw_alloc_malloc(size);

	if (copy == NULL)
		return NULL;

	memcpy(copy, str, size);
	return copy;
}

/*
 * Bound a reply by bounding the allocator, because there is nothing else.
 *
 * A multi-bulk header declares its element count and the reader can refuse it
 * (vfdw_io_set_limits), but a bulk string is allocated from the length its own
 * header declares and libvalkey exposes no setting that refuses one. A peer
 * answering GET with $1073741824 therefore gets a gigabyte of backend memory
 * before any of our code sees a byte of it - and under tls 'false' or
 * tls_verify 'none' that peer need not be the server the operator configured.
 * The allocator is the only hook the library offers, so the bound lives there:
 * NULL becomes VALKEY_ERR_OOM inside libvalkey, which vfdw_errcode_for_context
 * already reports as ERRCODE_OUT_OF_MEMORY.
 *
 * THE CEILING IS FIXED, AND THAT IS THE POINT.
 *
 * It was a server option first, and that shape was wrong in a way worth
 * recording. valkeySetAllocators installs ONE set of function pointers for
 * the whole process, so a per-server value makes a process-global weapon out
 * of a tuning knob: options are read before the password check and before the
 * cluster refusal, so any role holding USAGE on a server defined with a small
 * value could lower the ceiling for every other server in the backend merely
 * by ATTEMPTING a connection that was then refused. A hardening change that
 * hands an unprivileged caller a cross-server denial of service is worse than
 * the unbounded allocation it set out to prevent.
 *
 * Nothing user-controllable reaches it now. The bound is a safety net against
 * a peer that lies about a length - which under tls 'false' or
 * tls_verify 'none' need not be the server the operator configured at all -
 * and a safety net does not need to be adjustable per server.
 *
 * Installed lazily from vfdw_io_connect rather than from a reader of options,
 * because that is the one function EVERY connection goes through: the
 * diagnostic entry points that take a host and a port never read a server's
 * options at all, and arming from there left exactly those outside the bound.
 *
 * Two honest limits remain. It bounds every libvalkey allocation and not only
 * a reply - the command buffer and the reply structs come through these same
 * wrappers - so it is wider than its name suggests. And it is approximate in
 * size, because the reader's buffer is an sds string that grows greedily,
 * doubling below a megabyte and by a megabyte above it, so a reply somewhat
 * under the ceiling can still ask for a step over it. Bounding memory
 * approximately is worth more than not bounding it; claiming it is the exact
 * byte at which a reply stops fitting is not something this can support.
 */
void
vfdw_io_install_alloc_ceiling(void)
{
	static bool installed = false;
	valkeyAllocFuncs fns;

	if (installed)
		return;

	fns.mallocFn = vfdw_alloc_malloc;
	fns.callocFn = vfdw_alloc_calloc;
	fns.reallocFn = vfdw_alloc_realloc;
	fns.strdupFn = vfdw_alloc_strdup;
	fns.freeFn = free;

	(void) valkeySetAllocators(&fns);
	installed = true;
}

/*
 * Lower the ceiling, for the one suite that has to see it refuse.
 *
 * A fixed bound no statement can reach is a bound no statement can prove, and
 * an assertion that cannot fail is the thing this tree refuses to ship. So the
 * setter exists and lives behind valkey_fdw_test, which is superuser-only:
 * the privilege model is what makes a lever safe to leave lying about, not its
 * absence. Zero restores the built-in ceiling.
 */
void
vfdw_io_set_alloc_ceiling_for_test(int64 bytes)
{
	vfdw_io_install_alloc_ceiling();
	vfdw_alloc_ceiling = bytes > 0 ? (size_t) bytes : VFDW_MAX_REPLY_BYTES;
}

void
vfdw_io_set_limits(valkeyContext *conn, size_t maxbuf, long maxelements)
{
	if (conn->reader == NULL)
		return;

	conn->reader->maxbuf = maxbuf;
	conn->reader->maxelements = maxelements;
}

void
vfdw_io_flush(valkeyContext *conn, TimestampTz deadline)
{
	int			done = 0;

	while (!done)
	{
		if (valkeyBufferWrite(conn, &done) != VALKEY_OK)
			vfdw_error_from_context(conn, "could not send command to Valkey");

		if (done)
			break;

		vfdw_io_wait(conn, WL_SOCKET_WRITEABLE, deadline, "sending a command");
	}
}

valkeyReply *
vfdw_io_get_reply(valkeyContext *conn, TimestampTz deadline)
{
	void	   *reply = NULL;

	for (;;)
	{
		/*
		 * A complete reply may already be sitting in the reader from an
		 * earlier read - this is what makes pipelining work, and why the
		 * parse attempt comes before the wait rather than after it.
		 */
		if (valkeyGetReplyFromReader(conn, &reply) != VALKEY_OK)
			vfdw_error_from_context(conn, "could not parse reply from Valkey");

		if (reply != NULL)
			return (valkeyReply *) reply;

		vfdw_io_wait(conn, WL_SOCKET_READABLE, deadline, "waiting for a reply");

		if (valkeyBufferRead(conn) != VALKEY_OK)
			vfdw_error_from_context(conn, "could not read reply from Valkey");
	}
}
