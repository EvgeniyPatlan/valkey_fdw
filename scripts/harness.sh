#!/usr/bin/env bash
#
# valkey_fdw container harness.
#
# Every build and every test runs through here, locally and in CI, so that
# "green on my machine" and "green in CI" are the same statement. Uses plain
# docker: compose v1 is end-of-life and v2 is not universally installed, and
# the orchestration needed here is small enough not to justify the dependency.
#
#   ./scripts/harness.sh images
#   ./scripts/harness.sh up standalone
#   ./scripts/harness.sh test
#   ./scripts/harness.sh test --topology tls --suite tls
#   ./scripts/harness.sh isolation            # two-session specs (standalone)
#   ./scripts/harness.sh tap                  # two-session cancel proofs
#   ./scripts/harness.sh bench                # flush latency vs row count
#   ./scripts/harness.sh clean                # remove build products
#   ./scripts/harness.sh shell
#   ./scripts/harness.sh down
#   ./scripts/harness.sh ci
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# --------------------------------------------------------------------------
# Defaults. Override with flags or environment.
# --------------------------------------------------------------------------
PG_MAJOR="${PG_MAJOR:-17}"
VALKEY_VERSION="${VALKEY_VERSION:-9.0.5}"
LIBVALKEY_REF="${LIBVALKEY_REF:-0.5.0}"
TOPOLOGY="${TOPOLOGY:-standalone}"
SUITE="${SUITE:-}"
SANITIZE="${SANITIZE:-}"
COVERAGE="${COVERAGE:-0}"
VENDORED="${VENDORED:-0}"
STRICT="${STRICT:-1}"

NET=vfdw-net
VK=vfdw-valkey
BUILD_IMAGE_PREFIX=valkey_fdw/build
CASSERT_IMAGE_PREFIX=valkey_fdw/cassert
VALKEY_IMAGE_PREFIX=valkey_fdw/valkey

# PG majors and Valkey versions the ci subcommand sweeps.
CI_PG_MAJORS=(16 17 18)
CI_VALKEY_VERSIONS=(8.1.9 9.0.5 9.1.1)

STATE_DIR="$REPO_ROOT/.harness"
mkdir -p "$STATE_DIR"

# --------------------------------------------------------------------------
say()  { printf '\033[1;36m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

build_image() {
    # CASSERT=1 swaps in a PostgreSQL built with --enable-cassert, which is
    # the only way to get CLOBBER_FREED_MEMORY and MEMORY_CONTEXT_CHECKING.
    # See docker/Dockerfile.cassert for why ASan is not the answer here.
    if [[ "${CASSERT:-0}" == "1" ]]; then
        echo "${CASSERT_IMAGE_PREFIX}:pg${PG_MAJOR}"
    else
        echo "${BUILD_IMAGE_PREFIX}:pg${PG_MAJOR}"
    fi
}
valkey_image() {
    local kind="${1:-plain}"
    echo "${VALKEY_IMAGE_PREFIX}-${kind}:${VALKEY_VERSION}"
}

# The search topology needs the module bundle; everything else uses the plain
# server. Both are built from the same Dockerfile with a different base.
base_for_kind() {
    case "$1" in
        bundle) echo "valkey/valkey-bundle:${VALKEY_VERSION}" ;;
        *)      echo "valkey/valkey:${VALKEY_VERSION}" ;;
    esac
}

# Which regression suites are meaningful on which topology. The fault suite
# needs the proxy in front of the server; the io suite assumes the server
# behaves.
# The Valkey major, for the suites whose subject only exists on some of them.
#
# A suite is already scoped to the topology where it means something; a feature
# the server either has or does not have is the same statement about a
# different axis. Per-field expiry arrived in Valkey 9, so `ttl` asserts what it
# does and `ttl_absent` asserts the refusal, and each runs only where its own
# assertion is the true one. Neither is skipped anywhere - between them they
# cover every supported server.
valkey_major() {
    echo "${VALKEY_VERSION%%.*}"
}

suites_for_topology() {
    local ttl_suite=ttl_absent

    [[ "$(valkey_major)" -ge 9 ]] && ttl_suite=ttl

    case "$1" in
        # priv runs last: it drops and re-creates valkey_fdw_test to assert the
        # split between the two extensions, so anything after it would be
        # running against a catalog this suite briefly emptied.
        standalone) echo "smoke probe options ddl mapping scan fetch legacy position ${ttl_suite} io pool leak val wbuf modify script dml overlay priv" ;;
        # smoke is no longer topology-neutral: now that it really scans, it
        # needs a server it can reach in plaintext. The tls suite covers the
        # load-and-connect path for this topology itself.
        tls)        echo "tls probe_tls" ;;
        # The default user is disabled on this topology, so nothing that
        # connects without credentials can run here - smoke included.
        acl)        echo "acl probe_acl" ;;
        # The write-path suites run here as well as on standalone, which is
        # the whole of item 5.4: the overlay, the ledger fold and the script
        # were exercised only over a plain socket to one node, and the proxy
        # this topology puts in front answers to the same name, so they run
        # unchanged and reach the server by a different path.
        fault)      echo "smoke fault wfault resp wbuf overlay script dml" ;;

        cluster)    echo "smoke cluster" ;;
        search)     echo "smoke vsearch" ;;

        # tls, acl and search bring their topology up and confirm the
        # extension loads against it, but have no feature suite yet: the
        # search suite lands with the pushdown work. Listing only `smoke` here is
        # deliberate - running the standalone suites against these topologies
        # would fail for reasons that have nothing to do with the code under
        # test, and pretending otherwise would make CI green mean less.
        *)          echo "smoke" ;;
    esac
}

