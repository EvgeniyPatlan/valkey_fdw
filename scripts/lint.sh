#!/usr/bin/env bash
#
# Structural limits, enforced rather than intended.
#
# A three-thousand-line file with a 446-line function cannot be reviewed: no
# reader holds that much at once, so its defects are found by their symptoms
# rather than by reading. That review cost is what this exists to prevent.
# Runs inside the build container via `harness.sh lint`.
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

MAX_FILE_LINES=800
MAX_FUNC_LINES=60
status=0

fail() { printf '\033[1;31m[lint]\033[0m %s\n' "$*" >&2; status=1; }

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
note() { printf '\033[1;36m[lint]\033[0m %s\n' "$*" >&2; }

# --- banned constructs ----------------------------------------------------
# Each of these is banned because of a specific defect it has caused, not on
# style grounds.
#
# The ereport rule covers errdetail, errhint and errcontext as well as errmsg,
# because invariant I2 is about server-supplied bytes reaching ANY of them: a
# Valkey error message carrying a '%' is just as dangerous in a DETAIL as in a
# MESSAGE. It matches member access too (errdetail(buf.data)), which the
# errmsg-only form this replaced let through - that spelling was stopped only
# by -Werror=format-security, so the gate was claiming coverage the compiler
# was actually providing.
#
# Whitespace is spelled [[:space:]] rather than \s throughout. \s is a GNU
# extension, and this gate has to give the same answer in every container the
# project builds in; a pattern that silently matches nothing under a different
# grep is worth less than no pattern at all.
#
declare -A BANNED=(
    ["\\batoi[[:space:]]*\\("]="atoi() silently yields 0 on garbage; use vfdw_parse_int()"
    ["\\bsprintf[[:space:]]*\\("]="unbounded sprintf; use snprintf or psprintf"
    ["\\bstrcat[[:space:]]*\\("]="unbounded strcat; use StringInfo"
    ["\\berr(msg|detail|hint|context)[[:space:]]*\\([[:space:]]*[a-z_][A-Za-z0-9_.>-]*[[:space:]]*[,)]"]="non-literal ereport format string; pass the text as an argument to \"%s\""
)

HASH_ENTER_WHY="HASH_ENTER with no foundPtr: the caller cannot then tell a new entry from an existing one, and dynahash writes nothing but the key - every other field of a new entry is read uninitialised"

# --- HASH_ENTER with no foundPtr -------------------------------------------
#
# Checked on JOINED STATEMENTS, not on lines, and that is the whole reason it
# is not in the table above. Every pattern there is a single token, and a line
# holds one. This one is a relationship between two arguments of one call, and
# hash_search's arguments wrap as a matter of course - the tree's own calls
# wrap in three different places.
#
# A line-at-a-time pattern for it passes its fixture and misses the code: put
# HASH_ENTER and the NULL on separate lines and it sees neither. That is a
# gate reporting coverage it does not have, which is the exact failure the
# self-test below exists to catch, so it may not be reintroduced here in the
# name of keeping the table uniform.
#
# Statements are accumulated until a ';' and collapsed to one line. The
# reported number is the line the statement STARTED on, which is where a
# reader looks.
scan_hash_enter() {
    local dir="$1" f hit

    while IFS= read -r f; do
        while IFS= read -r hit; do
            [[ -n "$hit" ]] || continue
            fail "${hit%%:*}: ${HASH_ENTER_WHY}"
            fail "    ${hit}"
        done < <(awk -v file="$f" '
            {
                line = $0
                sub(/^[[:space:]]+/, "", line)
                if (buf == "") { start = FNR; buf = line }
                else           { buf = buf " " line }

                if (index(buf, ";") > 0) {
                    if (buf ~ /HASH_ENTER/ &&
                        buf ~ /,[[:space:]]*(\([[:space:]]*bool[[:space:]]*\*[[:space:]]*\)[[:space:]]*)?NULL[[:space:]]*\)/)
                        printf "%s:%d: %s\n", file, start, buf
                    buf = ""
                }
            }
        ' "$f")
    done < <(find "$dir" \( -name '*.c' -o -name '*.h' \) 2>/dev/null)
}

# grep has three exit statuses and only two of them are an answer: 0 found
# something, 1 found nothing, 2 could not look - a pattern that is not a valid
# regular expression, a tree that cannot be read. `if grep ...` puts 1 and 2
# in the same branch, so a check written that way reports the source clean
# when what actually happened is that it never searched a single byte. That is
# how three of the patterns above were able to spend their entire life
# rejected by grep -E for an unterminated group while this gate passed every
# build. The status is read into a variable and the three cases kept apart.
scan_banned() {
    local dir="$1" pattern hits hit rc

    for pattern in "${!BANNED[@]}"; do
        hits=$(grep -rnE "$pattern" "$dir" 2>/dev/null)
        rc=$?
        if (( rc > 1 )); then
            fail "lint pattern is not a valid regular expression: ${pattern}"
            continue
        fi
        (( rc == 0 )) || continue
        while IFS= read -r hit; do
            fail "${hit%%:*}: ${BANNED[$pattern]}"
            fail "    ${hit}"
        done <<< "$hits"
    done
}

