-- ---------------------------------------------------------------------------
-- Which queries a vector table plans, and what every other one is told.
--
-- PLAN ONLY. Every statement here is an EXPLAIN without ANALYZE or a refusal
-- raised while planning, so nothing reaches a server and no search module has
-- to be loaded. Running the search itself needs the bundle image and lives in
-- the suite that runs on that topology; what this file asserts is the
-- decision, which is where a wrong answer would be decided.
--
-- pgvector is installed for the operators and for nothing else. valkey_fdw
-- does not link it and recognises <->, <=> and <#> by NAME - the spike
-- measured those OIDs at 16432 in one database, assigned by CREATE EXTENSION
-- and different in the next. But a KNN query cannot be written without an
-- operator of that name, so the suite that asserts one is recognised has to
-- have a real one.
-- ---------------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS vector;

CREATE SERVER kp_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER kp_srv;

CREATE FOREIGN TABLE kp (
    k    text             OPTIONS (key 'true'),
    emb  vector(4)        OPTIONS (field 'emb', index_type 'vector'),
    tag  text             OPTIONS (field 'tag', index_type 'tag'),
    tagv vector(4)        OPTIONS (field 'tagv', index_type 'tag'),
    n    integer          OPTIONS (field 'n', index_type 'numeric'),
    big  bigint           OPTIONS (field 'big', index_type 'numeric'),
    dist double precision OPTIONS (distance 'true')
) SERVER kp_srv OPTIONS (tabletype 'vector', search_index 'kpidx');

-- ---------------------------------------------------------------------------
-- The shape that plans.
-- ---------------------------------------------------------------------------
EXPLAIN (COSTS OFF)
SELECT k, dist FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 3;

-- No Sort above the scan: the path claims the ordering, because the rows
-- arrive in the index's own ranking. A plan that sorted would still be
-- correct and would mean the pathkey was not recognised.
EXPLAIN (COSTS OFF)
SELECT * FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 10;

-- All three metrics, one matcher. The operator is recognised by name; which
-- one was written travels to the executor, where it is checked against what
-- the index says it measures.
EXPLAIN (COSTS OFF)
SELECT k FROM kp ORDER BY emb <=> '[1,0,0,0]'::vector LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT k FROM kp ORDER BY emb <#> '[1,0,0,0]'::vector LIMIT 1;

-- The column on the right. Distance is symmetric, and the planner keeps the
-- operands in the order they were written, so both spellings must match.
EXPLAIN (COSTS OFF)
SELECT k FROM kp ORDER BY '[1,0,0,0]'::vector <-> emb LIMIT 2;

-- OFFSET is part of k. The server is asked for offset+count rows and the
-- Limit node above discards the first offset of them, which is the same
-- answer as asking for count starting at offset.
EXPLAIN (COSTS OFF)
SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 2 OFFSET 3;

-- A query vector that is an expression rather than a literal. It is evaluated
-- when the scan runs, so anything constant with respect to this table works.
--
-- current_setting rather than a scalar subquery, which was the first spelling:
-- a subquery becomes an InitPlan, and EXPLAIN labels those differently on
-- PostgreSQL 16 than on 17 and 18. The point of the case is that the query
-- vector is not a Const, and a stable function call makes it just as
-- thoroughly without dragging core's plan-printing into the assertion.
SET vfdw.q = '[0,1,0,0]';
EXPLAIN (COSTS OFF)
SELECT k FROM kp
ORDER BY emb <-> current_setting('vfdw.q')::vector LIMIT 1;

-- ---------------------------------------------------------------------------
-- Everything else, and what each is told.
--
-- One message per reason, because "unsupported" would leave a user guessing
-- between the join, the WHERE and the missing LIMIT - and the fix for each is
-- a different line of their query.
-- ---------------------------------------------------------------------------

-- No ORDER BY at all: there is no query vector, so there is nothing to
-- search for. This is the plain SELECT that a keyspace walk would answer
-- with the right rows in an order that means nothing.
SELECT * FROM kp;

-- ORDER BY something that is not a distance.
SELECT k FROM kp ORDER BY k LIMIT 5;

