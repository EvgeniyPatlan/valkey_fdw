/*-------------------------------------------------------------------------
 *
 * vfdw_val.h
 *		Datum to bytes: the write direction, and the key rules that go with it.
 *
 * One header, three implementation files, because the 800-line gate does not
 * fit the whole of it and the three parts answer different questions:
 *
 *	src/vfdw_val.c		Datum to bytes, the NULL rule per column role, and the
 *						key builder with its prefix, keyset and singleton
 *						validation.
 *	src/vfdw_score.c	what this wrapper will accept as a zset score.
 *	src/vfdw_slot.c		hash-tag extraction and the CRC-16 cluster slot.
 *
 * They share a header because a caller needs all three to write one row, and
 * splitting the declarations would only make that harder to see.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_val.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_VAL_H
#define VFDW_VAL_H

#include "vfdw.h"

#include "fmgr.h"
#include "utils/relcache.h"

#include "vfdw_map.h"

/*
 * The largest single argument valkey_fdw will hand to Valkey.
 *
 * This is proto-max-bulk-len's default. It is a ceiling we impose, not a
 * promise about the server: an operator may lower proto-max-bulk-len, and we
 * deliberately never probe it, because the value can change between the probe
 * and the pre-commit flush and the probe would cost a round trip per
 * statement to produce an answer that might already be stale. If the server's
 * limit is lower, its own error reports it.
 *
 * write_max_bytes does not subsume this. That option bounds the transaction
 * and is settable to 1 GiB, so a single 600 MB value slips under it and
 * reaches the wire - where a bulk argument over the limit is a PROTOCOL
 * error, not a command error: the server answers "Protocol error: invalid
 * bulk length" and closes the connection. At pre-commit that is only
 * classifiable as 08007 transaction_resolution_unknown for the whole
 * transaction, so an avoidable client-side check is the difference between a
 * clear statement error naming a column and an unknown-outcome COMMIT.
 */
#define VFDW_MAX_ARG_BYTES	((size_t) 536870912)

/*
 * One converted value.
 *
 * NULL is signalled by the isnull flag and by nothing else. A non-NULL empty
 * value is isnull = false, len = 0, and data may be any pointer at all,
 * including NULL. No code may infer NULL-ness from data == NULL and len must
 * never carry a sentinel.
 *
 * The read side does use a null pointer as its NULL signal
 * (src/vfdw_row.c:vfdw_scan_store) and gets away with it only because
 * libvalkey never returns a NULL str for a zero-length bulk. Here, VARDATA_ANY
 * of an empty bytea and palloc(0) both legitimately produce pointers a caller
 * would read as "nothing", so reusing the pointer would collapse '' into NULL
 * - and Valkey stores '' perfectly well, while the read path returns it as a
 * distinct value from an absent key and from an absent hash field. Three
 * states survive the read; conflating any two of them on the write side makes
 * the round trip lossy in a way no error would reveal.
 */
typedef struct VfdwValue
{
	bool		isnull;
	const char *data;
	size_t		len;
} VfdwValue;

/*
 * Per-statement conversion state.
 *
 * A plain struct rather than an opaque handle so that the diagnostic entry
 * points in src/vfdw_testfuncs.c can build one around a single column and
 * drive exactly the code a real statement drives.
 *
 *	rel			names the relation and its columns in error messages, and
 *				carries errtablecol. May be NULL in a diagnostic.
 *	outfuncs	one FmgrInfo per attribute, indexed by attnum - 1, resolved
 *				once per statement. Never per row: a COPY of 5000 rows would
 *				otherwise pay a syscache lookup per column per row.
 *	scratch		reset after every row. Detoasting and output functions
 *				allocate in the current context and clean up after nothing,
 *				so their garbage is confined here.
 *	dest		the transaction's write buffer. Converted bytes are copied
 *				into it and only that copy is retained, because VfdwCmd
 *				borrows argument pointers and copies nothing while the flush
 *				runs at pre-commit, long after any per-tuple context is gone.
 */
typedef struct VfdwValCtx
{
	Relation	rel;
	FmgrInfo   *outfuncs;
	int			noutfuncs;
	MemoryContext scratch;
	MemoryContext dest;
} VfdwValCtx;

extern void vfdw_val_ctx_init(VfdwValCtx *vc, Relation rel,
							  const VfdwTableMap *map,
							  MemoryContext stmt_cxt, MemoryContext dest);
extern void vfdw_val_ctx_reset(VfdwValCtx *vc);

/*
 * Render one Datum to bytes, with no rule beyond the column's type.
 *
 * Every function below that can refuse a value refuses it while the bytes are
 * still in the scratch context, and retains them into dest only once they are
 * accepted - so a value refused BY ITS OWN RULE leaves nothing in the write
 * buffer. The buffer's byte counter is deliberately monotonic, so bytes
 * retained for a value that was then refused would consume write_max_bytes
 * the user never spent.
 *
 * That is a statement about one value, not about one row, and the difference
 * is load-bearing. Retention is per column, so a row refused by a LATER
 * column's rule - or by a buffer cap - leaves the columns already accepted
 * behind, and those bytes are counted. That is required rather than
 * tolerated: if a cap refusal did not count the bytes it had already caused,
 * a LOOP ... EXCEPTION ... END over failing rows would allocate without bound
 * while the cap read as untouched, which is the exact failure the monotonic
 * counter exists to prevent. The modify callbacks render in refusal-cost
 * order - key, score, member, payload - so the common refusals still leave
 * nothing behind.
 */
extern void vfdw_val_render(VfdwValCtx *vc, const VfdwColumn *col,
							Datum d, bool isnull, VfdwValue *out);

