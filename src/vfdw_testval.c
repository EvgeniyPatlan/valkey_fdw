/*-------------------------------------------------------------------------
 *
 * vfdw_testval.c
 *		SQL entry points that exercise the value layer directly.
 *
 * Split from src/vfdw_testfuncs.c, which reaches the connection and I/O
 * layers: these answer a different question and the two together exceed the
 * file-length gate.
 *
 * Every one of them takes and returns bytea rather than text, because the
 * properties worth asserting here are byte-exact: an embedded NUL, a value
 * that is not valid in the server encoding, and the difference between an
 * empty value and a NULL one all disappear the moment a probe hands its
 * result through a text type.
 *
 * The rule these obey, and the reason the file exists in this shape: a probe
 * calls the SAME entry point a statement calls, never the layer below it. A
 * probe that reached vfdw_val_render directly would leave the kind dispatch,
 * the per-role NULL rules, the score routing and the scratch/retain split
 * with no caller at all - which is exactly the state this file was written to
 * end. vfdw_test_val_setup therefore builds its VfdwValCtx with the
 * production vfdw_val_ctx_init rather than filling the struct by hand.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_testval.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "vfdw_row.h"
#include "vfdw_val.h"

/*
 * One column of a given type, wired to both I/O directions.
 *
 * The type is described by vfdw_map_resolve_type, the same call a real table
 * makes, so a probe can never disagree with a foreign table about whether a
 * type is binary. The one-column VfdwTableMap is what lets vfdw_val_ctx_init
 * run here unchanged.
 */
typedef struct VfdwTestVal
{
	VfdwColumn	col;
	VfdwTableMap map;
	FmgrInfo	infunc;
	VfdwRowCtx	rowctx;
	VfdwValCtx	vc;
} VfdwTestVal;

static void
vfdw_test_val_setup(VfdwTestVal *tv, Oid typid, VfdwColKind kind)
{
	memset(tv, 0, sizeof(*tv));

	tv->col.attnum = 1;
	tv->col.kind = kind;
	vfdw_map_resolve_type(&tv->col, typid, -1);

	/*
	 * THE ONE RULE THIS PROBE CANNOT SKIP.
	 *
	 * A probe builds its column directly and so bypasses the map, which is the
	 * point of it - a real table cannot be talked into most of the shapes worth
	 * asserting about. But the write conversion for a ttl column reads its
	 * Datum as an Interval, and the reason it may is that vfdw_map_build
	 * refuses a ttl column of any other type. Bypassing that here would not
	 * exercise the code a real statement drives; it would hand a text varlena
	 * to a struct pointer and read whatever follows it.
	 *
	 * So the probe applies the map's rule rather than trusting it, and the
	 * refusal is what the val suite asserts for the combination.
	 */
	if (kind == VFDW_COL_TTL && getBaseType(typid) != INTERVALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ttl columns must be of type interval"),
				 errdetail("A real table is refused this at CREATE; the probe "
						   "refuses it because the write conversion reads the "
						   "Datum as an Interval.")));

	fmgr_info(tv->col.typinput, &tv->infunc);
	tv->rowctx.infuncs = &tv->infunc;

	tv->map.natts = 1;
	tv->map.cols = &tv->col;
	tv->map.keyattno = (kind == VFDW_COL_KEY) ? 1 : InvalidAttrNumber;
	tv->rowctx.map = &tv->map;

	/*
	 * rel stays NULL: a probe has no relation, so errtablecol is absent and
	 * the messages name a placeholder column. Message text with real column
	 * identity is what the dml suite asserts; what these prove is which
	 * SQLSTATE fires and on which bytes.
	 *
	 * dest is the caller's context and scratch is a child of it, so a reset of
	 * the scratch cannot reach anything already retained. A probe that made
	 * the two the same context would agree with a broken vfdw_val_retain.
	 */
	vfdw_val_ctx_init(&tv->vc, NULL, &tv->map, CurrentMemoryContext,
					  CurrentMemoryContext);
}

static bytea *
vfdw_test_bytea(const char *data, size_t len)
{
	bytea	   *b = (bytea *) palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	if (len > 0)
		memcpy(VARDATA(b), data, len);

	return b;
}

/*
 * The column role a probe asks for, by name.
 *
 * Spelled out rather than taken as an integer so a vector table reads as the
 * roles a real table declares, and so an unknown name is a refusal rather
 * than an out-of-range kind that would reach the dispatch's default arm and
 * look like the thing under test.
 */
