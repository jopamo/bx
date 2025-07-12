#!/bin/sh
set -eu

builddir="${1:-build-lto}"

awk '
/^build src\/gnu\/libbx_gnu_shared_raw\.a\.p\/shared_gnulib_.*: c_COMPILER ..\/src\/gnu\/shared\/gnulib\// {
  gsub(/^src\/gnu\/libbx_gnu_shared_raw\.a\.p\//, "", $2)
  gsub(/:$/, "", $2)
  print $2, $4
}
' "$builddir/build.ninja" | sed 's/:$//' | sort > "$builddir/gnu-object-source-map.txt"

grep -ho 'shared_gnulib_[^ ]*\.c\.o' "$builddir/bx-gnu-tar.map" "$builddir/bx-gnu-cpio.map" | sort -u > "$builddir/gnu-used-objects.txt"

join "$builddir/gnu-used-objects.txt" "$builddir/gnu-object-source-map.txt" | awk '{print $2}' | sort -u > "$builddir/gnu-used-sources.txt"

awk '{print $2}' "$builddir/gnu-object-source-map.txt" | sort -u > "$builddir/gnu-all-sources.txt"

comm -23 "$builddir/gnu-all-sources.txt" "$builddir/gnu-used-sources.txt" > "$builddir/gnu-unused-sources.txt"

echo "USED:"
sed -n '1,200p' "$builddir/gnu-used-sources.txt"
echo
echo "UNUSED:"
sed -n '1,200p' "$builddir/gnu-unused-sources.txt"
