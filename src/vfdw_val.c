/*-------------------------------------------------------------------------
 *
 * vfdw_val.c
 *		Datum to bytes: the exact inverse of src/vfdw_row.c.
 *
 * The read direction has two branches keyed on VfdwColumn.is_binary - bytea
 * takes Valkey's bytes verbatim, everything else goes through the type's
 * input function. This file is that mirror and has the same two branches on
 * the same flag: bytea emits the datum's bytes verbatim, every other type
 * goes through the type's output function. There is deliberately no per-type
 * table and no supported-type whitelist. A whitelist would have to be
 * maintained, would refuse user-defined types for no reason, and would be a
 * second place that could disagree with the read path about a type's wire
 * form. The out/in function pair is the type's own definition of its text
 * representation; committing to it in one direction and to a table in the
 * other is precisely what breaks a round trip.
 *
 * strlen appears here four times and none of them is an invariant I3
 * violation. I3 forbids strlen on Valkey-originated bytes; these four are
 * ours. An output function returns a cstring, so it is NUL-terminated by the
 * type system's own contract and lives in our memory. map->keyprefix,
 * map->keyset and map->singleton_key are catalog option strings. The
 * consequence of the first is load-bearing rather than incidental: a
 * text-typed write can never carry an interior NUL, so only the bytea branch
 * has to be binary-safe - and that is a fact about PostgreSQL types, not a
 * check we perform.
 *
 * No encoding validation happens in this direction, for any type. Validation
 * is meaningful only where bytes arrive from a system that does not share our
 * encoding, which is inbound, which is exactly where the read path puts it.
 * Outbound, the Datum reached us either through the type's input function or
 * through a server-side computation, so pg_verifymbstr would verify the
 * server against its own encoding: a tautology costing a full scan of every
 * byte written. It would also have to be skipped for bytea, so it would not
 * even be universal, and applying it to bytea would refuse the one type that
 * exists to carry non-text bytes. What follows from that, and is documented
 * rather than prevented: Valkey stores bytes with no encoding label, so two
 * PostgreSQL databases with different server encodings over one keyspace will
 * disagree about the same bytes, and a bytea value carrying non-UTF8 bytes
 * raises when later read through a text-typed column of a UTF8 database. That
 * last is the read path's existing and correct behaviour, not a new defect.
 *
 * Two further properties the write direction owns alone. A datum can arrive
 * out-of-line or compressed - from INSERT ... SELECT, from COPY, from any
 * heap column - so it is detoasted once before either branch; VARDATA_ANY on
 * an undetoasted TOAST pointer yields the 18-byte pointer struct rather than
 * the value. And a bytea of 126 bytes or fewer arrives with a 1-byte varlena
 * header, so VARSIZE - VARHDRSZ is wrong by 3 on every small value, which is
 * why only VARDATA_ANY/VARSIZE_ANY_EXHDR appear here. The read path meets
 * neither case because it constructs its bytea itself.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_val.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_val.h"

/*
 * varatt.h explicitly rather than through utils/builtins.h: VARDATA_ANY and
 * VARSIZE_ANY_EXHDR are the whole reason the bytea branch is correct, so the
 * header that defines them is named here rather than arriving by accident
 * through something that might later stop including it.
 */
#include "varatt.h"

#include "access/tupdesc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* ---------------------------------------------------------------------
 * Names for error messages
 * --------------------------------------------------------------------- */

/*
 * A diagnostic caller builds a VfdwValCtx by hand and has no Relation. The
 * placeholder keeps such a message well formed rather than making the probe a
 * second code path with messages of its own that nobody reads.
 */
static const char *
vfdw_val_relname(const VfdwValCtx *vc)
{
	return vc->rel != NULL ? RelationGetRelationName(vc->rel) : "(diagnostic)";
}

static const char *
vfdw_val_colname(const VfdwValCtx *vc, const VfdwColumn *col)
{
	if (vc->rel == NULL || col == NULL || col->attnum == InvalidAttrNumber)
		return "(diagnostic)";

	return NameStr(TupleDescAttr(RelationGetDescr(vc->rel),
								 col->attnum - 1)->attname);
}

