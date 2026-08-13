#!/usr/bin/env bash
#
# Render img/*.mmd to img/*.png.
#
# The PNGs are committed and the sources beside them are what they are
# generated from. A checked-in image with no way to reproduce it is a document
# nobody can correct: the next person to change the write path finds a picture
# that disagrees with the code and no means to fix it but an image editor.
#
# Containerised for the same reason everything else here is - the renderer is
# a headless browser and a Node toolchain, and requiring either on the host
# would put "can you rebuild the diagrams" at the mercy of whatever node
# version the developer happens to have.
#
#   ./scripts/diagrams.sh            # render every diagram
#   ./scripts/diagrams.sh --check    # fail if any PNG is out of date
#
# --check is what a CI job runs. It re-renders into a scratch directory and
# compares, so a source edited without regenerating is caught the same way a
# stale container image is, rather than being discovered by a reader.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

IMAGE="${MERMAID_IMAGE:-minlag/mermaid-cli:latest}"
SRC_DIR=img

# 3 renders text that survives a 2x display and a PDF. White rather than
# transparent: a transparent background is unreadable in any dark-themed
# viewer that composites it onto black, which is most of them.
SCALE=3
BACKGROUND=white

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

docker image inspect "$IMAGE" >/dev/null 2>&1 \
    || die "image $IMAGE is missing; run: docker pull $IMAGE"

# One container per run rather than one per diagram: the renderer's startup is
# most of its cost.
render_into() {
    local out_dir="$1" name

    for src in "$SRC_DIR"/*.mmd; do
        name="$(basename "$src" .mmd)"
        docker run --rm -u "$(id -u):$(id -g)" \
            -v "$REPO_ROOT/$SRC_DIR:/src:ro" \
            -v "$out_dir:/out" \
            "$IMAGE" \
            -i "/src/${name}.mmd" -o "/out/${name}.png" \
            -b "$BACKGROUND" -s "$SCALE" >/dev/null
    done
}

if [[ "${1-}" == "--check" ]]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    render_into "$tmp"

    stale=()
    for src in "$SRC_DIR"/*.mmd; do
        name="$(basename "$src" .mmd)"
        if [[ ! -f "$SRC_DIR/${name}.png" ]]; then
            stale+=("${name}.png (missing)")
        elif ! cmp -s "$tmp/${name}.png" "$SRC_DIR/${name}.png"; then
            stale+=("${name}.png")
        fi
    done

    if (( ${#stale[@]} > 0 )); then
        die "these diagrams do not match their sources: ${stale[*]}
    Run: $0"
    fi
    say "every diagram matches its source"
    exit 0
fi

if [[ -n "${1-}" ]]; then
    die "unknown argument: $1"
fi

render_into "$REPO_ROOT/$SRC_DIR"
say "rendered $(ls -1 "$SRC_DIR"/*.mmd | wc -l) diagram(s) into $SRC_DIR/"