kind_for_topology() {
    case "$1" in
        search) echo bundle ;;
        *)      echo plain ;;
    esac
}

# --------------------------------------------------------------------------
parse_flags() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --pg)        PG_MAJOR="$2"; shift 2 ;;
            --valkey)    VALKEY_VERSION="$2"; shift 2 ;;
            --topology)  TOPOLOGY="$2"; shift 2 ;;
            --cassert)   CASSERT=1; shift ;;
            --suite)     SUITE="$2"; shift 2 ;;
            --sanitize)  SANITIZE="$2"; shift 2 ;;
            --coverage)  COVERAGE=1; shift ;;
            --vendored)  VENDORED=1; shift ;;
            --no-strict) STRICT=0; shift ;;
            *)           die "unknown flag: $1" ;;
        esac
    done
}

# --------------------------------------------------------------------------
# Is each image still made of the files in this working tree?
#
# The failure this prevents has cost real debugging time twice, in both
# directions and with two different images:
#
#   `max_prepared_transactions=4` was added to docker/build-entrypoint.sh,
#   which is BAKED INTO the build image. Until `harness.sh images` was re-run,
#   every run used the old entrypoint and wbuf failed its empty-PREPARE clause
#   with "prepared transactions are disabled" - which reads exactly like a
#   code defect and is not one.
#
#   Three actions were added to the fault proxy and the container kept running
#   the old script. `arm no_resp3` answered +OK, the fired counter incremented,
#   and the command was forwarded untouched, so the suite asserted nothing and
#   said so in the shape of a wrong answer rather than an error.
#
# Neither is detectable from inside the container, and both LOOK like the
# code under test being wrong. So each image is stamped at build time with a
# digest of the files that went into it, and that stamp is checked before
# anything runs. Being told to rebuild is a diagnosis; the two failures above
# were not.
#
# cmd_images already handles the opposite direction - it drops the topology
# stamp so rebuilt images cannot be represented by containers that predate
# them.
image_sources() {
    case "$1" in
        build)      echo "docker/Dockerfile.build docker/build-entrypoint.sh" ;;
        cassert)    echo "docker/Dockerfile.cassert docker/build-entrypoint.sh" ;;
        faultproxy) echo "docker/Dockerfile.faultproxy docker/faultproxy/faultproxy.py" ;;
        valkey)     echo "docker/Dockerfile.valkey docker/valkey/gen-certs.sh" \
                         "docker/valkey/users.acl docker/valkey/entrypoint.sh" ;;
    esac
}

src_sha() {
    # shellcheck disable=SC2046
    cat $(image_sources "$1") 2>/dev/null | sha256sum | cut -c1-16
}

# Absent (an image built before stamping existed) is treated as stale rather
# than as fine: "I cannot tell" and "it matches" must not be the same answer.
check_image_fresh() {
    local kind="$1" image="$2" want have
    docker image inspect "$image" >/dev/null 2>&1 || return 0
    want="$(src_sha "$kind")"
    have="$(docker image inspect --format '{{index .Config.Labels "vfdw.src_sha"}}' \
            "$image" 2>/dev/null)"
    [ "$want" = "$have" ] && return 0
    die "$image was built from different sources than this tree ($have != $want).
    Its inputs are: $(image_sources "$kind")
    Run: $0 images --pg ${PG_MAJOR}"
}

# --------------------------------------------------------------------------
cmd_images() {
    say "building ${BUILD_IMAGE_PREFIX}:pg${PG_MAJOR} (libvalkey ${LIBVALKEY_REF})"
    docker build \
        -f docker/Dockerfile.build \
        --build-arg "PG_MAJOR=${PG_MAJOR}" \
        --build-arg "LIBVALKEY_REF=${LIBVALKEY_REF}" \
        --build-arg "UID=$(id -u)" \
        --build-arg "GID=$(id -g)" \
        --build-arg "VFDW_SRC_SHA=$(src_sha build)" \
        -t "$(build_image)" .

    # Built only on request: it compiles PostgreSQL from source, which is
    # twenty minutes, and it is a deliberate check rather than a default.
    # It had no build step here at all until CI needed one - it was made by
    # hand, which is precisely how a check stops being run.
    if [[ "${CASSERT:-0}" == "1" ]]; then
        say "building ${CASSERT_IMAGE_PREFIX}:pg${PG_MAJOR} (from source)"
        docker build -f docker/Dockerfile.cassert \
            --build-arg "PG_MAJOR=${PG_MAJOR}" \
            --build-arg "LIBVALKEY_REF=${LIBVALKEY_REF}" \
            --build-arg "UID=$(id -u)" \
            --build-arg "GID=$(id -g)" \
            --build-arg "VFDW_SRC_SHA=$(src_sha cassert)" \
            -t "${CASSERT_IMAGE_PREFIX}:pg${PG_MAJOR}" .
    fi

    say "building ${VALKEY_IMAGE_PREFIX}-faultproxy"
    docker build -f docker/Dockerfile.faultproxy \
        --build-arg "VFDW_SRC_SHA=$(src_sha faultproxy)" \
        -t "${VALKEY_IMAGE_PREFIX}-faultproxy:latest" .

    local kind
    for kind in plain bundle; do
        say "building $(valkey_image "$kind") from $(base_for_kind "$kind")"
        docker build \
            -f docker/Dockerfile.valkey \
            --build-arg "VALKEY_IMAGE=$(base_for_kind "$kind")" \
            --build-arg "VFDW_SRC_SHA=$(src_sha valkey)" \
            -t "$(valkey_image "$kind")" .
    done

    # Whatever is running was started from the images these just replaced, so
    # it no longer represents any topology. Forgetting this is how a rebuilt
    # server or proxy appears to have changed nothing.
    rm -f "$STATE_DIR/topology"
}