static VfdwColKind
vfdw_test_kind(const char *name)
{
	if (strcmp(name, "key") == 0)
		return VFDW_COL_KEY;
	if (strcmp(name, "value") == 0)
		return VFDW_COL_VALUE;
	if (strcmp(name, "field") == 0)
		return VFDW_COL_FIELD;
	if (strcmp(name, "member") == 0)
		return VFDW_COL_MEMBER;
	if (strcmp(name, "score") == 0)
		return VFDW_COL_SCORE;
	if (strcmp(name, "ttl") == 0)
		return VFDW_COL_TTL;
	if (strcmp(name, "distance") == 0)
		return VFDW_COL_DISTANCE;
	if (strcmp(name, "legacy_value") == 0)
		return VFDW_COL_LEGACY_VALUE;
	if (strcmp(name, "dropped") == 0)
		return VFDW_COL_DROPPED;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown column kind"),
			 errhint("One of key, value, field, member, score, ttl, distance, "
					 "legacy_value or dropped.")));
	return VFDW_COL_DROPPED;	/* keep the compiler quiet */
}

/*
 * valkey_fdw_test_val_out(anyelement) -> bytea
 *
 * The write direction alone: whatever bytes this Datum would put on the wire.
 * Polymorphic and not STRICT, so it can be handed a value straight out of a
 * heap column - which is the only way to present a compressed or out-of-line
 * bytea, and therefore the only way to prove the detoast happens.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_val_out);
Datum
valkey_fdw_test_val_out(PG_FUNCTION_ARGS)
{
	Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	VfdwTestVal tv;
	VfdwValue	val;
	bool		isnull = PG_ARGISNULL(0);

	if (!OidIsValid(typid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine the argument's type"),
				 errhint("Cast the argument, for example NULL::bytea.")));

	vfdw_test_val_setup(&tv, typid, VFDW_COL_VALUE);
	vfdw_val_render(&tv.vc, &tv.col, isnull ? (Datum) 0 : PG_GETARG_DATUM(0),
					isnull, &val);

	/*
	 * The isnull flag decides, never the pointer. An empty value returns a
	 * zero-length bytea here; only a NULL input returns NULL.
	 */
	if (val.isnull)
		PG_RETURN_NULL();

	PG_RETURN_BYTEA_P(vfdw_test_bytea(val.data, val.len));
}

/*
 * valkey_fdw_test_val_roundtrip(payload bytea, typ regtype) -> bytea
 *
 * Bytes in through the read direction, back out through the write direction,
 * using the code the scan and the write path actually use. For text and bytea
 * the result must equal the input byte for byte; for every other type it is
 * out(in(bytes)), which is the property a round trip through Valkey has.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_val_roundtrip);
Datum
valkey_fdw_test_val_roundtrip(PG_FUNCTION_ARGS)
{
	VfdwTestVal tv;
	VfdwValue	val;
	bytea	   *payload;
	Datum		d;

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a target type is required")));

	/* A NULL payload is SQL NULL, which no byte sequence can represent. */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	vfdw_test_val_setup(&tv, PG_GETARG_OID(1), VFDW_COL_VALUE);

	payload = PG_GETARG_BYTEA_PP(0);
	d = vfdw_row_datum_from_bytes(&tv.rowctx, &tv.col, VARDATA_ANY(payload),
								  VARSIZE_ANY_EXHDR(payload));

	vfdw_val_render(&tv.vc, &tv.col, d, false, &val);

	PG_RETURN_BYTEA_P(vfdw_test_bytea(val.data, val.len));
}

/*
 * Fill in the three table-level key options from arguments 3, 4 and 5 of the
 * key-shaped probes, which share a signature.
 */
static void
vfdw_test_key_options(VfdwTestVal *tv, FunctionCallInfo fcinfo)
{
	if (!PG_ARGISNULL(1))
		tv->map.keyprefix = text_to_cstring(PG_GETARG_TEXT_PP(1));
	if (!PG_ARGISNULL(2))
		tv->map.keyset = text_to_cstring(PG_GETARG_TEXT_PP(2));
	if (!PG_ARGISNULL(3))
		tv->map.singleton_key = text_to_cstring(PG_GETARG_TEXT_PP(3));
}

