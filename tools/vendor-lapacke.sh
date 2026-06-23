#!/bin/sh
# Vendor the LAPACKE C-interface closure for a chosen set of routines.
#
# Maintainer tool: needs network + clang + Apple's Accelerate. The COMMITTED
# result under external/lapacke/ builds offline with nothing but a C compiler,
# exactly like the other vendored deps. logicforth's LAPACKE path is
# Accelerate/macOS-specific, so this script is too.
#
# LAPACKE ships ~2600 wrapper files (every routine x s/d/c/z x {high,_work}).
# We compile the whole thing transiently, then let the linker tell us the exact
# object closure of the routines we actually expose and copy only those.
#
# Refresh / add a routine: pass the LAPACKE_<name> list as arguments (or edit
# DEFAULT_ROUTINES) and rerun.
set -eu

REPO=https://github.com/Reference-LAPACK/lapack
DEFAULT_ROUTINES="dgesvd dggglm dgglse"
ROUTINES="${*:-$DEFAULT_ROUTINES}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/external/lapacke"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "cloning $REPO ..."
git clone --depth 1 "$REPO" "$WORK/lapack" >/dev/null 2>&1
COMMIT=$(git -C "$WORK/lapack" rev-parse HEAD)

cd "$WORK/lapack/LAPACKE"
# lapack.h includes "lapacke_mangling.h", which upstream only ships as a .in
# template (CMake/Make generates it). Its default branch is the lcname##_
# (trailing-underscore) mangling that matches Accelerate's exported _dgesvd_.
cp include/lapacke_mangling_with_flags.h.in include/lapacke_mangling.h
echo "compiling full LAPACKE C archive (transient) ..."
clang -O2 -DNDEBUG -Iinclude -c src/*.c utils/*.c
ar cr libfull.a ./*.o
ranlib libfull.a

echo "deriving object closure for: $ROUTINES"
exports=""
for r in $ROUTINES; do exports="$exports -Wl,-exported_symbol,_LAPACKE_$r"; done
# -undefined dynamic_lookup only so the probe link can't fail over Fortran
# externals (they are leaves, not in the archive, and don't affect membership).
clang -dynamiclib -o /dev/null $exports -Wl,-dead_strip \
      -Wl,-map,"$WORK/map.txt" libfull.a -framework Accelerate \
      -undefined dynamic_lookup
objs=$(grep -oE 'lapacke_[a-z0-9_]+\.o' "$WORK/map.txt" | sort -u)

echo "copying closure into $DEST ..."
rm -rf "$DEST/src" "$DEST/utils" "$DEST/include"
mkdir -p "$DEST/src" "$DEST/utils" "$DEST/include"
n=0
for o in $objs; do
	c="${o%.o}.c"
	if   [ -f "src/$c" ];   then cp "src/$c"   "$DEST/src/";   n=$((n+1))
	elif [ -f "utils/$c" ]; then cp "utils/$c" "$DEST/utils/"; n=$((n+1))
	fi
done
cp include/lapack.h include/lapacke.h include/lapacke_64.h \
   include/lapacke_config.h include/lapacke_utils.h \
   include/lapacke_mangling.h "$DEST/include/"
cp ../LICENSE "$DEST/LICENSE"

cat > "$DEST/PROVENANCE" <<EOF
LAPACKE (C interface to LAPACK) -- vendored closure, not the full library.

Upstream:  $REPO
Commit:    $COMMIT
Routines:  $ROUTINES
Contents:  the exact object closure of those LAPACKE_<routine> wrappers
           (derived from a linker dead-strip map), split into src/ and utils/,
           plus the include/ headers they need.
License:   modified BSD (BSD-3-Clause). See LICENSE.

Build:     the top-level Makefile compiles src/ + utils/ into liblapacke.a, then
           links liblapacke_accel.dylib with -framework Accelerate, so the
           Fortran routines (dgesvd_, dggglm_, ...) resolve from Accelerate. The
           dylib's -exported_symbol list is exactly the routines above.

Refresh:   tools/vendor-lapacke.sh [routine ...]   (needs network + Accelerate)
EOF

echo "vendored $n source files for [$ROUTINES] (closure = $(echo "$objs" | wc -l | tr -d ' ') objects)"
echo "src:";   ls "$DEST/src"
echo "utils:"; ls "$DEST/utils"
