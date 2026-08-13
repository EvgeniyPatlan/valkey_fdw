/*
 * V1 spike: can valkey_fdw drive libvalkey I/O itself, so that every wait is
 * interruptible by PostgreSQL?
 *
 * The FDW's cancellation story depends on never calling a libvalkey function
 * that blocks. That requires four things to hold, none of which are stated in
 * the public headers. This program asserts each against a live server:
 *
 *   1. A non-blocking connect can be completed by waiting for writability and
 *      checking SO_ERROR, without calling into libvalkey's blocking helpers.
 *   2. valkeyBufferWrite drains the output buffer across multiple calls and
 *      reports completion via *done, treating EWOULDBLOCK as benign.
 *   3. valkeyBufferRead + valkeyGetReplyFromReader together yield replies
 *      incrementally, returning VALKEY_OK with no reply when the socket has
 *      nothing to give.
 *   4. Pipelining works on that footing: N appended commands produce N replies
 *      in order.
 *
 * Standalone so it can be run without PostgreSQL. Built and run by
 * scripts/harness.sh spike.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <valkey/valkey.h>

#define PIPELINE_DEPTH 500

static int failures = 0;

static void
ok(int cond, const char *what)
{
	printf("%-6s %s\n", cond ? "ok" : "not ok", what);
	if (!cond)
		failures++;
}

static void
fatal(const char *what, valkeyContext *c)
{
	printf("not ok %s: %s\n", what, c ? c->errstr : "(no context)");
	exit(1);
}

/*
 * Wait for the socket, the way the FDW will: a bounded wait the caller can
 * abandon. In the extension this is WaitLatchOrSocket over the connection
 * socket plus MyLatch; here poll() stands in for it, since the question under
 * test is libvalkey's behaviour, not PostgreSQL's event loop.
 */
static int
wait_socket(int fd, short events, int timeout_ms)
{
	struct pollfd pfd = {.fd = fd, .events = events};
	int			rc;

	do
	{
		rc = poll(&pfd, 1, timeout_ms);
	} while (rc < 0 && errno == EINTR);

	return rc;
}

/* Drain the output buffer without ever blocking. */
static void
flush_output(valkeyContext *c)
{
	int			done = 0;
	int			rounds = 0;

	while (!done)
	{
		if (valkeyBufferWrite(c, &done) != VALKEY_OK)
			fatal("valkeyBufferWrite", c);
		if (done)
			break;
		if (wait_socket(c->fd, POLLOUT, 5000) <= 0)
			fatal("timed out waiting for writability", c);
		rounds++;
	}
	if (rounds > 0)
		printf("#      output buffer needed %d extra write round(s)\n", rounds);
}

/* Read exactly one reply, waiting only through our own poll(). */
static valkeyReply *
next_reply(valkeyContext *c)
{
	void	   *reply = NULL;

	for (;;)
	{
		if (valkeyGetReplyFromReader(c, &reply) != VALKEY_OK)
			fatal("valkeyGetReplyFromReader", c);
		if (reply != NULL)
			return (valkeyReply *) reply;

		if (wait_socket(c->fd, POLLIN, 5000) <= 0)
			fatal("timed out waiting for readability", c);

		if (valkeyBufferRead(c) != VALKEY_OK)
			fatal("valkeyBufferRead", c);
	}
}

