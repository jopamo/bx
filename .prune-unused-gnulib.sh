#!/bin/sh
set -eu

builddir="${1:-build-lto}"
used="$builddir/gnu-used-sources.txt"
unused="$builddir/gnu-unused-sources.txt"
meson_file="src/gnu/meson.build"
tmp_file="$meson_file.new"

awk -v used_list="$used" '
BEGIN {
  while ((getline line < used_list) > 0) {
    sub(/^\.\.\/src\/gnu\//, "", line)
    used[++used_count] = line
  }
}
$0 == "bx_gnu_shared_gnulib_sources = files(" {
  print
  for (i = 1; i <= used_count; i++)
    printf("  '\''%s'\'',\n", used[i])
  in_block = 1
  next
}
in_block {
  if ($0 == ")") {
    print
    in_block = 0
  }
  next
}
{ print }
' "$meson_file" > "$tmp_file"

mv "$tmp_file" "$meson_file"

while IFS= read -r path; do
  rel=${path#../src/gnu/}
  rm -f "src/gnu/$rel"
done < "$unused"
