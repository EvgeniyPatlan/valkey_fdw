#!/usr/bin/env bash
#
# Entry point for the valkey_fdw build container.
#
# Brings up a private PostgreSQL instance on a Unix socket in /tmp, then execs
# whatever it was asked to run. Every invocation gets a fresh cluster, so a
# test run can never inherit state from a previous one.
set -euo pipefail

log() { printf '[entrypoint] %s\n' "$*" >&2; }

start_postgres() {
    if [[ -d "${PGDATA}" ]]; then
        log "reusing existing cluster at ${PGDATA}"
    else
        log "initialising cluster at ${PGDATA}"
        initdb --auth-local=trust --auth-host=trust --no-sync -D "${PGDATA}" >/dev/null
        {
            echo "unix_socket_directories = '/tmp'"
            echo "port = ${PGPORT}"
            echo "fsync = off"
            echo "full_page_writes = off"
            echo "log_min_messages = warning"
            # Needed by the TAP suite to count backend file descriptors and
            # inspect connection reuse.
            echo "log_line_prefix = '%m [%p] '"
        } >> "${PGDATA}/postgresql.conf"
    fi

    log "starting postgres"
    # max_prepared_transactions is 0 by default, and core refuses PREPARE
    # TRANSACTION for that reason inside MarkAsPreparing. The wbuf suite has to
    # be able to PREPARE an EMPTY transaction successfully, which is the clause
    # that catches an over-broad refusal in our own PRE_PREPARE handler; with
    # the default, core's refusal would mask ours either way.
    #
    # Passed with -o rather than appended to postgresql.conf because this
    # entrypoint reuses an existing cluster at ${PGDATA}, which a conf append
    # made after initdb would never reach.
    pg_ctl -D "${PGDATA}" -l "${PGDATA}/server.log" -w \
        -o "-k /tmp -c max_prepared_transactions=4" start >/dev/null
    log "postgres $(psql -X -tAc 'show server_version' postgres) ready"
}

stop_postgres() {
    pg_ctl -D "${PGDATA}" -m immediate stop >/dev/null 2>&1 || true
}

if [[ "${VFDW_NO_POSTGRES:-0}" != "1" ]]; then
    trap stop_postgres EXIT
    start_postgres
fi

exec "$@"