int
main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "valkey";
	int			port = argc > 2 ? atoi(argv[2]) : 6379;
	struct timeval connect_timeout = {5, 0};
	valkeyOptions opts = {0};
	valkeyContext *c;
	valkeyReply *reply;
	int			soerr = 0;
	socklen_t	soerrlen = sizeof(soerr);
	int			i;

	VALKEY_OPTIONS_SET_TCP(&opts, host, port);
	opts.options |= VALKEY_OPT_NONBLOCK;
	opts.connect_timeout = &connect_timeout;

	c = valkeyConnectWithOptions(&opts);
	if (c == NULL)
	{
		printf("not ok allocation failed\n");
		return 1;
	}
	/*
	 * A non-blocking connect may still report an immediate hard failure, e.g.
	 * an unresolvable host. Anything else is "in progress" and is resolved by
	 * the writability wait below.
	 */
	if (c->err && c->fd == VALKEY_INVALID_FD)
		fatal("valkeyConnectWithOptions", c);

	ok(!(c->flags & VALKEY_BLOCK),
	   "context is non-blocking (EWOULDBLOCK will be benign, not an I/O error)");

	/* 1. Complete the connect ourselves. */
	if (wait_socket(c->fd, POLLOUT, 5000) <= 0)
		fatal("timed out completing connect", c);
	if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &soerrlen) != 0 || soerr != 0)
	{
		printf("not ok connect failed: %s\n", strerror(soerr));
		return 1;
	}
	/*
	 * libvalkey tracks an in-progress connect internally; clear that state by
	 * letting it observe the completed socket before we drive it ourselves.
	 */
	c->err = 0;
	c->errstr[0] = '\0';
	ok(1, "non-blocking connect completed via poll + SO_ERROR");

	/* 2 and 3: a single round trip driven entirely by us. */
	if (valkeyAppendCommand(c, "PING") != VALKEY_OK)
		fatal("valkeyAppendCommand(PING)", c);
	flush_output(c);
	reply = next_reply(c);
	ok(reply != NULL && reply->type == VALKEY_REPLY_STATUS &&
	   strcmp(reply->str, "PONG") == 0,
	   "PING answered PONG with no blocking libvalkey call");
	freeReplyObject(reply);

	/* Binary safety: a value with an embedded NUL must survive intact. */
	{
		const char	payload[] = "abc\0def";
		size_t		payload_len = sizeof(payload) - 1;	/* 7 bytes */

		if (valkeyAppendCommand(c, "SET vfdw:spike:bin %b",
								payload, payload_len) != VALKEY_OK)
			fatal("valkeyAppendCommand(SET %b)", c);
		flush_output(c);
		reply = next_reply(c);
		freeReplyObject(reply);

		if (valkeyAppendCommand(c, "GET vfdw:spike:bin") != VALKEY_OK)
			fatal("valkeyAppendCommand(GET)", c);
		flush_output(c);
		reply = next_reply(c);
		ok(reply != NULL && reply->type == VALKEY_REPLY_STRING &&
		   reply->len == (size_t) payload_len &&
		   memcmp(reply->str, payload, payload_len) == 0,
		   "value with an embedded NUL round-trips byte-for-byte via %b");
		freeReplyObject(reply);
	}

	/* 4. Pipelining: many commands in flight, replies in order. */
	{
		int			mismatches = 0;

		for (i = 0; i < PIPELINE_DEPTH; i++)
		{
			if (valkeyAppendCommand(c, "SET vfdw:spike:%d %d", i, i) != VALKEY_OK)
				fatal("valkeyAppendCommand(SET)", c);
		}
		flush_output(c);
		for (i = 0; i < PIPELINE_DEPTH; i++)
		{
			reply = next_reply(c);
			if (reply->type != VALKEY_REPLY_STATUS)
				mismatches++;
			freeReplyObject(reply);
		}
		ok(mismatches == 0, "500 pipelined SETs acknowledged");

		for (i = 0; i < PIPELINE_DEPTH; i++)
		{
			if (valkeyAppendCommand(c, "GET vfdw:spike:%d", i) != VALKEY_OK)
				fatal("valkeyAppendCommand(GET)", c);
		}
		flush_output(c);
		mismatches = 0;
		for (i = 0; i < PIPELINE_DEPTH; i++)
		{
			char		expected[32];

			snprintf(expected, sizeof(expected), "%d", i);
			reply = next_reply(c);
			if (reply->type != VALKEY_REPLY_STRING ||
				strcmp(reply->str, expected) != 0)
				mismatches++;
			freeReplyObject(reply);
		}
		ok(mismatches == 0, "500 pipelined GETs returned in request order");
	}

	/* RESP3, which gives typed maps and real doubles. */
	if (valkeyAppendCommand(c, "HELLO 3") != VALKEY_OK)
		fatal("valkeyAppendCommand(HELLO 3)", c);
	flush_output(c);
	reply = next_reply(c);
	ok(reply != NULL && reply->type == VALKEY_REPLY_MAP,
	   "HELLO 3 negotiated RESP3 (reply is a typed map)");
	freeReplyObject(reply);

	/* Leave nothing behind. */
	if (valkeyAppendCommand(c, "DEL vfdw:spike:bin") == VALKEY_OK)
	{
		flush_output(c);
		freeReplyObject(next_reply(c));
	}

	valkeyFree(c);

	printf("# %s\n", failures == 0 ? "V1 resolved: manual non-blocking I/O is viable"
		   : "V1 NOT resolved");
	return failures == 0 ? 0 : 1;
}
