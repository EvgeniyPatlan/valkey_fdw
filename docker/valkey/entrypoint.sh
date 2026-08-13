#!/usr/bin/env sh
#
# Mode selector for the Valkey test server.
#
#   standalone  plain TCP on 6379
#   tls         TLS required on 6379 (plain port disabled)
#   acl         plain TCP on 6379 with users.acl loaded
#   cluster     cluster-enabled node; slots are assigned by the harness
#   search      standalone plus --loadmodule for the bundled search module.
#
#               The comment here used to say the bundle image loads it
#               already. It does not: the image SHIPS /usr/lib/valkey/*.so and
#               loads nothing, so FT.CREATE came back "unknown command" and
#               MODULE LIST was empty. Found by asking the server rather than
#               by reading, which is why the Phase 5 spike exists.
set -eu

MODE="${1:-standalone}"
shift 2>/dev/null || true

COMMON="--save --appendonly no --protected-mode no --enable-debug-command yes"

case "$MODE" in
    standalone)
        # shellcheck disable=SC2086
        exec valkey-server --port 6379 --bind 0.0.0.0 $COMMON "$@"
        ;;

    search)
        # Only the search module. The bundle also ships json, bloom and ldap,
        # and loading what no suite exercises would mean a topology whose
        # failures are its own configuration - the lesson the cassert image
        # already taught this tree.
        # shellcheck disable=SC2086
        exec valkey-server --port 6379 --bind 0.0.0.0 $COMMON \
            --loadmodule /usr/lib/valkey/libsearch.so "$@"
        ;;

    acl)
        # shellcheck disable=SC2086
        exec valkey-server --port 6379 --bind 0.0.0.0 \
            --aclfile /etc/valkey/users.acl $COMMON "$@"
        ;;

    tls|tls-expired)
        # Plain port off entirely: a test that accidentally connects without
        # TLS must fail to connect, not silently succeed in cleartext.
        #
        # tls-expired serves a certificate whose validity window has already
        # passed. It is issued for its own hostname and by the same CA, so the
        # only thing wrong with it is the dates - otherwise a rejection would
        # not say which check did the rejecting.
        CERT=server
        [ "$MODE" = "tls-expired" ] && CERT=expired

        # shellcheck disable=SC2086
        exec valkey-server --port 0 --tls-port 6379 --bind 0.0.0.0 \
            --tls-cert-file "/tls/${CERT}.crt" \
            --tls-key-file "/tls/${CERT}.key" \
            --tls-ca-cert-file /tls/ca.crt \
            --tls-auth-clients no \
            $COMMON "$@"
        ;;

    cluster)
        # shellcheck disable=SC2086
        exec valkey-server --port 6379 --bind 0.0.0.0 \
            --cluster-enabled yes \
            --cluster-config-file /tmp/nodes.conf \
            --cluster-node-timeout 5000 \
            $COMMON "$@"
        ;;

    *)
        echo "unknown mode: $MODE (expected standalone|tls|acl|cluster|search)" >&2
        exit 64
        ;;
esac