/* ---------------------------------------------------------------------
 * Datum -> bytes
 * --------------------------------------------------------------------- */

static void
vfdw_val_check_arg_len(const VfdwValCtx *vc, const VfdwColumn *col, size_t len)
{
	if (len <= VFDW_MAX_ARG_BYTES)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("value is too large to send to Valkey as one argument"),
			 errdetail("Column \"%s\" of relation \"%s\" rendered to " UINT64_FORMAT
					   " bytes; the limit is " UINT64_FORMAT ".",
					   vfdw_val_colname(vc, col), vfdw_val_relname(vc),
					   (uint64) len, (uint64) VFDW_MAX_ARG_BYTES),
			 errhint("Valkey answers a bulk argument longer than its "
					 "proto-max-bulk-len setting with a protocol error and "
					 "closes the connection, which at commit time could only "
					 "be reported as an unknown outcome. Raising "
					 "write_max_bytes does not lift this limit.")));
}

/*
 * Copy finished bytes out of the per-row scratch and into the write buffer.
 *
 * Deliberately not NUL-terminated. A terminator would make strlen appear to
 * work on a value that may contain NUL, which is the truncation invariant I3
 * exists to prevent; without one, a caller that loses the length has no way
 * to pretend it still has it.
 */
static const char *
vfdw_val_retain(const VfdwValCtx *vc, const char *bytes, size_t len)
{
	char	   *copy = MemoryContextAlloc(vc->dest, len);

	if (len > 0)
		memcpy(copy, bytes, len);

	return copy;
}

/*
 * Render into the scratch context and stop there.
 *
 * Everything that can refuse a value runs on these bytes, before they are
 * retained, so a value refused by its own rule leaves nothing in the write
 * buffer. That matters because the buffer's byte counter is deliberately
 * monotonic - it is not decremented when an operation is dropped - so bytes
 * retained for a value that was then refused would consume write_max_bytes
 * the user never spent. Retention is per column, so a row refused by a later
 * column's rule still leaves the earlier ones behind; see the longer note in
 * src/vfdw_val.h for why that is deliberate.
 */
static void
vfdw_val_render_scratch(VfdwValCtx *vc, const VfdwColumn *col, Datum d,
						const char **bytes, size_t *len)
{
	MemoryContext old;

	Assert(col->attnum != InvalidAttrNumber);
	Assert(col->attnum <= vc->noutfuncs);

	old = MemoryContextSwitchTo(vc->scratch);

	if (col->typisvarlena)
		d = PointerGetDatum(PG_DETOAST_DATUM_PACKED(d));

	if (col->is_binary)
	{
		*bytes = VARDATA_ANY(DatumGetPointer(d));
		*len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
	}
	else
	{
		char	   *rendered = OutputFunctionCall(&vc->outfuncs[col->attnum - 1], d);

		*bytes = rendered;
		*len = strlen(rendered);
	}

	MemoryContextSwitchTo(old);

	vfdw_val_check_arg_len(vc, col, *len);
}

void
vfdw_val_render(VfdwValCtx *vc, const VfdwColumn *col, Datum d, bool isnull,
				VfdwValue *out)
{
	const char *bytes;
	size_t		len;

	out->isnull = isnull;
	out->data = NULL;
	out->len = 0;

	if (isnull)
		return;

	vfdw_val_render_scratch(vc, col, d, &bytes, &len);

	out->isnull = false;
	out->data = vfdw_val_retain(vc, bytes, len);
	out->len = len;
}

/* ---------------------------------------------------------------------
 * NULL, per column role
 * --------------------------------------------------------------------- */