-- The right operator on a column of the wrong TYPE is not this wrapper's
-- refusal at all - pgvector defines <-> over vectors and PostgreSQL rejects
-- it over text before any of this is reached. Recorded because it is what a
-- user meets, and because it is evidence for the claim above: the operator is
-- pgvector's, and nothing here defines one.
SELECT k FROM kp ORDER BY tag <-> 'x' LIMIT 5;

-- The right operator on a column of the right type that is indexed as
-- something else. THIS is the wrapper's own rule: index_type says what the
-- valkey-search index holds the field as, and a tag is not rankable.
SELECT k FROM kp ORDER BY tagv <-> '[1,0,0,0]'::vector LIMIT 5;

-- DESC is the FARTHEST k, which is not a k-row result reversed: the rows
-- needed are the ones the search did not return.
--
-- Both spellings, and the second is the one that tests what it looks like it
-- tests. Plain DESC implies NULLS FIRST, which is refused by a rule of its
-- own, so removing the ascending check leaves this first query refused
-- anyway - a mutation proved exactly that. NULLS LAST reaches the direction
-- check and nothing else.
SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector DESC LIMIT 5;
SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector DESC NULLS LAST LIMIT 5;

-- And NULLS FIRST on an ascending order, which is the other half of that
-- pair: the direction is right and the null placement is not one a search
-- can produce, because it never returns a NULL distance to place.
SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector NULLS FIRST LIMIT 5;

-- Two orderings. The second is a tie-break the server was never told about.
SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector, k LIMIT 5;

-- No LIMIT: no k.
SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector;

-- A LIMIT that is not a plan-time constant. Taken from the planner's own
-- limit_tuples, which is -1 exactly when the count is not known, so this
-- needs no separate rule.
PREPARE p(int) AS
    SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE p(5);
RESET plan_cache_mode;
DEALLOCATE p;

-- ---------------------------------------------------------------------------
-- A WHERE clause is compiled into the search, or the query is refused.
--
-- Applying one above the scan instead would subtract rows from a set the
-- server already cut to k, so the answer would be fewer than k rows and not
-- the k nearest matching ones. The clauses stay in the plan as rechecks, which
-- is safe only because each pushed term means EXACTLY what its clause means -
-- test/regress/sql/vfilter.sql is where that exactness was measured.
--
-- The plan shows the filter's SHAPE with `$` for each bound, because a bound
-- may be a Param and is not a plan-time fact. It is shown because a filter
-- pushed wrongly returns k rows in order and looks exactly like one pushed
-- rightly.
-- ---------------------------------------------------------------------------
EXPLAIN (COSTS OFF)
SELECT k FROM kp WHERE n >= 3
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT k FROM kp WHERE n > 3
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT k FROM kp WHERE n < 3
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT k FROM kp WHERE n = 3
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- Two clauses become two terms, ANDed by the space between them.
EXPLAIN (COSTS OFF)
SELECT k FROM kp WHERE n >= 1 AND n < 10
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- The constant on the left. Which side the column is on decides which END of
-- the range the bound is, and getting that backwards is a filter that returns
-- rows nobody asked for.
EXPLAIN (COSTS OFF)
SELECT k FROM kp WHERE 3 < n
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- A parameterised bound, which is why the bounds travel as expressions.
PREPARE pf(int) AS
    SELECT k FROM kp WHERE n < $1
    ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE pf(5);
RESET plan_cache_mode;
DEALLOCATE pf;

-- ---------------------------------------------------------------------------
-- And what is not pushed, each refused rather than applied locally.
-- ---------------------------------------------------------------------------

-- A TAG field. The index tokenises it on its separator, so @tag:{a} matches a
-- document whose field holds "a,b" while SQL `tag = 'a'` does not - a filter
-- that is a SUPERSET, which is the direction the recheck cannot recover from.
SELECT k FROM kp WHERE tag = 'a'
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- A bigint. A valkey-search NUMERIC is a double, so a value beyond 2^53 is
-- rounded on the way into the index and two distinct values become one. The
-- bound being small does not help: it is the STORED values that round.
SELECT k FROM kp WHERE big = 3
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- <> has no range that means it.
SELECT k FROM kp WHERE n <> 3
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- OR, which a space-separated list of terms cannot express.
SELECT k FROM kp WHERE n = 1 OR n = 2
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- IS NULL, which is not a comparison a range can hold.
SELECT k FROM kp WHERE n IS NULL
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- The KEY column, which is the most natural clause to write and is still
-- refused: the index has no attribute for it, and narrowing by key is what
-- the ordinary key path does on a table that is not a vector one.
SELECT k FROM kp WHERE k = 'kp:1'
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- A bound that depends on the table itself would be a different filter per
-- row, which one query string cannot express.
SELECT k FROM kp WHERE n < dist
ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- A join. The LIMIT bounds the join's output, not this scan's.
CREATE TABLE kp_local (k text);
SELECT kp.k FROM kp JOIN kp_local USING (k)
ORDER BY kp.emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- A second table in the FROM list, without a join clause.
SELECT kp.k FROM kp, kp_local
ORDER BY kp.emb <-> '[1,0,0,0]'::vector LIMIT 5;