# --------------------------------------------------------------------------
ensure_net() {
    docker network inspect "$NET" >/dev/null 2>&1 || docker network create "$NET" >/dev/null
}

rm_container() { docker rm -f "$1" >/dev/null 2>&1 || true; }

#
# Remove every server container this harness owns, whichever topology left it.
#
# Topologies share network aliases: the fault proxy answers to "valkey" so
# that the FDW reaches it without knowing, and each cluster node answers to
# its own name. A container left behind by the previous topology therefore
# competes for a name the new one needs, Docker's DNS alternates between them,
# and roughly half the connections land somewhere they were never meant to -
# plaintext into a TLS port, resets mid-handshake. The failure looks like a
# bug in the code under test and is not one, so every topology starts from
# nothing.
#
rm_all_servers() {
    local c

    for c in $(docker ps -aq --filter "name=^${VK}"); do
        docker rm -f "$c" >/dev/null 2>&1 || true
    done
    rm_container vfdw-faultproxy
}

#
# How to reach this topology's server from inside its own container.
#
# Each one answers to something different: TLS needs the CA, and the ACL
# topology has no anonymous access at all - its default user is disabled, so
# an unauthenticated PING is answered with NOAUTH and a readiness probe that
# ignores that would wait out its timeout on a server that came up fine.
#
valkey_cli_args() {
    case "${1:-$TOPOLOGY}" in
        tls) echo "--tls --cacert /tls/ca.crt" ;;
        acl) echo "--user fdw_admin --pass admin_pw --no-auth-warning" ;;
        *)   echo "" ;;
    esac
}

#
# What the running topology must match for it to be reused.
#
# The name alone is not enough: changing how a topology is built - an added
# network alias, a different server flag - leaves the old containers running,
# because the name did not change. The fix then appears not to work, which is
# the same "a previous run leaks into this one" failure that the container
# teardown and the keyspace flush already address. Hashing this script closes
# the last way in.
#
topology_stamp() {
    printf '%s %s\n' "$1" "$(sha1sum "${BASH_SOURCE[0]}" | cut -d" " -f1)"
}

wait_ready() {
    local name="$1" tries=60
    local -a cli=(valkey-cli)

    # An explicit second argument overrides the topology default, for a node
    # this harness deliberately cannot verify.
    # shellcheck disable=SC2206
    cli+=(${2-$(valkey_cli_args)})

    while (( tries-- > 0 )); do
        if docker exec "$name" "${cli[@]}" ping 2>/dev/null | grep -q PONG; then
            return 0
        fi
        sleep 0.5
    done
    warn "--- $name logs ---"
    docker logs --tail 50 "$name" >&2 || true
    die "$name did not become ready"
}

