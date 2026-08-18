/*-------------------------------------------------------------------------
 *
 * vfdw_vec.c
 *		Vectors, between PostgreSQL's text form and Valkey's bytes.
 *
 * valkey-search stores and searches a vector as raw little-endian FLOAT32:
 * four bytes per element, no header, no count. PostgreSQL has no such type
 * of its own, and this wrapper deliberately does not link the extension that
 * does - so the two ends are joined through the only representation both
 * sides can produce without knowing anything about each other's internals:
 * the type's own text form, "[1,0.5,-2]".
 *
 * WHY NOT THE BINARY FORM. pgvector's on-disk vector is a header of two
 * int16s followed by float4s in NETWORK byte order, and its send function
 * emits the same. Reading that here would mean copying a struct layout out of
 * another project and being silently wrong the day it changes - and being
 * wrong here does not fail, it searches the wrong point in space. The text
 * form is a documented, stable interface, and one round trip through it costs
 * a few microseconds once per query.
 *
 * NOTHING IS PARSED BY HAND. Elements go out through float4out and come back
 * in through float4in, so the rounding, the spelling of infinity and the
 * rejection of nonsense are PostgreSQL's rather than this file's. A
 * hand-rolled parser here would be a second float syntax that agreed with the
 * first until it did not.
 *
 * Invariant I3 applies with force: a vector's bytes contain NULs by
 * construction - element 0.0f is four of them - so every length in this file
 * travels with its data and no result is ever measured with strlen.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_vec.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_vec.h"

#include "lib/stringinfo.h"
#include "utils/builtins.h"

/*
 * A vector is float32, so its byte length is a multiple of four.
 *
 * Stated as a constant rather than sizeof(float) because it is a property of
 * valkey-search's wire format, not of this compiler's float. A platform where
 * they differ should fail to build here rather than search a keyspace whose
 * vectors it lays out differently.
 */
#define VFDW_VEC_ELEM_BYTES 4

StaticAssertDecl(sizeof(float) == VFDW_VEC_ELEM_BYTES,
				 "valkey-search vectors are FLOAT32 and this float is not");

/*
 * Read one little-endian float32 out of a buffer.
 *
 * Assembled from bytes rather than cast through a float pointer: the buffer
 * comes from a Valkey reply and carries no alignment guarantee at all, and an
 * unaligned load is undefined behaviour that happens to work on x86 and traps
 * elsewhere. The shifts also make the byte order explicit, so a big-endian
 * build reads the same value this comment claims rather than a byte-swapped
 * one.
 */
static float
vfdw_vec_get_le(const unsigned char *p)
{
	uint32		bits;
	float		f;

	bits = (uint32) p[0]
		| ((uint32) p[1] << 8)
		| ((uint32) p[2] << 16)
		| ((uint32) p[3] << 24);

	memcpy(&f, &bits, sizeof(f));
	return f;
}

/* The inverse, with the same reasoning. */
static void
vfdw_vec_put_le(unsigned char *p, float f)
{
	uint32		bits;

	memcpy(&bits, &f, sizeof(bits));

	p[0] = (unsigned char) (bits & 0xff);
	p[1] = (unsigned char) ((bits >> 8) & 0xff);
	p[2] = (unsigned char) ((bits >> 16) & 0xff);
	p[3] = (unsigned char) ((bits >> 24) & 0xff);
}

/*
 * Valkey's bytes -> the text form a vector type's input function accepts.
 *
 * Returns a palloc'd, NUL-terminated cstring, because that is what an input
 * function takes. The NUL is a terminator here and not data: this side of the
 * conversion is text by definition, and the bytes it was built from are gone.
 *
 * A length that is not a multiple of four is refused rather than truncated.
 * Truncating would build a shorter vector that the column's own dimension
 * check would then usually accept, because the stored dimension and the
 * declared one are not the same fact.
 */
