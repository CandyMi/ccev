#!/bin/sh
# init-deps.sh — Re-initialize or repair git submodules for ccev.
#
# Usage:
#   ./scripts/init-deps.sh          # Interactive (prompts before wiping)
#   ./scripts/init-deps.sh -f       # Force, no prompt
#
# This script removes deps/ and re-runs git submodule update --init.
# It's only needed when:
#   - You're mirroring the repos to a different git server
#   - A submodule is corrupted
#   - You want to switch to a fork of a dependency

set -e

# ── check prerequisites ──────────────────────────────────────────────
if ! command -v git >/dev/null 2>&1; then
    echo "[init-deps] ERROR: git not found"
    exit 1
fi

# Must be run from the project root
if [ ! -f ".gitmodules" ]; then
    echo "[init-deps] ERROR: no .gitmodules found — run from ccev project root"
    exit 1
fi

# ── prompt ───────────────────────────────────────────────────────────
if [ "$1" != "-f" ]; then
    echo "============================================"
    echo "  ccev — deps re-initialization"
    echo ""
    echo "  This will DELETE deps/ and re-clone all"
    echo "  submodules from the URLs in .gitmodules."
    echo ""
    echo "  If you forked the deps to your own git"
    echo "  server, edit .gitmodules FIRST to point"
    echo "  to your URLs, then run this."
    echo "============================================"
    printf "Proceed? [y/N] "
    read reply
    case "$reply" in
        y|Y|yes|YES) ;;
        *) echo "[init-deps] cancelled"; exit 0 ;;
    esac
fi

# ── clean ────────────────────────────────────────────────────────────
echo "[init-deps] removing deps/ ..."
rm -rf deps/epoll deps/ccalg deps/ccsocket

# ── re-init ──────────────────────────────────────────────────────────
echo "[init-deps] initializing submodules ..."
git submodule update --init --recursive --force 2>&1

# ── verify ───────────────────────────────────────────────────────────
missing=""
for dep in epoll ccalg ccsocket; do
    if [ ! -f "deps/$dep/README.md" ]; then
        missing="$missing $dep"
    fi
done

if [ -n "$missing" ]; then
    echo "[init-deps] ERROR: submodule(s) missing:$missing"
    echo "[init-deps]   Check your git server URLs in .gitmodules"
    echo "[init-deps]   or restore connectivity and re-run."
    exit 1
fi

echo "[init-deps] OK — all submodules ready"