-- An aggregate or a DISTINCT consumes rows the LIMIT above it does not
-- count, and both are guarded. Neither reaches that guard here, because a
-- query with one cannot also have an ORDER BY this matcher accepts - which
-- is why the reason reported is the ORDER BY. The guard is what makes that
-- true rather than a coincidence.
SELECT count(*) FROM kp;
SELECT DISTINCT k FROM kp ORDER BY k LIMIT 5;

-- A subquery is planned in its own right, so the same shape inside one is
-- the shape that runs: the LIMIT bounds that scan and nothing stands between
-- them. What the outer query then does with the k rows is its own business.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM (
    SELECT k FROM kp ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 5
) s;

-- A query vector that depends on the table itself would be a different
-- search per row, which FT.SEARCH cannot express.
SELECT k FROM kp ORDER BY emb <-> emb LIMIT 5;

-- ---------------------------------------------------------------------------
-- Writes are refused by their own reason, which is now the one a user sees.
--
-- The refusal used to be raised while BUILDING THE MAP, so every write met
-- the read path's objection on the way past and was told about ORDER BY. It
-- moved to the access-path choice, where it is a fact about the query rather
-- than about the table - and PlanForeignModify refuses a write before any
-- scan of the target is planned, so all three now say what is wrong with the
-- write.
-- ---------------------------------------------------------------------------
INSERT INTO kp VALUES ('v:1', '[1,0,0,0]', 'a', '[0,0,0,0]', 1, 1, 1.0);
UPDATE kp SET tag = 'b';
DELETE FROM kp;

SELECT c.relname, pg_relation_is_updatable(c.oid, false) AS mask
FROM pg_class c WHERE c.relname = 'kp';

-- ---------------------------------------------------------------------------
-- A FIELD NAME BECOMES PART OF THE QUERY, so one carrying the query
-- language's punctuation is refused at the table rather than escaped.
--
-- The server accepts `@n:[1 1] @tag:{a}` written as one field name without
-- complaint and applies both terms, so a name like this would ask something
-- the table never declared. Refused rather than escaped, because
-- valkey-search's grammar is not this tree's to model and a wrong escape is
-- silent. On a hash table the same name is an argument rather than syntax, so
-- nothing is refused there.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE kp_badname (
    k text      OPTIONS (key 'true'),
    e vector(4) OPTIONS (field 'emb} @n:[0 0', index_type 'vector')
) SERVER kp_srv OPTIONS (tabletype 'vector', search_index 'kpidx');

SELECT k FROM kp_badname ORDER BY e <-> '[1,0,0,0]'::vector LIMIT 1;

CREATE FOREIGN TABLE kp_badname_hash (
    k text OPTIONS (key 'true'),
    e text OPTIONS (field 'emb} @n:[0 0')
) SERVER kp_srv OPTIONS (tabletype 'hash', keyprefix 'kp:');

EXPLAIN (COSTS OFF) SELECT * FROM kp_badname_hash;

-- ---------------------------------------------------------------------------
-- A table without index_type 'vector' has no column a search could rank by,
-- so the same query is refused for naming the wrong column.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE kp_untyped (
    k   text      OPTIONS (key 'true'),
    emb vector(4) OPTIONS (field 'emb')
) SERVER kp_srv OPTIONS (tabletype 'vector', search_index 'kpidx');

SELECT k FROM kp_untyped ORDER BY emb <-> '[1,0,0,0]'::vector LIMIT 1;

DROP TABLE kp_local;
DROP SERVER kp_srv CASCADE;
