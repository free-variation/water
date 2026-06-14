#!/bin/sh
# Vendor PCRE2 into external/pcre2 from a pinned upstream release tarball.
#
# Why the release tarball and not a git checkout: the tarball ships the
# pre-generated files (pcre2.h, config.h template, default chartables) that let
# PCRE2 build with no autotools/CMake step, so the vendored tree compiles with
# nothing but a C compiler. The git repo omits those, and carries sljit as a
# nested submodule.
#
# To bump versions: edit PCRE2_VERSION + PCRE2_SHA256, run this, review the diff.
#
#   sh tools/vendor-pcre2.sh
#
set -eu

PCRE2_VERSION=10.47
PCRE2_SHA256=c08ae2388ef333e8403e670ad70c0a11f1eed021fd88308d7e02f596fcd9dc16
PCRE2_URL="https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VERSION}/pcre2-${PCRE2_VERSION}.tar.gz"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/external/pcre2"

sha256() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		sha256sum "$1" | cut -d' ' -f1
	fi
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vendor-pcre2.XXXXXX")
trap 'rm -rf "$work"' EXIT

tarball="$work/pcre2-${PCRE2_VERSION}.tar.gz"
echo "downloading $PCRE2_URL"
curl -sSL --fail --max-time 180 -o "$tarball" "$PCRE2_URL"

got=$(sha256 "$tarball")
if [ "$got" != "$PCRE2_SHA256" ]; then
	echo "sha256 mismatch:" >&2
	echo "  expected $PCRE2_SHA256" >&2
	echo "  got      $got" >&2
	exit 1
fi
echo "sha256 ok"

tar xzf "$tarball" -C "$work"
upstream="$work/pcre2-${PCRE2_VERSION}"

rm -rf "$dest"
mkdir -p "$dest/src" "$dest/deps/sljit"

# The 8-bit library file set, taken straight from upstream's COMMON_SOURCES so
# this stays correct across version bumps. These are width-parameterised single
# sources compiled with PCRE2_CODE_UNIT_WIDTH=8.
common=$(awk '/^COMMON_SOURCES *=/{f=1} f{print} f&&!/\\$/{exit}' "$upstream/Makefile.am" \
	| grep -oE 'src/[A-Za-z0-9_]+\.[ch]')
for f in $common; do
	cp "$upstream/$f" "$dest/$f"
done

# Generated/template files the tarball provides so no configure step is needed.
cp "$upstream/src/pcre2_chartables.c.dist" "$dest/src/pcre2_chartables.c"
cp "$upstream/src/pcre2.h.generic" "$dest/src/pcre2.h"
sed -e 's:/\* #undef SUPPORT_PCRE2_8 \*/:#define SUPPORT_PCRE2_8 1:' \
    -e 's:/\* #undef SUPPORT_UNICODE \*/:#define SUPPORT_UNICODE 1:' \
    -e 's:/\* #undef SUPPORT_JIT \*/:#define SUPPORT_JIT 1:' \
    "$upstream/src/config.h.generic" > "$dest/src/config.h"

# JIT backend: pcre2_jit_compile.c #includes ../deps/sljit/sljit_src/sljitLir.c,
# so the directory layout must be preserved.
cp -R "$upstream/deps/sljit/sljit_src" "$dest/deps/sljit/sljit_src"
[ -f "$upstream/deps/sljit/LICENSE" ] && cp "$upstream/deps/sljit/LICENSE" "$dest/deps/sljit/LICENSE"

cp "$upstream/LICENCE.md" "$dest/LICENCE.md"

cat > "$dest/PROVENANCE" <<EOF
PCRE2 vendored source.

upstream:  $PCRE2_URL
version:   $PCRE2_VERSION
sha256:    $PCRE2_SHA256
config:    SUPPORT_PCRE2_8, SUPPORT_UNICODE, SUPPORT_JIT enabled in src/config.h

Produced by tools/vendor-pcre2.sh. Do not edit these files by hand; re-run the
script to update. PCRE2 is distributed under the terms in LICENCE.md.
EOF

echo "vendored PCRE2 $PCRE2_VERSION into $dest"
