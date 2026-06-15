#!/bin/sh
# Vendor SQLite into external/sqlite from a pinned upstream amalgamation.
#
# The amalgamation is SQLite's whole library as one sqlite3.c + sqlite3.h
# (public domain), so the vendored tree compiles with nothing but a C compiler.
#
# To bump versions: update the three SQLITE_* identifiers + SQLITE_SHA256 from
# https://sqlite.org/download.html, run this, review the diff.
#
#   sh tools/vendor-sqlite.sh
#
set -eu

SQLITE_VERSION=3.53.2
SQLITE_VERSION_ID=3530200
SQLITE_YEAR=2026
SQLITE_SHA256=8a310d0a16c7a90cacd4c884e70faa51c902afed2a89f63aaa0126ab83558a32
SQLITE_URL="https://sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION_ID}.zip"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/sqlite"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-sqlite.XXXXXX")
trap 'rm -rf "$work"' EXIT

zip="$work/sqlite-amalgamation-${SQLITE_VERSION_ID}.zip"
echo "downloading $SQLITE_URL"
curl -sSL --fail --max-time 180 -o "$zip" "$SQLITE_URL"

got=$(sha256 "$zip")
if [ "$got" != "$SQLITE_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $SQLITE_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

unzip -q "$zip" -d "$work"
upstream="$work/sqlite-amalgamation-${SQLITE_VERSION_ID}"

rm -rf "$dest"
mkdir -p "$dest"
cp "$upstream/sqlite3.c" "$dest/sqlite3.c"
cp "$upstream/sqlite3.h" "$dest/sqlite3.h"
cp "$upstream/sqlite3ext.h" "$dest/sqlite3ext.h"

cat > "$dest/PROVENANCE" <<EOF
SQLite vendored source (amalgamation).

upstream:  $SQLITE_URL
version:   $SQLITE_VERSION
sha256:    $SQLITE_SHA256

Compile flags live in the Makefile (SQLITE_CFLAGS). SQLite is in the public
domain. Produced by tools/vendor-sqlite.sh; do not edit these files by hand,
re-run the script to update.
EOF

echo "vendored SQLite $SQLITE_VERSION into $dest"