char *
vfdw_vec_to_text(const char *bytes, size_t len, const char *what)
{
	StringInfoData buf;
	size_t		i;

	/*
	 * Empty is refused rather than rendered as "[]", so that both directions
	 * agree about the zero-dimensional vector: from_text refuses "[]" because
	 * no valkey-search index has that dimension, and a field holding no bytes
	 * is the same absence spelled the other way.
	 */
	if (len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("%s holds no bytes, so it is not a vector", what),
				 errdetail("A valkey-search index has a fixed dimension and "
						   "no zero-dimensional form.")));

	if (len % VFDW_VEC_ELEM_BYTES != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("%s is %zu bytes, which is not a whole number of "
						"float32 elements", what, len),
				 errdetail("A valkey-search vector is raw little-endian "
						   "FLOAT32, so its length is a multiple of %d.",
						   VFDW_VEC_ELEM_BYTES)));

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');

	for (i = 0; i < len; i += VFDW_VEC_ELEM_BYTES)
	{
		float		f = vfdw_vec_get_le((const unsigned char *) bytes + i);
		char	   *s;

		if (i > 0)
			appendStringInfoChar(&buf, ',');

		/*
		 * float4out, so the digits are the ones PostgreSQL would print for
		 * the same float and the value survives the trip back through
		 * float4in. Writing "%g" here would silently round, and a rounded
		 * query vector points somewhere else.
		 */
		s = DatumGetCString(DirectFunctionCall1(float4out, Float4GetDatum(f)));
		appendStringInfoString(&buf, s);
		pfree(s);
	}

	appendStringInfoChar(&buf, ']');
	return buf.data;
}

/*
 * Refuse a string that is not a vector literal, naming what was seen.
 *
 * One place rather than four, because the four call sites differ only in
 * which part of the shape was wrong and every one of them has to print the
 * whole string: a message that said "invalid vector" without it leaves a user
 * looking at a column whose value they cannot see.
 *
 * why travels through "%s" and never as the format (invariant I2). It is a
 * literal at every call site today, and that is exactly the property a
 * refactor silently breaks.
 */
VFDW_NORETURN static void
vfdw_vec_bad_form(const char *what, const char *text, const char *why)
VFDW_NORETURN_TAIL;

static void
vfdw_vec_bad_form(const char *what, const char *text, const char *why)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_EXCEPTION),
			 errmsg("%s is not in vector form", what),
			 errdetail("%s: \"%s\".", why, text),
			 errhint("The column or expression must be of a type whose text "
					 "form is a vector literal, such as pgvector's "
					 "\"vector\".")));
}

/*
 * One element, from *pp up to the next comma or bracket.
 *
 * Advances *pp to that delimiter and leaves it there: the caller decides what
 * a comma and a bracket each mean, which keeps the loop's termination in one
 * place rather than half here and half there.
 */
static float
vfdw_vec_take_elem(const char **pp, const char *what, const char *text)
{
	const char *start = *pp;
	char	   *elem;
	float		f;

	while (**pp != '\0' && **pp != ',' && **pp != ']')
		(*pp)++;

	if (**pp == '\0')
		vfdw_vec_bad_form(what, text, "The list is not closed");

	/*
	 * float4in decides what a number is. It raises on anything that is not
	 * one, and its message names the offending text - which is more use than
	 * "invalid vector" would be. Leading and trailing spaces are its problem
	 * too, so nothing here trims them.
	 */
	elem = pnstrdup(start, (Size) (*pp - start));
	f = DatumGetFloat4(DirectFunctionCall3(float4in,
										   CStringGetDatum(elem),
										   ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(-1)));
	pfree(elem);
	return f;
}

/*
 * The text form -> Valkey's bytes.
 *
 * text is what the column's output function produced, so this accepts the
 * shape that function emits: an optional run of whitespace, '[', elements
 * separated by commas, ']'. Anything else is refused naming what was seen,
 * because the alternative is to build a vector out of whatever prefix parsed
 * and search with it.
 *
 * The empty vector "[]" is refused too. valkey-search has no zero-dimensional
 * index, so it could only ever produce an error from the server, and one
 * raised here names the column.
 */
char *
vfdw_vec_from_text(const char *text, const char *what, size_t *lenp)
{
	const char *p = text;
	StringInfoData buf;

	while (isspace((unsigned char) *p))
		p++;

	if (*p != '[')
		vfdw_vec_bad_form(what, text,
						  "Not a bracketed list of numbers such as "
						  "\"[1,0.5,-2]\"");
	p++;

	initStringInfo(&buf);

	for (;;)
	{
		unsigned char raw[VFDW_VEC_ELEM_BYTES];

		while (isspace((unsigned char) *p))
			p++;

		if (*p == ']' && buf.len == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("%s is an empty vector", what),
					 errdetail("A valkey-search index has a fixed dimension "
							   "and no zero-dimensional form.")));

		vfdw_vec_put_le(raw, vfdw_vec_take_elem(&p, what, text));
		appendBinaryStringInfo(&buf, (const char *) raw, VFDW_VEC_ELEM_BYTES);

		if (*p == ']')
			break;
		p++;					/* the comma */
	}

	p++;						/* the ']' */
	while (isspace((unsigned char) *p))
		p++;

	if (*p != '\0')
		vfdw_vec_bad_form(what, text, "There is text after the closing bracket");

	*lenp = (size_t) buf.len;
	return buf.data;
}
