#!/usr/bin/env bash
# Writes the changes made inside src-upstream/ to patches/jsrf.patch (see tools/setup-source.sh).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
git -C "$root/src-upstream" add -N .
git -C "$root/src-upstream" diff > "$root/patches/jsrf.patch"
echo "patches/jsrf.patch: $(grep -c '^diff --git' "$root/patches/jsrf.patch") file(s)"
