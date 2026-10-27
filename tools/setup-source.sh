#!/usr/bin/env bash
# Checks out JSRF Unleashed (JuMatt/JetSetRadioFuture-Unleashed) at the pinned commit into src-upstream/ and
# applies patches/jsrf.patch. patches/ is the source of truth: edit inside src-upstream/, then save the change
# with tools/save-patch.sh. Refuses to touch a src-upstream/ with changes that are not saved in the patch yet.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=https://github.com/JuMatt/JetSetRadioFuture-Unleashed
commit=be930bd
src="$root/src-upstream"
patch="$root/patches/jsrf.patch"

if [[ ! -d $src/.git ]]; then
    git clone --quiet "$repo" "$src"
fi
if [[ -n $(git -C "$src" status --porcelain) ]]; then
    saved=$(mktemp)
    git -C "$src" diff > "$saved"
    if ! cmp -s "$saved" "$patch"; then
        rm -f "$saved"
        echo "setup-source.sh: src-upstream/ has changes not in patches/jsrf.patch; run tools/save-patch.sh first" >&2
        exit 1
    fi
    rm -f "$saved"
    git -C "$src" checkout --quiet -- .
fi
git -C "$src" checkout --quiet "$commit"
if [[ -s $patch ]]; then
    git -C "$src" apply --whitespace=nowarn "$patch"
fi
echo "$src at $commit$( [[ -s $patch ]] && echo ", patched")"
