#!/bin/sh
# Vendor isocline into external/isocline from a pinned upstream commit.
#
# isocline (MIT) is a readline replacement; src/isocline.c compiles as a single
# source unit (see its readme.md), so the vendored tree builds with just a C
# compiler. Compile flags live in the Makefile (ISOCLINE_CFLAGS).
#
# To bump: set ISOCLINE_COMMIT to a new commit, run this, copy the new hash from
# the mismatch message into ISOCLINE_SHA256, review the diff.
#
#   sh tools/vendor-isocline.sh
#
set -eu

ISOCLINE_COMMIT=8d6dc1ef95b1b46711e66eb23d39d4467a0fcdac
ISOCLINE_SHA256=0a2149aa99bdffc0c430aa4ecfcdc378bd5f98565eef10991475db52b33f607b
ISOCLINE_URL="https://codeload.github.com/daanx/isocline/tar.gz/${ISOCLINE_COMMIT}"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/isocline"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-isocline.XXXXXX")
trap 'rm -rf "$work"' EXIT

tgz="$work/isocline.tar.gz"
echo "downloading $ISOCLINE_URL"
curl -sSL --fail --max-time 180 -o "$tgz" "$ISOCLINE_URL"

got=$(sha256 "$tgz")
if [ "$got" != "$ISOCLINE_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $ISOCLINE_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tgz" -C "$work"
upstream="$work/isocline-${ISOCLINE_COMMIT}"

rm -rf "$dest"
mkdir -p "$dest"
cp -R "$upstream/src" "$dest/src"
cp -R "$upstream/include" "$dest/include"
cp "$upstream/LICENSE" "$dest/LICENSE"

cat > "$dest/PROVENANCE" <<EOF
isocline vendored source.

upstream:  https://github.com/daanx/isocline
commit:    $ISOCLINE_COMMIT
tarball:   $ISOCLINE_URL
sha256:    $ISOCLINE_SHA256

Built as a single source unit (src/isocline.c) per isocline's readme.md;
compile flags live in the Makefile (ISOCLINE_CFLAGS). MIT-licensed (see
LICENSE). Produced by tools/vendor-isocline.sh; do not edit by hand, re-run
the script to update.
EOF

echo "vendored isocline ${ISOCLINE_COMMIT} into $dest"