# A gate that cannot fire is a broken gate, and reading the patterns is not how
# that gets found: the unterminated groups survived every reading of this file,
# because an unescaped '(' in a pattern meant to match a function call looks
# exactly like the call it is meant to match. The only evidence that a pattern
# still works is a match. So --selftest runs the same loop over a directory
# holding one instance of every banned construct and demands that each pattern
# find its own; one that matches nothing there has stopped working whatever it
# looks like, and the tree it calls clean means nothing.
selftest_banned() {
    local dir=test/lint/fixtures
    local pattern hits rc

    [[ -d $dir ]] || { fail "self-test fixtures are missing: ${dir}"; return; }

    for pattern in "${!BANNED[@]}"; do
        hits=$(grep -rnE "$pattern" "$dir" 2>/dev/null)
        rc=$?
        if (( rc > 1 )); then
            fail "lint pattern is not a valid regular expression: ${pattern}"
        elif (( rc == 1 )); then
            fail "lint pattern matches nothing in ${dir} and can no longer fire: ${pattern}"
        else
            note "self-test: $(wc -l <<< "$hits") hit(s) for ${pattern}"
        fi
    done

    #
    # The joined-statement check gets the same demand, and the fixture holds
    # every spelling it has to survive: on one line, wrapped before the
    # action, wrapped between the action and the argument, and with the NULL
    # cast. A line-at-a-time pattern passes the first and fails the rest,
    # which is what this count is here to notice.
    #
    # Run inside a command substitution on purpose: scan_hash_enter reports
    # through fail(), and here its hits are the expected result rather than a
    # violation. The subshell is what keeps that status from leaking out and
    # failing the self-test with its own fixture.
    #
    local want=4 got
    got=$(scan_hash_enter "$dir" 2>&1 | grep -c "${HASH_ENTER_WHY}")
    if (( got == want )); then
        note "self-test: the HASH_ENTER check finds all ${want} spellings"
    else
        fail "the HASH_ENTER check found ${got} of ${want} spellings in ${dir};" \
             "a wrapped call is hiding from it"
    fi
}

case "${1-}" in
    --selftest)
        selftest_banned
        (( status == 0 )) && note "self-test: all ${#BANNED[@]} banned-construct patterns fire"
        exit $status
        ;;
    "")
        ;;
    *)
        fail "unknown argument: ${1}"
        exit 1
        ;;
esac

scan_banned src
scan_hash_enter src

# --- file length ----------------------------------------------------------
while IFS= read -r f; do
    lines=$(wc -l < "$f")
    if (( lines > MAX_FILE_LINES )); then
        fail "$f: ${lines} lines exceeds ${MAX_FILE_LINES}"
    fi
done < <(find src -name '*.c' -o -name '*.h' 2>/dev/null)

# --- function length ------------------------------------------------------
# Relies on the PostgreSQL C style this project follows: a definition opens
# with '{' in column 1 and closes with '}' in column 1.
while IFS= read -r f; do
    awk -v file="$f" -v maxl="$MAX_FUNC_LINES" '
        /^[{]/ { start = NR; name = prev; next }
        /^[}]/ {
            if (start > 0) {
                len = NR - start + 1
                if (len > maxl)
                    printf "%s:%d: function %s is %d lines (max %d)\n",
                           file, start, name, len, maxl
                start = 0
            }
            next
        }
        { if ($0 !~ /^[[:space:]]*$/ && $0 !~ /^[[:space:]]/) prev = $0 }
    ' "$f" > "$tmp"
    # Read from a redirect, not a pipe: a piped `while` runs in a subshell,
    # so every `fail` it recorded would be discarded and the gate would
    # report violations and still exit 0.
    while IFS= read -r line; do
        [[ -n "$line" ]] && fail "$line"
    done < "$tmp"
done < <(find src -name '*.c' 2>/dev/null)

# --- frozen shell-out fixtures ---------------------------------------------
#
# Shell-out fixtures are frozen. valkey_fdw_test_probe replaces them: it is
# binary safe, it distinguishes nil from empty array, it cannot report success
# for a command that failed, and it works on tls, acl and cluster where a
# plaintext valkey-cli cannot. Seven files are listed below and a new one is a
# regression. Four of them - scan, io, leak and acl - predate the probe and are
# left converted-in-place-only.
#
# The other three are the fault-topology suites, here permanently rather than
# pending conversion: they drive the proxy's CONTROL port, which speaks its own
# line protocol and is not a Valkey server at all, so there is no probe to use.
ALLOWED_PROGRAM_FIXTURES="scan.sql io.sql leak.sql fault.sql acl.sql wfault.sql resp.sql"