void
vfdw_val_error_null(Relation rel, const VfdwColumn *col)
{
	const char *colname = "(diagnostic)";
	bool		known = (rel != NULL && col != NULL &&
						 col->attnum != InvalidAttrNumber);

	if (known)
		colname = NameStr(TupleDescAttr(RelationGetDescr(rel),
										col->attnum - 1)->attname);

	/*
	 * 23502 rather than something wrapper-specific, with errtablecol attached,
	 * so the error carries the same structured column identity a real NOT NULL
	 * violation does and an application already branching on that SQLSTATE
	 * needs no new case. Each of these four columns is the row's existence or
	 * its identity: Valkey has no null key, no null member, no scoreless zset
	 * member, and a string table's row is the key-to-value pair.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_NOT_NULL_VIOLATION),
			 errmsg("null value in column \"%s\" has no representation in Valkey",
					colname),
			 errdetail("A key, a member, a score and a string table's value "
					   "each define the row; Valkey has no null form of any "
					   "of them."),
			 known ? errtablecol(rel, col->attnum) : 0));
}

void
vfdw_val_error_all_fields_null(Relation rel)
{
	/*
	 * Refused rather than treated as a delete, because whether the key
	 * survives depends on fields this table does not map, and finding out
	 * would need a read before the write - the check-then-write race this
	 * write path is built to avoid. So the wrapper cannot honestly promise
	 * either outcome, and this is the only rule decidable at DML time.
	 * Refusing keeps INSERT from reporting a row it did not create, and keeps
	 * UPDATE from being a disguised DELETE that still claims one row updated
	 * for a row that no longer exists.
	 *
	 * The message says "the columns this table maps", not "the hash has no
	 * fields", and the difference is the point. A table mapping a proper
	 * subset of a key's fields is the normal case for this wrapper, and the
	 * read path legitimately returns a row whose every MAPPED field is null
	 * for a key that exists and holds only unmapped ones. Asserting that such
	 * a hash does not exist would be a claim about bytes nobody looked at.
	 *
	 * Scope: the rule is right for INSERT, where nothing would be created, and
	 * wrong for UPDATE, where clearing every mapped field is a legitimate
	 * request that means HDEL them. vfdw_modify_render_fields applies it per
	 * OPERATION for that reason - it is not a property of the row. Refusing
	 * both shapes looks like the cautious reading and is not: it leaves no way
	 * to express "clear every mapped field" at all. Where a table maps every
	 * field of the key, clearing them all removes the key, which is the same
	 * physical outcome as deleting it - Valkey has no row that exists and is
	 * empty.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_NOT_NULL_VIOLATION),
			 errmsg("every mapped field of the row is null"),
			 errdetail("This operation sets none of the columns this table "
					   "maps, so it would report a row while writing nothing."),
			 errhint("Give at least one mapped field a value, or use DELETE "
					 "to remove the key."),
			 rel != NULL ? errtable(rel) : 0));
}

/*
 * A zset score, validated on its rendered bytes before those bytes are kept.
 */
static void
vfdw_val_score_column(VfdwValCtx *vc, const VfdwColumn *col, Datum d,
					  VfdwValue *out)
{
	const char *bytes;
	size_t		len;

	vfdw_val_render_scratch(vc, col, d, &bytes, &len);
	vfdw_val_check_score(bytes, len, vfdw_val_relname(vc),
						 vfdw_val_colname(vc, col));

	out->isnull = false;
	out->data = vfdw_val_retain(vc, bytes, len);
	out->len = len;
}

void
vfdw_val_from_slot(VfdwValCtx *vc, const VfdwTableMap *map,
				   const VfdwColumn *col, Datum d, bool isnull, VfdwValue *out)
{
	switch (col->kind)
	{
		case VFDW_COL_KEY:
			vfdw_val_build_key(vc, map, col, d, isnull, out);
			return;

		case VFDW_COL_VALUE:
		case VFDW_COL_MEMBER:
			if (isnull)
				vfdw_val_error_null(vc->rel, col);
			vfdw_val_render(vc, col, d, isnull, out);
			return;

		case VFDW_COL_SCORE:
			if (isnull)
				vfdw_val_error_null(vc->rel, col);
			vfdw_val_score_column(vc, col, d, out);
			return;

		case VFDW_COL_FIELD:

			/*
			 * NULL here is meaningful, not an error: on INSERT the field
			 * contributes no HSET argument and is simply never created, and on
			 * UPDATE it becomes an HDEL. This is the exact inverse of the read
			 * direction, where an absent field leaves the column NULL. Writing
			 * it as an empty string, or as the text 'NULL', would make the
			 * round trip return a value the user never wrote.
			 */
			vfdw_val_render(vc, col, d, isnull, out);
			return;

		case VFDW_COL_DROPPED:
		case VFDW_COL_TTL:
		case VFDW_COL_DISTANCE:
		case VFDW_COL_LEGACY_VALUE:

			/*
			 * Unreachable: vfdw_map_check_implemented refuses ttl, distance,
			 * legacy_value and json tables at plan time, and a dropped column
			 * has no slot value to read. Loud rather than a silent default,
			 * because an arm that falls through is how the next kind added to
			 * the enum acquires accidental semantics.
			 */
			break;
	}

	elog(ERROR, "valkey_fdw: column kind %d has no write conversion",
		 (int) col->kind);
}

/* ---------------------------------------------------------------------
 * The key builder
 * --------------------------------------------------------------------- */

const char *
vfdw_val_key_verdict_name(VfdwKeyVerdict verdict)
{
	switch (verdict)
	{
		case VFDW_KEY_OK:
			return "ok";
		case VFDW_KEY_NULL:
			return "null";
		case VFDW_KEY_NO_COLUMN:
			return "no_key_column";
		case VFDW_KEY_PREFIX:
			return "prefix";
		case VFDW_KEY_KEYSET:
			return "keyset_collision";
		case VFDW_KEY_SINGLETON:
			return "singleton_mismatch";
	}
	return "unknown";
}

/*
 * Does the key lie inside the table's keyprefix?
 *
 * memcmp with an explicit length guard, never strncmp. The read path's
 * strncmp is correct where it stands because its input is a NUL-terminated
 * string an output function produced; a write-path key may come from a bytea
 * column, where VARDATA_ANY is not terminated and strncmp would read past the
 * datum. Same rule, two implementations, because the inputs have different
 * provenance.
 *
 * The prefix is compared as a literal byte string and never as a glob: the
 * read path escapes '*', '?', '[', ']' and '\' before the prefix becomes a
 * SCAN MATCH pattern, so a glob-matching check here would accept keys the
 * scan can never return.
 */
static bool
vfdw_val_has_prefix(const VfdwTableMap *map, const char *key, size_t keylen)
{
	size_t		plen = strlen(map->keyprefix);

	return keylen >= plen && memcmp(key, map->keyprefix, plen) == 0;
}

/*
 * Is this key the name of the table's own keyset?
 *
 * Length equality first, and that is not a micro-optimisation: without it a
 * key that merely BEGINS with the keyset's name - 'kset2' for keyset 'kset' -
 * would be refused as a collision, and that key is a perfectly ordinary row.
 * The keyset is one exact name, not a namespace.
 */
static bool
vfdw_val_is_keyset(const VfdwTableMap *map, const char *key, size_t keylen)
{
	size_t		slen = strlen(map->keyset);

	return keylen == slen && memcmp(key, map->keyset, slen) == 0;
}

/*
 * On a singleton table the Valkey key is always the option value.
 *
 * A NULL key column is accepted and means "unspecified": INSERT INTO
 * tags_k(t) VALUES ('red') is the natural statement and the only shape COPY
 * tags_k (t) FROM ... can produce, and the read path never produces NULL
 * there, so a NULL is unambiguous rather than a lost value.
 *
 * A non-NULL one must match byte for byte. Accepting anything else would
 * write the row to the singleton key regardless, so the user's value would be
 * silently discarded while INSERT reported success and a later WHERE k =
 * 'other' returned nothing - the exact anomaly the singleton plan path
 * already guards on the read side. The same rule makes a singleton key column
 * non-renameable by UPDATE.
 */
static VfdwKeyVerdict
vfdw_val_singleton_key(VfdwValCtx *vc, const VfdwTableMap *map,
					   const VfdwColumn *col, Datum d, bool isnull,
					   VfdwValue *out)
{
	/* Ours, from the catalog, NUL-terminated: strlen is correct (I3). */
	size_t		slen = strlen(map->singleton_key);

	if (col != NULL && !isnull)
	{
		const char *given;
		size_t		givenlen;

		vfdw_val_render_scratch(vc, col, d, &given, &givenlen);

		if (givenlen != slen || memcmp(given, map->singleton_key, slen) != 0)
			return VFDW_KEY_SINGLETON;
	}

	out->isnull = false;
	out->data = vfdw_val_retain(vc, map->singleton_key, slen);
	out->len = slen;
	return VFDW_KEY_OK;
}

VfdwKeyVerdict
vfdw_val_key_classify(VfdwValCtx *vc, const VfdwTableMap *map,
					  const VfdwColumn *col, Datum d, bool isnull,
					  VfdwValue *out)
{
	const char *bytes;
	size_t		len;

	/*
	 * Tested before the column is touched: map->keyattno may legitimately be
	 * InvalidAttrNumber on a singleton table, which is the path that would
	 * otherwise index cols[-1].
	 */
	if (map->singleton_key != NULL)
		return vfdw_val_singleton_key(vc, map, col, d, isnull, out);

	/*
	 * Past the singleton branch, a key column is the only thing that can name
	 * the row, so its absence is a shape vfdw_map_build refuses to produce.
	 * Checked anyway: this is reachable from a SQL-callable probe, and relying
	 * on a guarantee enforced in another file is how the crash this replaces
	 * got here.
	 */
	if (col == NULL)
		return VFDW_KEY_NO_COLUMN;

	/*
	 * NULL first, so a row missing its key sees the more specific of the two
	 * errors. Running the prefix test first would report a check violation for
	 * a row whose real defect is a missing key, and would send the user to the
	 * wrong option.
	 */
	if (isnull)
		return VFDW_KEY_NULL;

	vfdw_val_render_scratch(vc, col, d, &bytes, &len);

	/*
	 * Validate, then pass through. There is no step that modifies the bytes:
	 * keyprefix filters, it never transforms, because the read path stores the
	 * raw SCAN key and refines with the prefix at plan time. A write that
	 * prepended would produce 'str:str:a' for a table whose scan already
	 * records 'str:a'.
	 *
	 * A keyset table has no prefix to agree with - membership is defined by
	 * the set, not by the key's spelling - so nothing but the collision check
	 * applies there.
	 */
	if (map->keyprefix != NULL && !vfdw_val_has_prefix(map, bytes, len))
		return VFDW_KEY_PREFIX;

	if (map->keyset != NULL && vfdw_val_is_keyset(map, bytes, len))
		return VFDW_KEY_KEYSET;

	out->isnull = false;
	out->data = vfdw_val_retain(vc, bytes, len);
	out->len = len;
	return VFDW_KEY_OK;
}

/*
 * A key outside the table's keyprefix would be written where the table that
 * wrote it can never read it back: the keyspace scan's MATCH excludes it and
 * the point-lookup refinement drops it. So it is a check-constraint violation
 * on a table-level restriction, which is what 23514 says.
 *
 * The message must not echo the key bytes - a key may contain NUL and invalid
 * UTF-8 (invariant I2) - so it names the relation and the prefix, both
 * catalog-sourced C strings. Writing only the suffix is by far the most likely
 * user error, and the README is not where the user is looking when the error
 * fires, so the errhint states the convention in full: the key column holds
 * the complete key, prefix included, because keyprefix filters and never
 * transforms.
 */
static void
vfdw_val_error_prefix(const VfdwValCtx *vc, const VfdwTableMap *map)
{
	ereport(ERROR,
			(errcode(ERRCODE_CHECK_VIOLATION),
			 errmsg("key does not begin with the table's keyprefix"),
			 errdetail("Table \"%s\" is scoped to keys beginning with \"%s\".",
					   vfdw_val_relname(vc), map->keyprefix),
			 errhint("The key column holds the complete Valkey key. Include "
					 "the \"%s\" prefix in the value you write.",
					 map->keyprefix)));
}

/*
 * A keyset table indexes its own keys in a set. A row whose key is that set's
 * name would both be SADDed to it and written as a data key of another type.
 * Phase 1 of the flush would catch that as "the key holds a different Valkey
 * type", but only at COMMIT and without saying that the key collided with the
 * table's own index.
 */
static void
vfdw_val_error_keyset(const VfdwValCtx *vc, const VfdwTableMap *map)
{
	ereport(ERROR,
			(errcode(ERRCODE_CHECK_VIOLATION),
			 errmsg("key collides with the table's keyset"),
			 errdetail("Table \"%s\" records its key names in the set \"%s\", "
					   "so that name cannot also hold a row.",
					   vfdw_val_relname(vc), map->keyset),
			 errhint("Choose a different key, or a different keyset name.")));
}

static void
vfdw_val_error_singleton(const VfdwValCtx *vc, const VfdwTableMap *map,
						 const VfdwColumn *col)
{
	ereport(ERROR,
			(errcode(ERRCODE_CHECK_VIOLATION),
			 errmsg("key column does not match the table's singleton_key"),
			 errdetail("Table \"%s\" draws every row from the single key "
					   "\"%s\".",
					   vfdw_val_relname(vc), map->singleton_key),
			 errhint("Leave the key column unspecified, or write \"%s\".",
					 map->singleton_key),
			 (vc->rel != NULL && col != NULL)
			 ? errtablecol(vc->rel, col->attnum) : 0));
}

/*
 * Report a classified refusal.
 *
 * Every arm ereports, so this never returns; the trailing elog exists so that
 * a verdict added to the enum and forgotten here is loud rather than a silent
 * fall-through past a rule that was supposed to refuse the row.
 */
static void
vfdw_val_key_error(VfdwValCtx *vc, const VfdwTableMap *map,
				   const VfdwColumn *col, VfdwKeyVerdict verdict)
{
	switch (verdict)
	{
		case VFDW_KEY_OK:
			break;
		case VFDW_KEY_NULL:
			vfdw_val_error_null(vc->rel, col);
			break;
		case VFDW_KEY_NO_COLUMN:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("table has no key column and no singleton_key")));
			break;
		case VFDW_KEY_PREFIX:
			vfdw_val_error_prefix(vc, map);
			break;
		case VFDW_KEY_KEYSET:
			vfdw_val_error_keyset(vc, map);
			break;
		case VFDW_KEY_SINGLETON:
			vfdw_val_error_singleton(vc, map, col);
			break;
	}

	elog(ERROR, "valkey_fdw: key verdict %d has no message", (int) verdict);
}

