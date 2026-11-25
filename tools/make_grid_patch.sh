#!/bin/bash
# Regenerates ps5/patches/grid/0001-grid-recomp.patch: the GRID changes as a patch against the DiRT 3 tree.
# Use it when you develop in a working copy of the GRID tree (default ../grid-recomp-src, a copy of
# the DiRT 3 tree with the GRID changes) so that this repository stays the only thing you publish.
#
#   tools/make_grid_patch.sh [DIRT3_TREE] [GRID_TREE]
#
# Check it afterwards: apply it to a clean copy of the DiRT 3 tree, then the PS5 patch on top
# (patch -p1 < ps5/patches/grid/0001-grid-recomp.patch; patch -p1 < ps5/patches/dirt3/0001-ps5-port.patch).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
BASE=$(cd "${1:-$HERE/../dirt3-recomp-src}" && pwd)
GRID=$(cd "${2:-$HERE/../grid-recomp-src}" && pwd)
OUT=$HERE/ps5/patches/grid/0001-grid-recomp.patch
mkdir -p "$(dirname "$OUT")"
cd "$(dirname "$BASE")"
RAW=$(mktemp)
# diff exits 1 when the trees differ
diff -ruN -x build -x generated -x 'generated_*' -x third_party -x .git -x .DS_Store -x .claude -x work -x cfg3 \
    "$(basename "$BASE")" "$(basename "$GRID")" > "$RAW" || true
python3 - "$RAW" "$OUT" "$(basename "$BASE")" "$(basename "$GRID")" <<'PY'
import re, sys
raw, out, base, grid = sys.argv[1:5]
lines = []
for l in open(raw, encoding='utf-8', errors='surrogateescape'):
    if l.startswith('--- '):
        l = '--- ' + l[4:].split('\t')[0].rstrip('\n').replace(base + '/', 'a/', 1) + '\n'
    elif l.startswith('+++ '):
        l = '+++ ' + l[4:].split('\t')[0].rstrip('\n').replace(grid + '/', 'b/', 1) + '\n'
    elif l.startswith('diff -ruN'):
        m = re.match(r'diff -ruN .*?%s/(\S+) %s/(\S+)' % (re.escape(base), re.escape(grid)), l)
        if m: l = 'diff -ruN a/%s b/%s\n' % (m.group(1), m.group(2))
    lines.append(l)
open(out, 'w', encoding='utf-8', errors='surrogateescape').writelines(lines)
PY
rm -f "$RAW"
echo "wrote $OUT ($(grep -c '^diff -ruN' "$OUT") files)"