while IFS= read -r f; do
    # Everything from '--' to end of line is a comment and executes nothing, so
    # it is removed before matching. Stripping only WHOLE comment lines was
    # wrong in both directions at once: a trailing '-- ... FROM PROGRAM ...'
    # explaining why a file avoids this tripped the gate, while the SQL forms
    # that actually run - lowercase, or two spaces before PROGRAM - walked
    # straight past a fixed-string 'FROM PROGRAM' match. Case and inner
    # whitespace are both free in SQL and neither may be a way through.
    #
    # The match is on FROM PROGRAM rather than on COPY, because the two are
    # routinely on different lines and grep works a line at a time.
    #
    # The status is read rather than branched on directly, for the reason
    # given beside the banned-construct loop: a pattern this one's shape can
    # be edited into something grep rejects, and `|| continue` would then skip
    # every file in silence and leave the whole gate reporting nothing wrong.
    # PIPESTATUS[1] and not $?, because under pipefail the pipeline's status
    # can be sed dying of SIGPIPE when grep -q leaves early on a match.
    sed 's/--.*$//' "$f" | grep -Eqi 'FROM[[:space:]]+PROGRAM'
    rc=${PIPESTATUS[1]}
    if (( rc > 1 )); then
        fail "$f: the FROM PROGRAM pattern could not be run (grep exit ${rc})"
        continue
    fi
    (( rc == 0 )) || continue
    case " $ALLOWED_PROGRAM_FIXTURES " in
        *" $(basename "$f") "*) continue ;;
    esac
    fail "$f: COPY ... FROM PROGRAM in a new suite; use valkey_fdw_test_probe"
done < <(find test/regress/sql -name '*.sql' 2>/dev/null)

# ---------------------------------------------------------------------------
# W1: the script's write verbs must be LITERAL in the source.
#
# Phase 2 dispatches through closures that name their verb as a string
# constant. A script that took the verb from ARGV would be shorter, would work
# identically, and would make this gate vacuous - there would be no write verb
# in the file to find. So the gate is not "does the script look right", it is
# "is the property this gate depends on still true".
#
# Checked as a count, not a presence: one surviving literal SET would satisfy
# a grep for "is there a write verb" while the other twelve had been
# refactored into data.
# ---------------------------------------------------------------------------
if [[ -f src/vfdw_script.c ]]; then
    want_verbs="SET DEL HSET HDEL SADD SREM ZADD ZREM RPUSH LREM RENAME"
    for v in $want_verbs; do
        if ! grep -q "server.pcall('${v}'" src/vfdw_script.c; then
            fail "src/vfdw_script.c: write verb ${v} is not a literal in a server.pcall (invariant W1)"
        fi
    done

    # The read half of phase 1 must never call a write verb, and phase 2 must
    # never appear before phase 1: both would break the single-exit property.
    p1_line=$(grep -n 'PHASE 1' src/vfdw_script.c | head -1 | cut -d: -f1)
    p2_line=$(grep -n 'PHASE 2' src/vfdw_script.c | head -1 | cut -d: -f1)
    if [[ -z "$p1_line" || -z "$p2_line" ]]; then
        fail "src/vfdw_script.c: cannot find both phase markers"
    elif (( p2_line <= p1_line )); then
        fail "src/vfdw_script.c: PHASE 2 appears before PHASE 1"
    else
        # Any server.pcall (the write form) inside phase 1 is a write in the
        # check phase, which is the partially-applied outcome by construction.
        #
        # The verb loop and the phase-marker searches above are left to `if !`
        # and to emptiness on purpose: a grep that cannot run makes both of
        # them report a violation, which is the direction that costs nothing
        # but a look. This one points the other way - a grep that cannot run
        # reads as "phase 1 is clean" - so its status is taken apart.
        sed -n "${p1_line},${p2_line}p" src/vfdw_script.c | grep -q "server.pcall"
        rc=${PIPESTATUS[1]}
        if (( rc > 1 )); then
            fail "src/vfdw_script.c: the phase 1 write check could not be run (grep exit ${rc})"
        elif (( rc == 0 )); then
            fail "src/vfdw_script.c: phase 1 contains a write call (invariant W1)"
        fi
    fi
fi

# strlen() on anything from a reply violates invariant I3. It is legitimate on
# our own C literals, so this is advisory rather than fatal.
if grep -rn 'strlen' src/ >/dev/null 2>&1; then
    note "strlen() present; confirm none of it is applied to Valkey-owned data (invariant I3)"
fi

if (( status == 0 )); then
    note "clean"
fi
exit $status
