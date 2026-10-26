#!/bin/sh
# Print a version's section of CHANGELOG.md, which is the body of its GitHub release.
#
#   sh scripts/release-notes.sh 0.2.0
#
# With no version, the one in src/main.c. Fails when the section is missing or empty.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

die() {
    printf 'release-notes.sh: %s\n' "$*" >&2
    exit 1
}

version=${1:-$(sed -n 's/^#define BEIGE_VERSION "\(.*\)"$/\1/p' "$ROOT/src/main.c")}
[ -n "$version" ] || die "cannot read BEIGE_VERSION from src/main.c"
[ -f "$ROOT/CHANGELOG.md" ] || die "no CHANGELOG.md"

# From "## VERSION ..." to the next "## ", without the blank lines at the start.
notes=$(awk -v v="$version" '
    $1 == "##" && $2 == v { on = 1; next }
    on && /^## / { exit }
    on && !text && /^[ \t]*$/ { next }
    on { text = 1; print }' "$ROOT/CHANGELOG.md")
[ -n "$notes" ] || die "CHANGELOG.md has no section for $version"
printf '%s\n' "$notes"