/*
 * Render one Datum and apply the NULL rule its column's role implies.
 *
 * Exhaustive over VfdwColKind: a kind with no write meaning reaches an
 * internal error rather than a silent default, so the next kind added to the
 * enum cannot acquire accidental semantics.
 */
extern void vfdw_val_from_slot(VfdwValCtx *vc, const VfdwTableMap *map,
							   const VfdwColumn *col, Datum d, bool isnull,
							   VfdwValue *out);

/*
 * Which rule refused a key, or VFDW_KEY_OK.
 *
 * Separated from the reporting for the same reason VfdwScoreVerdict is: three
 * of these five refusals share SQLSTATE 23514, so a test comparing only the
 * SQLSTATE passes while the builder rejects the key through entirely the wrong
 * branch. One classifier feeds both the raising path and the diagnostic, so
 * the rule and the name for it cannot drift.
 */
typedef enum VfdwKeyVerdict
{
	VFDW_KEY_OK = 0,
	VFDW_KEY_NULL,				/* a null key names no row */
	VFDW_KEY_NO_COLUMN,			/* no key column and no singleton_key */
	VFDW_KEY_PREFIX,			/* outside the table's keyprefix */
	VFDW_KEY_KEYSET,			/* collides with the table's own keyset */
	VFDW_KEY_SINGLETON			/* disagrees with singleton_key */
} VfdwKeyVerdict;

extern const char *vfdw_val_key_verdict_name(VfdwKeyVerdict verdict);

/*
 * The Valkey key for one row, without raising.
 *
 * Fills *out only when the verdict is VFDW_KEY_OK, so a refused key retains
 * nothing into the write buffer. May still raise for a reason that is not a
 * key rule - a rendering over VFDW_MAX_ARG_BYTES, or an output function of
 * the column's own type.
 */
extern VfdwKeyVerdict vfdw_val_key_classify(VfdwValCtx *vc,
											const VfdwTableMap *map,
											const VfdwColumn *col, Datum d,
											bool isnull, VfdwValue *out);

/*
 * The Valkey key for one row: render, then validate against the table's
 * keyprefix, keyset or singleton_key. Never concatenates, strips or rewrites
 * - the key column carries the complete key, prefix included, on write exactly
 * as on read. keyprefix filters, it never transforms: the scan keeps the raw
 * key the server returned and refines it against the prefix, so a builder that
 * prepended would write 'str:str:a' for a row that same table scans as
 * 'str:a'.
 *
 * col may be NULL only on a singleton table: a singleton table is allowed to have no key column at all
 * (src/vfdw_map.c, vfdw_map_assign_defaults), which is the path that would
 * otherwise index cols[-1].
 */
extern void vfdw_val_build_key(VfdwValCtx *vc, const VfdwTableMap *map,
							   const VfdwColumn *col, Datum d, bool isnull,
							   VfdwValue *out);

VFDW_NORETURN extern void vfdw_val_error_null(Relation rel,
											  const VfdwColumn *col) VFDW_NORETURN_TAIL;
VFDW_NORETURN extern void vfdw_val_error_all_fields_null(Relation rel) VFDW_NORETURN_TAIL;

/*
 * Why a rendered score is not acceptable, or VFDW_SCORE_OK.
 *
 * Separated from the reporting so that a diagnostic can classify a whole
 * table of vectors in one query, and so that the grammar can be read without
 * the message text around it.
 */
typedef enum VfdwScoreVerdict
{
	VFDW_SCORE_OK = 0,
	VFDW_SCORE_EMPTY,
	VFDW_SCORE_NOT_A_NUMBER,
	VFDW_SCORE_TRAILING,
	VFDW_SCORE_OVERFLOW,
	VFDW_SCORE_UNDERFLOW,

	/*
	 * +-Infinity, in any spelling either end produces. Distinguished from
	 * NOT_A_NUMBER because Infinity IS a number and Valkey stores it: the read
	 * path returns +inf scores as 'Infinity' or 'inf' with no complaint, so
	 * this is the one refusal a row this wrapper itself produced can meet, and
	 * an errdetail calling it "not a number" sends the user hunting a parse
	 * bug that is not there. Refusing it at all is policy; see
	 * src/vfdw_score.c and the round-trip note in README.md.
	 */
	VFDW_SCORE_NOT_FINITE
} VfdwScoreVerdict;

extern VfdwScoreVerdict vfdw_val_score_classify(const char *data, size_t len);
extern const char *vfdw_val_score_verdict_name(VfdwScoreVerdict verdict);
extern void vfdw_val_check_score(const char *data, size_t len,
								 const char *relname, const char *colname);

/*
 * Cluster slot arithmetic.
 *
 * Both are pure and total over every byte string including the empty one, and
 * neither may ereport: a slot is exactly what an error message about a
 * cross-slot refusal needs, and an ereport inside an ereport is fatal.
 *
 * vfdw_val_hashtag returns a pointer INTO the caller's buffer with a length -
 * no copy, no palloc, no NUL termination.
 *
 * The slot is consulted, not merely computed: vfdw_cluster_route reads it to
 * find the node that owns a key, and vfdw_flush_cluster_slot reads it to
 * refuse a transaction whose keys do not all agree on one.
 *
 * Slot 0 is NOT a usable "no slot" sentinel. The empty key hashes to 0, and so
 * does any key consisting solely of NUL bytes, because CRC16 with init 0x0000
 * is a fixed point at 0x00. Represent "unbound" with a separate flag.
 */
extern void vfdw_val_hashtag(const char *key, size_t keylen,
							 const char **tag, size_t *taglen);
extern uint16 vfdw_val_crc16(const char *buf, size_t len);
extern uint16 vfdw_val_slot(const char *key, size_t keylen);

#endif							/* VFDW_VAL_H */
