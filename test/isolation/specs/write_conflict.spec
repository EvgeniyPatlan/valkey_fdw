# Two sessions writing the same Valkey keyspace.
#
# Everything else in this project runs one session at a time, so this is the
# only place the answers below can be got at. All three are properties of the
# design rather than of the code's internals, which is why they are worth
# pinning even though they read as obvious:
#
#   1. A transaction's buffered writes are invisible to anyone else until it
#      commits - trivially true because they are in backend memory, and worth
#      asserting so that any future move toward writing early breaks here.
#   2. Cross-transaction uniqueness is reported at COMMIT, not at INSERT. That
#      is the same shape as a DEFERRABLE INITIALLY DEFERRED unique constraint,
#      and it is forced: deciding earlier needs the EXISTS-then-write read
#      that a check-then-write race would allow.
#   3. The loser's transaction is refused whole. No part of it is applied.

setup
{
    CREATE EXTENSION IF NOT EXISTS valkey_fdw;
    CREATE SERVER iso_srv FOREIGN DATA WRAPPER valkey_fdw
        OPTIONS (host 'valkey', port '6379');
    CREATE USER MAPPING FOR CURRENT_USER SERVER iso_srv;
    CREATE FOREIGN TABLE iso (k text OPTIONS (key 'true'), v text)
        SERVER iso_srv OPTIONS (tabletype 'string', keyprefix 'iso:');
    SELECT valkey_fdw_test_probe('iso_srv', 0, 'DEL', 'iso:k', 'iso:other');
}

teardown
{
    SELECT valkey_fdw_test_probe('iso_srv', 0, 'DEL', 'iso:k', 'iso:other');
    DROP FOREIGN TABLE iso;
    DROP USER MAPPING FOR CURRENT_USER SERVER iso_srv;
    DROP SERVER iso_srv;
}

session s1
step s1_begin  { BEGIN; }
step s1_insert { INSERT INTO iso VALUES ('iso:k', 'from s1'); }
step s1_other  { INSERT INTO iso VALUES ('iso:other', 'also s1'); }
step s1_read   { SELECT k, v FROM iso ORDER BY k; }
step s1_commit { COMMIT; }
step s1_abort  { ROLLBACK; }

session s2
step s2_begin  { BEGIN; }
step s2_insert { INSERT INTO iso VALUES ('iso:k', 'from s2'); }
step s2_read   { SELECT k, v FROM iso ORDER BY k; }
step s2_commit { COMMIT; }

# s1 wins; s2 is refused at its own COMMIT and nothing of s2 survives.
permutation s1_begin s2_begin s1_insert s2_insert s2_read s1_commit s2_commit s1_read

# The order of the INSERTs does not decide it - the order of the COMMITs does.
permutation s1_begin s2_begin s2_insert s1_insert s2_commit s1_commit s1_read

# A rolled-back writer leaves the field clear for the other.
permutation s1_begin s2_begin s1_insert s2_insert s1_abort s2_commit s1_read

# The loser's OTHER key is discarded too: the unit is refused whole, not
# per-key. Without this the suite would pass on a design that applied
# everything except the conflicting row.
permutation s1_begin s2_begin s2_insert s2_commit s1_insert s1_other s1_commit s1_read
