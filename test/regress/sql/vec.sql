-- ---------------------------------------------------------------------------
-- Vectors, between PostgreSQL's text form and Valkey's bytes.
--
-- valkey-search stores a vector as raw little-endian FLOAT32 with no header
-- and no count, and PostgreSQL has no type of that shape. The join between
-- them is the text form, and this file is the whole contract: what each
-- direction produces, what round-trips, and what is refused.
--
-- STANDALONE, not the search topology. Nothing here talks to a server - both
-- directions are pure conversion - so the suite that asserts them should run
-- everywhere rather than only where a search module happens to be loaded.
--
-- The literals are written as bytea rather than as text on the Valkey side
-- for the reason invariant I3 gives: element 0.0f is four NUL bytes, so a
-- vector containing a zero cannot be presented as a string at all.
-- ---------------------------------------------------------------------------

-- ---------------------------------------------------------------------------
-- Bytes to text.
--
-- 1.0f is 0x0000803f little-endian, which is the value every other assertion
-- in this file is anchored on: it is asymmetric byte for byte, so a
-- big-endian read produces a different number rather than the same one.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_vec_to_text('\x0000803f'::bytea) AS one;

SELECT valkey_fdw_test_vec_to_text(
    '\x0000803f0000004000004040'::bytea) AS one_two_three;

-- The zero vector, which is the case a text-typed probe could not express.
SELECT valkey_fdw_test_vec_to_text('\x0000000000000000'::bytea) AS zeroes,
       length('\x0000000000000000'::bytea) AS bytes_in;

-- Negatives and fractions, so the sign bit and the mantissa are both read.
SELECT valkey_fdw_test_vec_to_text('\x0000003f000000c0'::bytea) AS half_minus_two;

-- ---------------------------------------------------------------------------
-- Text to bytes.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_vec_from_text('[1]') AS one;
SELECT valkey_fdw_test_vec_from_text('[1,2,3]') AS one_two_three;
SELECT valkey_fdw_test_vec_from_text('[0,0]') AS zeroes;
SELECT valkey_fdw_test_vec_from_text('[0.5,-2]') AS half_minus_two;

-- Whitespace is what a type's output function may or may not emit, so both
-- spellings have to arrive at the same bytes.
SELECT valkey_fdw_test_vec_from_text('[1,2,3]')
     = valkey_fdw_test_vec_from_text('  [ 1 , 2 , 3 ]  ') AS whitespace_ignored;

-- ---------------------------------------------------------------------------
-- The round trip, which is the property that matters.
--
-- Asserted in both directions rather than one: text->bytes->text proves the
-- digits survive, and bytes->text->bytes proves no float was rounded on the
-- way past. A conversion that lost a digit would pass the first alone.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_vec_to_text(valkey_fdw_test_vec_from_text(v)) AS out, v AS in
FROM (VALUES ('[1,2,3]'), ('[0,0,0,0]'), ('[-1.5,0.25]'),
             ('[3.4028235e+38]'), ('[1.1754944e-38]')) t(v);

SELECT valkey_fdw_test_vec_from_text(valkey_fdw_test_vec_to_text(b)) = b AS survives
FROM (VALUES ('\x0000803f'::bytea),
             ('\x00000000000000000000000000000000'::bytea),
             ('\xdeadbeefcafebabe'::bytea)) t(b);

-- A float32 cannot hold a float64's digits, and this is where that shows: the
-- text goes in, is rounded to the nearest float32 by float4in, and comes back
-- as that float32's own shortest spelling. Recorded rather than hidden,
-- because a user comparing what they wrote with what they read back will meet
-- it, and the alternative - refusing the literal - would refuse a value
-- valkey-search stores perfectly well.
SELECT valkey_fdw_test_vec_to_text(
    valkey_fdw_test_vec_from_text('[0.1,0.123456789]')) AS narrowed;

-- ---------------------------------------------------------------------------
-- What is refused.
--
-- Each of these could instead be guessed at, and every guess produces a
-- vector that searches the wrong point in space - which is a wrong answer
-- with no symptom, so all of them raise.
-- ---------------------------------------------------------------------------

-- A length that is not a whole number of elements. Truncating would build a
-- shorter vector that the index's dimension check might well accept.
SELECT valkey_fdw_test_vec_to_text('\x0000803f00'::bytea);

-- Not a vector literal at all - which is what a column of the wrong type
-- would hand over.
SELECT valkey_fdw_test_vec_from_text('1,2,3');
SELECT valkey_fdw_test_vec_from_text('{1,2,3}');

-- Unclosed, and trailing junk. The first would otherwise parse a prefix and
-- search with it; the second is what a type whose output carries a suffix
-- (a dimension, a unit) would produce.
SELECT valkey_fdw_test_vec_from_text('[1,2,3');
SELECT valkey_fdw_test_vec_from_text('[1,2,3] extra');

-- An element that is not a number. float4in names the offending text, which
-- is more use than "invalid vector" would be.
SELECT valkey_fdw_test_vec_from_text('[1,x,3]');

-- The empty vector. valkey-search indexes have a fixed dimension and no
-- zero-dimensional form, so this could only ever become a server error.
SELECT valkey_fdw_test_vec_from_text('[]');

-- An empty byte string is the other spelling of the same thing, and is
-- refused for the same reason rather than becoming "[]".
SELECT valkey_fdw_test_vec_to_text(''::bytea);
