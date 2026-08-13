/*-------------------------------------------------------------------------
 *
 * banned.c
 *		One instance of every construct scripts/lint.sh refuses to accept.
 *
 * This is the lint gate's own test input, not code. `scripts/lint.sh
 * --selftest` runs the banned-construct patterns over this directory and
 * requires each of them to match here at least once, because the only
 * evidence that a pattern still works is a match: a pattern that has stopped
 * matching declares the whole tree clean and looks exactly like a pattern
 * that is doing its job.
 *
 * Exactly one instance of each, so that a pattern which has started matching
 * two things - or the wrong thing - shows up in the self-test counts instead
 * of being averaged away. That is also why the prose below never writes a
 * banned name with its parenthesis attached; a comment mentioning the
 * construct is a second instance of it as far as grep is concerned.
 *
 * It is never compiled and never linked. The Makefile's SRCS is
 * $(wildcard src/*.c), so nothing under test/ reaches the compiler, and
 * nothing here is called from anywhere. Do not repair it into something that
 * builds: every line exists in order to be matched, and correcting the code
 * would mean deleting the constructs being matched.
 *
 * IDENTIFICATION
 *		test/lint/fixtures/banned.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/hsearch.h"

/*
 * The conversion cannot report failure: 'abc' and '0' are the same answer,
 * which is how a mistyped port becomes the default one with no error.
 */
static int
lint_fixture_port(const char *value)
{
	return atoi(value);
}

/*
 * Both of these write until the source runs out rather than until the
 * destination is full, and every length in this wrapper comes off a reply.
 */
static void
lint_fixture_format(char *dst, const char *field, const char *suffix)
{
	sprintf(dst, "key:%s", field);
	strcat(dst, suffix);
}

/*
 * A reply carrying a '%' becomes a format directive here, reading arguments
 * that were never pushed. Invariant I2 is why the pattern covers the DETAIL
 * as well as the MESSAGE: server bytes are server bytes in either.
 */
static void
lint_fixture_report(StringInfo buf)
{
	ereport(ERROR,
			(errcode(ERRCODE_FDW_ERROR),
			 errmsg("valkey command failed"),
			 errdetail(buf->data)));
}

/*
 * The caller gets back an entry it cannot classify, and dynahash has written
 * nothing into it but the key - so every other field is whatever the freelist
 * chunk last held, and a caller that decides "already initialised" from one of
 * those fields is reading the residue to ask about the residue.
 *
 * Four spellings, because the check has to survive all four and a
 * line-at-a-time pattern survives only the first. A call whose arguments wrap
 * is not unusual C and is not a way out of this rule.
 */
static void *
lint_fixture_enter_one_line(HTAB *tab, const void *key)
{
	return hash_search(tab, key, HASH_ENTER, NULL);
}

static void *
lint_fixture_enter_wrapped_before_action(HTAB *tab, const void *key)
{
	return hash_search(tab, key,
					   HASH_ENTER, NULL);
}

static void *
lint_fixture_enter_wrapped_before_arg(HTAB *tab, const void *key)
{
	return hash_search(tab, key, HASH_ENTER,
					   NULL);
}

static void *
lint_fixture_enter_cast(HTAB *tab, const void *key)
{
	return hash_search(tab, key, HASH_ENTER, (bool *) NULL);
}
