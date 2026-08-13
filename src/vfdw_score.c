/*-------------------------------------------------------------------------
 *
 * vfdw_score.c
 *		What valkey_fdw will accept as a zset score.
 *
 * Split from src/vfdw_val.c because it answers a different question. That
 * file asks what a Datum's bytes are; this one asks whether a particular byte
 * string is a number Valkey will store, and the answer is deliberately not
 * "whatever the server would take".
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_score.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_val.h"

#include <math.h>

#include "lib/stringinfo.h"

/*
 * How far a valid score extends from the start of the string, in bytes.
 *
 * Grammar, deliberately a PROPER SUBSET of what every supported server
 * accepts:
 *
 *		score := [+-]? ( DIGIT+ ( '.' DIGIT* )? | '.' DIGIT+ )
 *				 ( [eE] [+-]? DIGIT+ )?
 *
 * valkey_fdw does not reproduce Valkey's parser. The accepted set has already
 * changed once between server minor versions - libc strtod gave way to
 * fast_float, which silently dropped hex float syntax - so a validator that
 * tracked it would have to be version-conditional and would still be wrong on
 * the next change. A strict subset is correct against every version and fails
 * only in the safe direction: it refuses things the server would have taken,
 * never accepts something the server will not.
 *
 * Walks (bytes, len) rather than a C string, so a NUL is an ordinary byte that
 * fails the grammar rather than an early terminator that hides the rest.
 */
static size_t
vfdw_val_score_scan(const char *s, size_t len)
{
	size_t		i = 0;
	size_t		digits = 0;
	size_t		end;

	if (i < len && (s[i] == '+' || s[i] == '-'))
		i++;

	while (i < len && s[i] >= '0' && s[i] <= '9')
	{
		i++;
		digits++;
	}

	if (i < len && s[i] == '.')
	{
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9')
		{
			i++;
			digits++;
		}
	}

	if (digits == 0)
		return 0;

	/* A valid score ends here unless a COMPLETE exponent follows. */
	end = i;

	if (i < len && (s[i] == 'e' || s[i] == 'E'))
	{
		size_t		edigits = 0;

		i++;
		if (i < len && (s[i] == '+' || s[i] == '-'))
			i++;
		while (i < len && s[i] >= '0' && s[i] <= '9')
		{
			i++;
			edigits++;
		}
		if (edigits > 0)
			end = i;
	}

	return end;
}

/*
 * Is this one of the spellings of +-Infinity?
 *
 * Called only after the grammar has already refused the bytes, so this never
 * widens what is accepted - it only decides which refusal is reported.
 * Infinity is worth its own verdict because it is the one value BOTH ends
 * produce and only one end accepts: Valkey stores +inf natively and the read
 * path hands it back as 'Infinity' through float8 or 'inf' through text, so
 * "INSERT INTO z SELECT * FROM z" meets this refusal on a row this wrapper
 * itself just returned. Reporting that as "not a number" would send the
 * reader looking for a parse bug instead of at the policy in this file.
 *
 * Both ends' spellings are covered: float8out writes "Infinity", Valkey
 * writes "inf", and the server reads either in any case, so the comparison is
 * case-insensitive over ASCII only - a locale-aware tolower would make the
 * verdict depend on LC_CTYPE.
 */
static bool
vfdw_val_score_is_infinite(const char *data, size_t len)
{
	static const char inf[] = "infinity";
	size_t		i = 0;
	size_t		j;

	if (i < len && (data[i] == '+' || data[i] == '-'))
		i++;

	/* "inf" is the shortest spelling either end produces, "infinity" the longest. */
	if (len - i != 3 && len - i != 8)
		return false;

	for (j = 0; i + j < len; j++)
	{
		char		c = data[i + j];

		if (c >= 'A' && c <= 'Z')
			c = (char) (c - 'A' + 'a');
		if (c != inf[j])
			return false;
	}

	return true;
}

/*
 * Classify the RENDERED bytes, never the score column's PostgreSQL type.
 *
 * The column may be float8, float4, numeric or text. An isnan()/isinf() test
 * on a float8 Datum covers one of those and leaves 'NaN'::numeric and
 * 'inf'::text for the server to reject - in phase 2 of the flush, after phase
 * 1 passed and after earlier actions have already run. That is the partially
 * applied outcome: a unit half in Valkey, reported XX000 with nothing able to
 * undo it, and the one outcome the whole deferred design exists to make
 * unreachable. One validator over the rendered bytes covers every type by
 * construction.
 */
VfdwScoreVerdict
vfdw_val_score_classify(const char *data, size_t len)
{
	char	   *copy;
	char	   *endptr;
	double		val;
	size_t		used;

	if (len == 0)
		return VFDW_SCORE_EMPTY;

	used = vfdw_val_score_scan(data, len);
	if (used == 0)
		return vfdw_val_score_is_infinite(data, len)
			? VFDW_SCORE_NOT_FINITE : VFDW_SCORE_NOT_A_NUMBER;
	if (used < len)
		return VFDW_SCORE_TRAILING;

	/*
	 * The grammar has already excluded every byte strtod could stop on,
	 * including NUL, so terminating a copy and handing it to a C API is safe
	 * here and only here.
	 */
	copy = pnstrdup(data, len);
	errno = 0;
	val = strtod(copy, &endptr);

	if (endptr != copy + len)
		return VFDW_SCORE_TRAILING; /* not reachable; loud, not silent */

	/*
	 * ERANGE alone is not a rejection: glibc reports it for subnormal results
	 * too, and '1e-320' is a perfectly good score that Valkey accepts. Valkey
	 * rejects ERANGE only when the result is +-HUGE_VAL or exactly zero, so
	 * this pairs the errno with the value the way string2d does.
	 */
	if (errno == ERANGE && (val == HUGE_VAL || val == -HUGE_VAL))
		return VFDW_SCORE_OVERFLOW;
	if (errno == ERANGE && val == 0.0)
		return VFDW_SCORE_UNDERFLOW;

	/*
	 * Unreachable via the grammar, which admits no inf or nan spelling - the
	 * infinity spellings are classified above, before strtod ever runs. Kept
	 * as a second catch: a future relaxation of the grammar that let one
	 * through would otherwise reach ZADD as VFDW_SCORE_OK.
	 */
	if (!isfinite(val))
		return VFDW_SCORE_NOT_FINITE;

	return VFDW_SCORE_OK;
}