void
vfdw_val_build_key(VfdwValCtx *vc, const VfdwTableMap *map,
				   const VfdwColumn *col, Datum d, bool isnull, VfdwValue *out)
{
	VfdwKeyVerdict verdict = vfdw_val_key_classify(vc, map, col, d, isnull,
												   out);

	if (verdict != VFDW_KEY_OK)
		vfdw_val_key_error(vc, map, col, verdict);
}

/* ---------------------------------------------------------------------
 * Per-statement setup
 * --------------------------------------------------------------------- */

void
vfdw_val_ctx_init(VfdwValCtx *vc, Relation rel, const VfdwTableMap *map,
				  MemoryContext stmt_cxt, MemoryContext dest)
{
	int			i;

	vc->rel = rel;
	vc->noutfuncs = map->natts;
	vc->dest = dest;
	vc->scratch = AllocSetContextCreate(stmt_cxt, "valkey_fdw value conversion",
										ALLOCSET_SMALL_SIZES);

	/*
	 * Zeroed rather than merely allocated. A dropped column has attnum
	 * InvalidAttrNumber and no type to resolve, so its entry stays empty; a
	 * zeroed FmgrInfo faults immediately if one is ever called, where an
	 * uninitialised one would dispatch through whatever the allocator last
	 * held.
	 */
	vc->outfuncs = MemoryContextAllocZero(stmt_cxt,
										  sizeof(FmgrInfo) * Max(map->natts, 1));

	for (i = 0; i < map->natts; i++)
	{
		if (map->cols[i].attnum == InvalidAttrNumber)
			continue;
		fmgr_info_cxt(map->cols[i].typoutput, &vc->outfuncs[i], stmt_cxt);
	}
}

/*
 * Called once per row. The scratch context is a child of the statement's, so
 * nothing here is ever freed on an error path (invariant I1): an unwind
 * destroys the parent and takes it along.
 */
void
vfdw_val_ctx_reset(VfdwValCtx *vc)
{
	MemoryContextReset(vc->scratch);
}