/*
 * valkey_fdw_test_build_key(key bytea, keyprefix text, keyset text,
 *                           singleton_key text, has_key_column boolean) -> bytea
 *
 * The key builder against a table description supplied per call rather than
 * per CREATE FOREIGN TABLE, so one query can walk the whole accept/reject
 * table. has_key_column false is the singleton table that declares no key
 * column at all - the shape whose keyattno is InvalidAttrNumber and which a
 * builder that indexed cols[keyattno - 1] unguarded would read as cols[-1].
 *
 * With a key column this goes through vfdw_val_from_slot, which is the entry
 * point a statement uses and which routes VFDW_COL_KEY to the builder. Only
 * the keyless shape calls the builder directly, and that is not a shortcut:
 * such a table has no slot column for the key, so there is no slot value for
 * from_slot to dispatch on and the write path will call the builder there
 * too.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_build_key);
Datum
valkey_fdw_test_build_key(PG_FUNCTION_ARGS)
{
	VfdwTestVal tv;
	VfdwValue	key;
	bool		isnull = PG_ARGISNULL(0);
	bool		has_col = PG_ARGISNULL(4) || PG_GETARG_BOOL(4);
	Datum		d = isnull ? (Datum) 0 : PG_GETARG_DATUM(0);

	vfdw_test_val_setup(&tv, BYTEAOID, VFDW_COL_KEY);
	vfdw_test_key_options(&tv, fcinfo);

	if (!has_col)
		tv.map.keyattno = InvalidAttrNumber;

	if (has_col)
		vfdw_val_from_slot(&tv.vc, &tv.map, &tv.col, d, isnull, &key);
	else
		vfdw_val_build_key(&tv.vc, &tv.map, NULL, d, isnull, &key);

	PG_RETURN_BYTEA_P(vfdw_test_bytea(key.data, key.len));
}

/*
 * valkey_fdw_test_key_why(key bytea, keyprefix text, keyset text,
 *                         singleton_key text, has_key_column boolean) -> text
 *
 * Which key rule fired, without raising. Three of the five refusals share
 * SQLSTATE 23514, so a vector table comparing only the SQLSTATE stays
 * byte-identical if the builder rejects a key through the wrong branch - or
 * if all three branches collapse into one. This is the key-side counterpart
 * of valkey_fdw_test_score's do_raise = false mode, and it reads the same
 * classifier the raising path reads.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_key_why);
Datum
valkey_fdw_test_key_why(PG_FUNCTION_ARGS)
{
	VfdwTestVal tv;
	VfdwValue	key;
	VfdwKeyVerdict verdict;
	bool		isnull = PG_ARGISNULL(0);
	bool		has_col = PG_ARGISNULL(4) || PG_GETARG_BOOL(4);
	Datum		d = isnull ? (Datum) 0 : PG_GETARG_DATUM(0);

	vfdw_test_val_setup(&tv, BYTEAOID, VFDW_COL_KEY);
	vfdw_test_key_options(&tv, fcinfo);

	if (!has_col)
		tv.map.keyattno = InvalidAttrNumber;

	verdict = vfdw_val_key_classify(&tv.vc, &tv.map,
									has_col ? &tv.col : NULL, d, isnull, &key);

	PG_RETURN_TEXT_P(cstring_to_text(vfdw_val_key_verdict_name(verdict)));
}

/*
 * valkey_fdw_test_val_slot(payload bytea, typ regtype, kind text,
 *                          keyprefix text, keyset text, singleton_key text)
 *   -> bytea
 *
 * One column through vfdw_val_from_slot, which is the entry point a statement
 * calls and the only place the per-role NULL rules, the score routing and the
 * refusal of an unimplemented kind live. A NULL payload is a NULL slot value,
 * which is what makes the NULL rules reachable; anything else is turned into
 * a Datum of the named type through the READ path, so the value under test is
 * one a scan could have produced.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_val_slot);
Datum
valkey_fdw_test_val_slot(PG_FUNCTION_ARGS)
{
	VfdwTestVal tv;
	VfdwValue	val;
	VfdwColKind kind;
	bool		isnull = PG_ARGISNULL(0);
	Datum		d = (Datum) 0;

	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a target type and a column kind are required")));

	kind = vfdw_test_kind(text_to_cstring(PG_GETARG_TEXT_PP(2)));
	vfdw_test_val_setup(&tv, PG_GETARG_OID(1), kind);

	if (!PG_ARGISNULL(3))
		tv.map.keyprefix = text_to_cstring(PG_GETARG_TEXT_PP(3));
	if (!PG_ARGISNULL(4))
		tv.map.keyset = text_to_cstring(PG_GETARG_TEXT_PP(4));
	if (!PG_ARGISNULL(5))
		tv.map.singleton_key = text_to_cstring(PG_GETARG_TEXT_PP(5));

	if (!isnull)
	{
		bytea	   *payload = PG_GETARG_BYTEA_PP(0);

		d = vfdw_row_datum_from_bytes(&tv.rowctx, &tv.col,
									  VARDATA_ANY(payload),
									  VARSIZE_ANY_EXHDR(payload));
	}

	vfdw_val_from_slot(&tv.vc, &tv.map, &tv.col, d, isnull, &val);

	if (val.isnull)
		PG_RETURN_NULL();

	PG_RETURN_BYTEA_P(vfdw_test_bytea(val.data, val.len));
}

/*
 * valkey_fdw_test_val_retain(a bytea, b bytea) -> bytea
 *
 * Render A, reset the per-row scratch, render B, and hand back A's retained
 * bytes. This is the one vector that can tell a real copy from a borrowed
 * pointer: vfdw_val_ctx_reset runs once per row in production, so a
 * vfdw_val_retain that returned its argument - or that copied into the
 * scratch rather than into the write buffer - leaves every value of every row
 * but the last pointing into memory the next row has already reused. Both
 * mistakes read back as B here; a correct retain reads back as A.
 *
 * text rather than bytea deliberately: the bytea branch takes VARDATA_ANY of
 * a datum that is already flat, so its bytes never live in the scratch at all
 * and the aliasing would be invisible. The output-function branch allocates
 * in the scratch, which is the case that matters.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_val_retain);
Datum
valkey_fdw_test_val_retain(PG_FUNCTION_ARGS)
{
	VfdwTestVal tv;
	VfdwValue	first;
	VfdwValue	second;
	bytea	   *a = PG_GETARG_BYTEA_PP(0);
	bytea	   *b = PG_GETARG_BYTEA_PP(1);

	vfdw_test_val_setup(&tv, TEXTOID, VFDW_COL_VALUE);

	vfdw_val_from_slot(&tv.vc, &tv.map, &tv.col,
					   vfdw_row_datum_from_bytes(&tv.rowctx, &tv.col,
												 VARDATA_ANY(a),
												 VARSIZE_ANY_EXHDR(a)),
					   false, &first);

	vfdw_val_ctx_reset(&tv.vc);

	vfdw_val_from_slot(&tv.vc, &tv.map, &tv.col,
					   vfdw_row_datum_from_bytes(&tv.rowctx, &tv.col,
												 VARDATA_ANY(b),
												 VARSIZE_ANY_EXHDR(b)),
					   false, &second);

	PG_RETURN_BYTEA_P(vfdw_test_bytea(first.data, first.len));
}

/*
 * valkey_fdw_test_all_fields_null() -> void
 *
 * The refusal a hash row whose every mapped column is null would meet. It has
 * no other caller until the statement builder lands, and a rule with no
 * caller and no test is a rule that says whatever the last edit left behind.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_all_fields_null);
Datum
valkey_fdw_test_all_fields_null(PG_FUNCTION_ARGS)
{
	vfdw_val_error_all_fields_null(NULL);
	PG_RETURN_VOID();
}

/*
 * valkey_fdw_test_score(score bytea, do_raise boolean) -> text
 *
 * The verdict name, or - with do_raise - the error a zset write would raise.
 * Two modes because one query listing every vector and its verdict is what
 * makes the grammar reviewable, while the SQLSTATE is what an application
 * branches on.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_score);
Datum
valkey_fdw_test_score(PG_FUNCTION_ARGS)
{
	bytea	   *score;
	const char *data;
	size_t		len;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a score is required"),
				 errhint("A null score column raises not_null_violation, "
						 "which is a different refusal from a malformed one.")));

	score = PG_GETARG_BYTEA_PP(0);
	data = VARDATA_ANY(score);
	len = VARSIZE_ANY_EXHDR(score);

	if (!PG_ARGISNULL(1) && PG_GETARG_BOOL(1))
		vfdw_val_check_score(data, len, "(diagnostic)", "(diagnostic)");

	PG_RETURN_TEXT_P(cstring_to_text(
									 vfdw_val_score_verdict_name(vfdw_val_score_classify(data, len))));
}

/*
 * valkey_fdw_test_crc16(bytea) -> int
 *
 * The raw CRC before the slot mask. crc16('123456789') = 12739 (0x31C3) is
 * the published XMODEM check value and pins the polynomial, the zero init,
 * the absence of reflection and the zero final xor in one number.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_crc16);
Datum
valkey_fdw_test_crc16(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);

	PG_RETURN_INT32((int32) vfdw_val_crc16(VARDATA_ANY(in),
										   VARSIZE_ANY_EXHDR(in)));
}

/*
 * valkey_fdw_test_keyslot(bytea) -> int
 *
 * Compared against the server's own CLUSTER KEYSLOT rather than a transcribed
 * table, so the vectors cannot be copied down wrongly.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_keyslot);
Datum
valkey_fdw_test_keyslot(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);

	PG_RETURN_INT32((int32) vfdw_val_slot(VARDATA_ANY(in),
										  VARSIZE_ANY_EXHDR(in)));
}

/*
 * valkey_fdw_test_hashtag(bytea) -> bytea
 *
 * The exact bytes the slot is computed over. Separating this from the CRC is
 * what lets a failing brace vector localise to one half or the other.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_hashtag);
Datum
valkey_fdw_test_hashtag(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	const char *tag;
	size_t		taglen;

	vfdw_val_hashtag(VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in), &tag, &taglen);

	PG_RETURN_BYTEA_P(vfdw_test_bytea(tag, taglen));
}