const char *
vfdw_val_score_verdict_name(VfdwScoreVerdict verdict)
{
	switch (verdict)
	{
		case VFDW_SCORE_OK:
			return "ok";
		case VFDW_SCORE_EMPTY:
			return "empty";
		case VFDW_SCORE_NOT_A_NUMBER:
			return "not_a_number";
		case VFDW_SCORE_TRAILING:
			return "trailing";
		case VFDW_SCORE_OVERFLOW:
			return "overflow";
		case VFDW_SCORE_UNDERFLOW:
			return "underflow";
		case VFDW_SCORE_NOT_FINITE:
			return "not_finite";
	}
	return "unknown";
}

static const char *
vfdw_val_score_reason(VfdwScoreVerdict verdict)
{
	switch (verdict)
	{
		case VFDW_SCORE_OK:
			return "which is a finite number";
		case VFDW_SCORE_EMPTY:
			return "which is empty";
		case VFDW_SCORE_NOT_A_NUMBER:
			return "which is not a number";
		case VFDW_SCORE_TRAILING:
			return "which has characters after the number";
		case VFDW_SCORE_OVERFLOW:
			return "which is out of range for a double";
		case VFDW_SCORE_UNDERFLOW:
			return "which underflows to zero";
		case VFDW_SCORE_NOT_FINITE:

			/*
			 * Not "which is not a number": it is one. What disqualifies it is
			 * that valkey_fdw does not store infinite scores, which the
			 * errhint says in full.
			 */
			return "which is infinite";
	}
	return "which is not a valid score";
}

/*
 * A score column can hold arbitrary bytes, and this error fires before the
 * value has been proved to be a number, so the offending text is echoed only
 * when it is short and entirely printable ASCII (invariants I2 and I3).
 */
static bool
vfdw_val_printable(const char *s, size_t len)
{
	size_t		i;

	if (len > 64)
		return false;

	for (i = 0; i < len; i++)
		if ((unsigned char) s[i] < 0x20 || (unsigned char) s[i] > 0x7e)
			return false;

	return true;
}

/*
 * Refusing here rather than letting ZADD answer is not an optimisation.
 *
 * (a) Atomicity. The score is an ARGV element consumed in phase 2 of the
 * flush script, and no phase-1 opcode can express "this ARGV parses as a
 * finite double" - the phase-1 verb set is EXISTS / TYPE / HEXISTS /
 * SISMEMBER / ZSCORE / LPOS / SCARD / ZCARD / LLEN. A bad score therefore
 * fails after phase 1 passed and after earlier actions have run, which is a
 * partially applied unit.
 *
 * (b) Attribution. A DML-time refusal names the row, the column and the
 * statement. A pre-commit refusal names none of them, because by COMMIT the
 * row is one of thousands in the buffer and es_processed has already been
 * reported.
 *
 * The message must not claim Valkey refuses +-Infinity, because it does not:
 * its parser reads "infinity" case-insensitively and ZADD documents +inf and
 * -inf as valid. Refusing them is valkey_fdw policy - the text does not
 * survive the round trip ('Infinity' out, 'inf' back) and ZINCRBY on an
 * infinite score produces NaN, which then cannot be stored - and blaming the
 * server would send the user to the wrong documentation. NaN is different and
 * non-negotiable: the server itself rejects it.
 *
 * (c) The asymmetry this policy creates is deliberate and one-directional,
 * and it is the only place in the wrapper where the read path accepts
 * something the write path refuses. A zset whose scores were set to +inf by
 * another client reads back without complaint; feeding those rows back in is
 * refused. That is recorded in README.md under what does not round-trip, and
 * pinned by the S-19/S-17 vectors in test/regress/sql/val.sql, so it is a
 * decision rather than an accident. The alternative - refusing on the read
 * side too - would make a table unreadable because of a value some other
 * client wrote, which is worse.
 */
void
vfdw_val_check_score(const char *data, size_t len, const char *relname,
					 const char *colname)
{
	VfdwScoreVerdict verdict = vfdw_val_score_classify(data, len);
	StringInfoData detail;

	if (verdict == VFDW_SCORE_OK)
		return;

	initStringInfo(&detail);
	appendStringInfo(&detail, "Column \"%s\" of relation \"%s\" produced ",
					 colname, relname);

	if (vfdw_val_printable(data, len))
		appendStringInfo(&detail, "the text \"%s\", ", pnstrdup(data, len));
	else
		appendStringInfo(&detail, "a " UINT64_FORMAT "-byte value, ",
						 (uint64) len);

	appendStringInfoString(&detail, vfdw_val_score_reason(verdict));

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("zset score must be a finite number"),
			 errdetail("%s.", detail.data),
			 errhint("valkey_fdw does not store infinite scores, even though "
					 "Valkey itself accepts +inf and -inf.")));
}
