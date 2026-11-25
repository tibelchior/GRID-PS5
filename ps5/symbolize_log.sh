#!/bin/bash
# Names the code addresses in a PS5 run's log lines (stdin) using the linked title ELF.
# Run inside the build container: symbolize_log.sh <anchor address from the "prof: ... anchor" line> [elf]
anchor=$1
elf=${2:-/build/game/llvm-pie-last.elf}
base=$(llvm-nm "$elf" | awk '/ T _ZN2rt14start_profilerEv$/ { print $1 }')
slide=$((0x$anchor - 0x$base))
while IFS= read -r line; do
    out=$line
    # The console prints %p without 0x: bare hex words of six to nine digits.
    for a in $(grep -oE '\b[0-9a-f]{6,9}\b' <<<"${line#*]}" | sort -u); do
        v=$((0x$a - slide))
        [ $v -gt 0 ] || continue
        name=$(llvm-symbolizer -C -f -s -e "$elf" "$(printf '0x%x' $v)" 2>/dev/null | head -1 | cut -c1-70)
        [ "$name" = "??" ] || out=${out//$a/$name}
    done
    printf '%s\n' "$out"
done