cmd_up() {
    local topo="${1:-$TOPOLOGY}"
    TOPOLOGY="$topo"
    ensure_net

    local kind image
    kind="$(kind_for_topology "$topo")"
    image="$(valkey_image "$kind")"

    docker image inspect "$image" >/dev/null 2>&1 \
        || die "image $image missing; run: $0 images --valkey ${VALKEY_VERSION}"

    check_image_fresh valkey "$image"
    # build_image() swaps in the cassert image under --cassert, and that one
    # is built from a different Dockerfile, so the source set must follow it.
    if [[ "${CASSERT:-0}" == "1" ]]; then
        check_image_fresh cassert "$(build_image)"
    else
        check_image_fresh build "$(build_image)"
    fi
    [ "$topo" = "fault" ] && \
        check_image_fresh faultproxy "${VALKEY_IMAGE_PREFIX}-faultproxy:latest"

    rm_all_servers

    case "$topo" in
        standalone|acl|search)
            say "starting $VK ($topo, valkey ${VALKEY_VERSION})"
            docker run -d --name "$VK" --network "$NET" \
                --network-alias valkey "$image" "$topo" >/dev/null
            wait_ready "$VK"
            ;;

        tls)
            say "starting $VK (tls, valkey ${VALKEY_VERSION})"
            # valkey-othername resolves to the same server, but the server
            # certificate names only 'valkey'. That is the valid-CA,
            # wrong-host case - the one libvalkey's own TLS helper does not
            # catch, and the reason vfdw_tls.c exists.
            docker run -d --name "$VK" --network "$NET" \
                --network-alias valkey \
                --network-alias valkey-othername \
                "$image" tls >/dev/null
            wait_ready "$VK"

            # PostgreSQL runs in a different container and needs the CA to
            # verify against, so lift the generated material onto the host.
            rm -rf "$STATE_DIR/tls"
            docker cp "$VK:/tls" "$STATE_DIR/tls" >/dev/null
            chmod -R a+rX "$STATE_DIR/tls"

            # A second node serving a certificate that expired a month ago,
            # issued for its own name by the same CA. Readiness cannot be
            # checked the usual way - verifying it is exactly what must fail -
            # so the probe skips verification that the test then demands.
            say "starting ${VK}-expired (tls, expired certificate)"
            docker run -d --name "${VK}-expired" --network "$NET" \
                --network-alias valkey-expired \
                "$image" tls-expired >/dev/null
            wait_ready "${VK}-expired" "--tls --cacert /tls/ca.crt --insecure"
            ;;

        fault)
            say "starting $VK behind faultproxy (valkey ${VALKEY_VERSION})"
            docker run -d --name "$VK" --network "$NET" \
                --network-alias valkey-upstream "$image" standalone >/dev/null
            TOPOLOGY=standalone wait_ready "$VK"
            docker run -d --name vfdw-faultproxy --network "$NET" \
                --network-alias valkey \
                "${VALKEY_IMAGE_PREFIX}-faultproxy:latest" \
                --upstream-host valkey-upstream >/dev/null
            # The proxy is ready once its control port answers.
            local tries=60
            while (( tries-- > 0 )); do
                if docker exec vfdw-faultproxy python3 -c \
                    "import socket;socket.create_connection(('127.0.0.1',6390),1)" \
                    2>/dev/null; then break; fi
                sleep 0.5
            done
            (( tries > 0 )) || die "faultproxy control port never opened"
            ;;

        cluster)
            local i ips=()
            say "starting 6-node cluster (valkey ${VALKEY_VERSION})"
            for i in 1 2 3 4 5 6; do
                # Node 1 also answers to the plain name every other topology
                # uses, so a suite that only wants "a reachable server" needs
                # no special case. Reaching one node of a cluster is all this
                # wrapper can do until Phase 4 teaches it about slots.
                local -a alias=(--network-alias "valkey-${i}")
                [[ "$i" == 1 ]] && alias+=(--network-alias valkey)

                docker run -d --name "${VK}-${i}" --network "$NET" \
                    "${alias[@]}" "$image" cluster >/dev/null
            done
            for i in 1 2 3 4 5 6; do wait_ready "${VK}-${i}"; done

            for i in 1 2 3 4 5 6; do
                ips+=("$(docker inspect -f \
                    "{{(index .NetworkSettings.Networks \"${NET}\").IPAddress}}" \
                    "${VK}-${i}"):6379")
            done

            say "forming cluster: 3 primaries, 1 replica each"
            docker exec "${VK}-1" valkey-cli --cluster create "${ips[@]}" \
                --cluster-replicas 1 --cluster-yes >/dev/null

            # Wait for slot coverage before declaring the topology up.
            local tries=60
            while (( tries-- > 0 )); do
                if docker exec "${VK}-1" valkey-cli cluster info \
                    | grep -q 'cluster_state:ok'; then break; fi
                sleep 0.5
            done
            (( tries > 0 )) || die "cluster did not reach state ok"
            ;;

        *)
            die "unknown topology: $topo"
            ;;
    esac

    topology_stamp "$topo" > "$STATE_DIR/topology"

    # Marked here as well as after a reset, because not everything that erases
    # a keyspace runs a reset first - `bench` does not - and an unmarked server
    # refuses the flush its fixtures expect.
    mark_disposable

    say "topology '$topo' up on network $NET"
}

cmd_down() {
    rm_all_servers
    docker network rm "$NET" >/dev/null 2>&1 || true
    rm -f "$STATE_DIR/topology"
    say "topology down"
}

# --------------------------------------------------------------------------
# Run a command inside the build container, with PostgreSQL already up.
# --------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Objects belong to exactly one PostgreSQL major.
#
# The build is a bind mount, so src/*.o live in the working tree and outlast
# the container. Nothing in a plain `make` knows which major produced them, so
# running --pg 17 and then --pg 18 with no source change leaves make with
# objects newer than every source: it skips compiling and links PG 17 objects
# into a PG 18 library. That loads and then fails at CREATE EXTENSION with an
# undefined symbol - or, for a symbol that happens to exist in both, does not
# fail at all and quietly tests the wrong thing.
#
# A stamp beside the objects is the cheapest thing that cannot be got wrong.
# ---------------------------------------------------------------------------
ensure_major_objects() {
    local stamp="$STATE_DIR/built_pg"

    mkdir -p "$STATE_DIR"
    local want="${PG_MAJOR}${CASSERT:+-cassert}"

    if [[ -f "$stamp" && "$(cat "$stamp")" == "$want" ]]; then
        return 0
    fi

    if [[ -f "$stamp" ]]; then
        say "objects were built for pg$(cat "$stamp"); rebuilding for pg${want}"
    fi

    find "$REPO_ROOT" \( -name '*.o' -o -name '*.bc' -o -name '*.d' \
                         -o -name '*.so' -o -name '*.gcno' -o -name '*.gcda' \) \
         -not -path '*/.git/*' -delete 2>/dev/null || true

    echo "$want" > "$stamp"
}

run_in_build() {
    local image
    image="$(build_image)"
    docker image inspect "$image" >/dev/null 2>&1 \
        || die "image $image missing; run: $0 images --pg ${PG_MAJOR}"

    ensure_major_objects

    ensure_net
    local -a tls_mount=()
    [[ -d "$STATE_DIR/tls" ]] && tls_mount=(-v "$STATE_DIR/tls:/tls:ro")

    docker run --rm -i \
        --name "vfdw-build-$$" \
        --network "$NET" \
        -v "$REPO_ROOT:/work" \
        "${tls_mount[@]}" \
        -e "VALKEY_HOST=${VALKEY_HOST:-valkey}" \
        -e "VALKEY_PORT=${VALKEY_PORT:-6379}" \
        -e "VFDW_TOPOLOGY=${TOPOLOGY}" \
        -e "STRICT=${STRICT}" \
        -e "COVERAGE=${COVERAGE}" \
        -e "SANITIZE=${SANITIZE}" \
        -e "VALKEY_VENDORED=${VENDORED}" \
        -e "VFDW_NO_POSTGRES=${VFDW_NO_POSTGRES:-0}" \
        "$image" bash -lc "$1"
}

# Object files are left in the bind mount for a fast edit/build loop, but they
# are not portable across PostgreSQL majors. Clean when the major changes.
maybe_clean() {
    local last=""
    [[ -f "$STATE_DIR/last-pg" ]] && last="$(cat "$STATE_DIR/last-pg")"
    if [[ "$last" != "${PG_MAJOR}:${VENDORED}:${SANITIZE}:${COVERAGE}" ]]; then
        say "build configuration changed (was '${last:-none}'), cleaning"
        run_in_build "make clean >/dev/null 2>&1 || true"
        echo "${PG_MAJOR}:${VENDORED}:${SANITIZE}:${COVERAGE}" > "$STATE_DIR/last-pg"
    fi
}

#
# Build the pinned libvalkey, if this run wants it linked in.
#
# INSIDE the build image, not on the host: the archive is linked into the
# extension, so it has to come from the same toolchain and libc. Called from
# every path that compiles rather than from cmd_build alone - cmd_test and
# friends each run their own make, which is why putting it in cmd_build fixed
# nothing and the link still failed on a missing .a.
#
ensure_vendored_libvalkey() {
    [[ "$VENDORED" == "1" ]] || return 0
    VFDW_NO_POSTGRES=1 run_in_build \
        "LIBVALKEY_REF=${LIBVALKEY_REF} bash scripts/fetch-libvalkey.sh"
}

cmd_build() {
    maybe_clean
    say "building valkey_fdw (pg${PG_MAJOR}, vendored=${VENDORED}, sanitize='${SANITIZE:-none}')"

    # The vendored library is built INSIDE the build image, not on the host:
    # it is linked into the extension, so it has to come from the same
    # toolchain and libc the extension is compiled with.
    ensure_vendored_libvalkey
    VFDW_NO_POSTGRES=1 run_in_build "make -j\$(nproc) && make install"
}

#
# Build, install and test in a single container.
#
# These cannot be separate `docker run` invocations: `make install` writes
# into the image's PostgreSQL directories, which are discarded when the
# container exits, so a later container would find no extension installed.
#
#
# Start the run from a known Valkey.
#
# pg_regress gives each run a fresh database; without the same guarantee on
# the Valkey side a run inherits whatever the previous one left behind, and a
# suite that reads the keyspace quietly measures history instead of its own
# fixture. Best-effort: a topology whose server is not reachable this way
# still runs, it just does not get the guarantee.
#
# Mark this server as one this harness created and may erase.
#
# Through `docker exec` into a container started here by name, which is the
# point: it is a claim about the SERVER rather than about a hostname, and a
# hostname is what $VALKEY_HOST would redirect. valkey_fdw_test_flush refuses
# to FLUSHDB anything without this mark, so a bench run pointed at a real cache
# stops instead of emptying it.
#
# IN DATABASE 15, which the fixtures do not use. FLUSHDB empties one database,
# so a mark kept beside the data would be erased by the first fixture that
# flushed - including the ones that still flush through valkey-cli rather than
# through the guard - and every guarded flush after it would refuse. Kept a
# database away, the mark outlives the keyspace it vouches for.
#
# FLUSHALL would take it, and nothing but reset_valkey issues one; that path
# marks again immediately afterwards.
#
# Best effort. A topology whose server will not take the mark still runs; what
# it loses is the ability to flush from inside a fixture, which fails loudly
# there rather than quietly here.
mark_disposable() {
    local -a cli=(valkey-cli)

    # shellcheck disable=SC2206
    cli+=($(valkey_cli_args))
    docker exec "$VK" "${cli[@]}" -n 15 SET valkey_fdw:disposable yes >/dev/null 2>&1 || true
}

reset_valkey() {
    local out
    local -a cli=(valkey-cli)

    # shellcheck disable=SC2206
    cli+=($(valkey_cli_args))

    if out="$(docker exec "$VK" "${cli[@]}" FLUSHALL 2>&1)" && [[ "$out" == OK* ]]; then
        mark_disposable
        return 0
    fi
    say "could not flush ${VK}; suites will see whatever it already holds"
}

#
# FLUSHALL EMPTIES A KEYSPACE; IT DOES NOT MOVE A SLOT BACK.
#
# The cluster suite migrates a hash slot for real and migrates it back, so its
# fixture is the topology itself and not just the keys in it. A run that fails
# anywhere between those two points leaves the slot where it was moved to, and
# because a matching topology stamp means the containers are reused, the next
# run starts from that and asserts against a cluster it did not build.
#
# What that costs is not a confusing failure, it is a CONFIDENT WRONG ANSWER.
# Bisecting a cluster failure across four commits produced a clean result -
# two good, two bad, a plausible culprit - and every one of those runs was
# measuring the previous run's slot map rather than the commit it had checked
# out. Re-forming before each one showed the same assertion failing on master.
#
# So the topology whose suite rewrites it is the topology that is never
# reused. It costs one cluster formation per run and buys the ability to
# believe the result.
#
topology_is_disposable() {
    [[ "$1" == cluster ]]
}

# ---------------------------------------------------------------------------
# Isolation specs: the only place two concurrent sessions can be arranged.
#
# Standalone only, and deliberately not part of cmd_test. The specs need a
# plaintext reachable server and say nothing topology-specific, so running
# them per topology would be five repeats of one answer plus five failures.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Remove build products from the source tree.
#
# The build is a bind mount, so every .o, .bc, .d and the .so land in the
# working directory rather than in the container. They are all gitignored, so
# nothing is ever committed - but a stale one of them linked against a changed
# header is what produced a segfault that took several rounds to find, so
# having a one-word way to be sure they are gone is worth the twenty lines.
# ---------------------------------------------------------------------------
cmd_clean() {
    say "removing build products (pg${PG_MAJOR})"
    run_in_build "make clean >/dev/null 2>&1 || true" || true

    # make clean runs as the container's user and covers what the Makefile
    # knows about; these are the leftovers it does not.
    rm -rf "$REPO_ROOT"/test/regress/results \
           "$REPO_ROOT"/test/regress/regression.{diffs,out} \
           "$REPO_ROOT"/test/isolation/{output_iso,results} \
           "$REPO_ROOT"/test/isolation/regression.{diffs,out} \
           "$REPO_ROOT"/coverage "$STATE_DIR/built_pg" 2>/dev/null || true

    local left
    left=$(find "$REPO_ROOT" \( -name '*.o' -o -name '*.bc' -o -name '*.d' \
                                -o -name '*.so' -o -name '*.gcno' -o -name '*.gcda' \) \
                -not -path '*/.git/*' 2>/dev/null | wc -l)
    if (( left > 0 )); then
        say "warning: ${left} build products remain; removing them directly"
        find "$REPO_ROOT" \( -name '*.o' -o -name '*.bc' -o -name '*.d' \
                             -o -name '*.so' -o -name '*.gcno' -o -name '*.gcda' \) \
             -not -path '*/.git/*' -delete 2>/dev/null || true
    fi

    say "source tree clean"
}

# ---------------------------------------------------------------------------
# TAP tests: the proofs that need a second session.
#
# These build their own cluster with PostgreSQL::Test::Cluster rather than
# using the entrypoint's, because a TAP test wants to start and stop a server
# it controls. The extension is installed into the shared libdir first, so the
# private cluster can CREATE EXTENSION; Valkey is reached over the docker
# network exactly as every other suite reaches it.
#
# Standalone only, for the same reason as the isolation specs: the properties
# are about interrupt handling, not about transport.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Flush latency, which is what write_max_ops actually bounds.
#
# Not a pass/fail suite: it prints numbers and a human reads them. Kept out of
# cmd_test for that reason - a benchmark whose result is "it did not crash" is
# a slow test, not a measurement.
# ---------------------------------------------------------------------------
cmd_bench() {
    if [[ ! -f "$STATE_DIR/topology" \
          || "$(cat "$STATE_DIR/topology")" != "$(topology_stamp standalone)" ]]; then
        cmd_up standalone
    fi

    reset_valkey

    say "flush latency (pg${PG_MAJOR}, valkey ${VALKEY_VERSION})"
    run_in_build "
        set -e
        make -j\$(nproc) >/dev/null
        make install >/dev/null
        bash test/bench/flush_latency.sh
        echo
        echo '=== overlay scan cost ==='
        bash test/bench/overlay_scan.sh
    "
}

#
# The in-container half of a TAP run, shared by `test` and by `tap`.
#
# One text, because the two callers must not be able to drift into running
# different things: `tap` is how the properties are exercised on their own
# while a cancel path is being worked on, and `test` is what decides whether
# the tree is green. If those diverged, the second would stop being an answer
# about the first.
#
# The caller has already built and installed. Nothing here starts a server:
# PostgreSQL::Test::Cluster builds its own, which is the whole reason these
# live outside pg_regress.
#
tap_fragment() {
    cat <<'TAPSH'

        echo '--- tap ---'

        # A previous run's data directory bails the whole file out before the
        # first assertion, so it goes before anything else.
        rm -rf test/tap/tmp_check test/tap/log tmp_check

        n=$(ls test/tap/t/*.pl 2>/dev/null | wc -l)
        if [ "$n" -eq 0 ]; then
            echo 'no TAP tests found in test/tap/t' >&2
            exit 1
        fi
        echo "running $n TAP file(s)"

        # REGRESS and ISOLATION emptied so installcheck runs the TAP tests
        # alone. prove exits 0 when it finds nothing, so the count above is
        # what makes an empty run a failure rather than a silent pass.
        if ! make installcheck TAP_TESTS=1 REGRESS='' ISOLATION=''; then
            for f in tmp_check/log/*; do
                echo "--- $f ---"; tail -40 "$f"
            done 2>/dev/null || true
            exit 1
        fi
TAPSH
}

cmd_tap() {
    if [[ "$TOPOLOGY" != "standalone" ]]; then
        die "tap runs on the standalone topology only (got: $TOPOLOGY)"
    fi

    if [[ ! -f "$STATE_DIR/topology" \
          || "$(cat "$STATE_DIR/topology")" != "$(topology_stamp standalone)" ]]; then
        cmd_up standalone
    fi

    reset_valkey

    say "tap tests (pg${PG_MAJOR}, valkey ${VALKEY_VERSION})"
    run_in_build "
        set -e
        make -j\$(nproc)
        make install
$(tap_fragment)
    "

    say "tap tests passed (pg${PG_MAJOR})"
}

cmd_isolation() {
    if [[ "$TOPOLOGY" != "standalone" ]]; then
        die "isolation runs on the standalone topology only (got: $TOPOLOGY)"
    fi

    if [[ ! -f "$STATE_DIR/topology" \
          || "$(cat "$STATE_DIR/topology")" != "$(topology_stamp standalone)" ]]; then
        cmd_up standalone
    fi

    reset_valkey

    say "isolation specs (pg${PG_MAJOR}, valkey ${VALKEY_VERSION})"
    run_in_build "
        set -e
        make -j\$(nproc)
        make install
        if ! make installcheck REGRESS='' ISOLATION='${ISOLATION_SPEC:-write_conflict}'; then
            echo '--- isolation output.diffs ---'
            cat test/isolation/output_iso/regression.diffs 2>/dev/null \
                || cat test/isolation/regression.diffs 2>/dev/null || true
            exit 1
        fi
    "

    say "isolation specs passed (pg${PG_MAJOR})"
}

cmd_test() {
    local topo="${TOPOLOGY}"

    if topology_is_disposable "$topo" \
       || [[ ! -f "$STATE_DIR/topology" \
             || "$(cat "$STATE_DIR/topology")" != "$(topology_stamp "$topo")" ]]; then
        cmd_up "$topo"
    fi

    reset_valkey
    maybe_clean

    #
    # Whether the caller named a suite, asked BEFORE the default is filled in.
    #
    # `test --suite scan` is someone iterating on one file, and dragging a
    # two-session cancel proof through every such run would make the fast loop
    # slow enough to be worked around. The TAP tests therefore join a full run
    # and not a targeted one.
    #
    local explicit_suite=0
    [[ -n "$SUITE" ]] && explicit_suite=1

    local regress_arg=""
    [[ -z "$SUITE" ]] && SUITE="$(suites_for_topology "$topo")"
    [[ -n "$SUITE" ]] && regress_arg="REGRESS='${SUITE}'"

    #
    # The TAP tests are part of a full standalone run.
    #
    # They prove what no pg_regress file can: a query cancelled from another
    # session, which needs two sessions and a server this harness starts and
    # stops itself. Leaving them to a subcommand of their own meant `test`
    # could be green while they were not, and they went unrun long enough for
    # one of them to stop being able to pass at all - so "green locally" and
    # "green in CI" were only ever the same statement for whoever remembered
    # the second command.
    #
    # Standalone only, and said out loud everywhere else. The properties are
    # about interrupt handling rather than transport, so tls, acl, fault,
    # cluster and search have nothing to add - but a run that quietly covers
    # less than the last one is how a suite stops being noticed, so the
    # topologies that skip them say so.
    #
    local tap_part=""
    if (( explicit_suite )); then
        :
    elif [[ "$topo" == "standalone" ]]; then
        tap_part="$(tap_fragment)"
    else
        say "tap tests not run on '${topo}': they assert interrupt handling, which is transport-independent - run them on standalone"
    fi

    ensure_vendored_libvalkey

    say "build + install + test (pg${PG_MAJOR}, valkey ${VALKEY_VERSION}, ${topo})"
    run_in_build "
        set -e
        echo '--- build ---'
        make -j\$(nproc)
        make install
        echo '--- regression ---'
        if ! make installcheck ${regress_arg}; then
            echo '--- regression.diffs ---'
            cat test/regress/regression.diffs 2>/dev/null || true
            exit 1
        fi
        echo '--- unit ---'
        bash test/unit/run.sh
${tap_part}
    "

    say "all suites passed (pg${PG_MAJOR}, valkey ${VALKEY_VERSION}, ${topo})"
}

#
# Bootstrap or refresh expected output from an actual run.
#
# Expected files are never written by hand; they are recorded from real
# output and then reviewed in the diff.
#
cmd_record() {
    local topo="${TOPOLOGY}"
    if topology_is_disposable "$topo" \
       || [[ ! -f "$STATE_DIR/topology" \
             || "$(cat "$STATE_DIR/topology")" != "$(topology_stamp "$topo")" ]]; then
        cmd_up "$topo"
    fi
    reset_valkey
    maybe_clean

    local regress_arg=""
    [[ -z "$SUITE" ]] && SUITE="$(suites_for_topology "$topo")"
    [[ -n "$SUITE" ]] && regress_arg="REGRESS='${SUITE}'"

    warn "recording expected output - review the resulting diff before committing"
    rm -rf "$REPO_ROOT/test/regress/results"
    run_in_build "
        set -e
        make -j\$(nproc)
        make install
        make installcheck ${regress_arg} || true
        for f in test/regress/results/*.out; do
            [ -e \"\$f\" ] || continue
            cp \"\$f\" \"test/regress/expected/\$(basename \"\$f\")\"
            echo \"recorded \$(basename \"\$f\")\"
        done
    "
}

# Standalone spike programs: small C probes that answer an open design
# question against a live server. They link libvalkey directly and need no
# PostgreSQL, so a failure points squarely at the client library.
cmd_spike() {
    local topo="${TOPOLOGY}"
    if topology_is_disposable "$topo" \
       || [[ ! -f "$STATE_DIR/topology" \
             || "$(cat "$STATE_DIR/topology")" != "$(topology_stamp "$topo")" ]]; then
        cmd_up "$topo"
    fi
    say "running spikes against ${topo}"
    VFDW_NO_POSTGRES=1 run_in_build "
        set -e
        for src in test/spike/*.c; do
            out=/tmp/\$(basename \"\$src\" .c)
            echo \"== \$(basename \$src) ==\"
            gcc -Wall -Wextra -Werror -O1 -g -o \"\$out\" \"\$src\" -lvalkey
            \"\$out\" \"\${VALKEY_HOST}\" \"\${VALKEY_PORT}\"
        done
    "
}

cmd_shell() {
    say "interactive build container (pg${PG_MAJOR})"
    local image; image="$(build_image)"
    ensure_net
    docker run --rm -it --network "$NET" \
        -v "$REPO_ROOT:/work" \
        -e "VALKEY_HOST=${VALKEY_HOST:-valkey}" \
        "$image" bash
}

# The self-test runs first and its failure is this command's failure. A
# banned-construct pattern that has stopped matching reports the tree clean,
# and a clean report from a gate that never searched is indistinguishable from
# a tree that really is clean - so the gate is asked to prove it can still fire
# before its verdict on src/ is worth reading.
cmd_lint() {
    VFDW_NO_POSTGRES=1 run_in_build "bash scripts/lint.sh --selftest && bash scripts/lint.sh"
}

# Coverage gate. 85% overall, deliberately above the 80% a line-count habit
# settles on, because the lines a suite misses are not spread evenly: they are
# the error paths, and an untested error path in a wrapper is how a query comes
# back short with no error attached rather than how it crashes. vfdw_conn.c,
# vfdw_io.c and vfdw_cmd.c are held to 95% - everything those three layers can
# do wrong IS an error path, so a gap there is not a gap in a corner. gcovr has
# no per-file threshold, so only the overall figure can fail this command; the
# 95% is read off the per-file rows of coverage/index.html.
cmd_coverage_report() {
    local overall_min=85
    VFDW_NO_POSTGRES=1 run_in_build "
        set -e
        mkdir -p coverage
        gcovr --root . --filter 'src/.*' \
              --html-details coverage/index.html \
              --print-summary --fail-under-line ${overall_min} \
        | tee coverage/summary.txt
    "
}

cmd_ci() {
    local pg vk failures=0
    for pg in "${CI_PG_MAJORS[@]}"; do
        for vk in "${CI_VALKEY_VERSIONS[@]}"; do
            PG_MAJOR="$pg" VALKEY_VERSION="$vk"
            say "=== pg${pg} x valkey ${vk} ==="
            cmd_images
            for TOPOLOGY in standalone tls acl cluster search fault; do
                cmd_up "$TOPOLOGY"
                if ! cmd_test; then
                    warn "FAILED: pg${pg} valkey${vk} ${TOPOLOGY}"
                    failures=$((failures + 1))
                fi
            done
            cmd_down
        done
    done
    (( failures == 0 )) || die "${failures} matrix cell(s) failed"
    say "full matrix green"
}

# --------------------------------------------------------------------------
main() {
    local sub="${1:-help}"
    shift || true

    case "$sub" in
        images) parse_flags "$@"; cmd_images ;;
        up)     local t="${1:-}"; [[ -n "$t" && "$t" != --* ]] && { TOPOLOGY="$t"; shift; }
                parse_flags "$@"; cmd_up "$TOPOLOGY" ;;
        down)   cmd_down ;;
        build)  parse_flags "$@"; cmd_build ;;
        test)   parse_flags "$@"; cmd_test ;;
        isolation) parse_flags "$@"; cmd_isolation ;;
        tap)    parse_flags "$@"; cmd_tap ;;
        bench)  parse_flags "$@"; cmd_bench ;;
        clean)  parse_flags "$@"; cmd_clean ;;
        record) parse_flags "$@"; cmd_record ;;
        mutate) parse_flags "$@"
                python3 scripts/mutate.py --pg "${PG_MAJOR}" ;;
        lint)   parse_flags "$@"; cmd_lint ;;
        spike)  parse_flags "$@"; cmd_spike ;;
        coverage-report) parse_flags "$@"; cmd_coverage_report ;;
        shell)  parse_flags "$@"; cmd_shell ;;
        ci)     parse_flags "$@"; cmd_ci ;;
        help|-h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            ;;
        *) die "unknown subcommand: $sub (try: help)" ;;
    esac
}

main "$@"
