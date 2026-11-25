#!/bin/sh
# Compiles one generated game source for PS5. Usage: compile_gen.sh <file.c>
# Needs PS5_PAYLOAD_SDK, LLVM_CONFIG and SPIKE_OBJ in the environment; run from the Dirt3 repo root.
# GEN_OPT: the optimisation level, -O2 by default (-Oz for the small payload build).
b=$(basename "$1" .c)
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=c11 -w ${GEN_OPT:--O2} -fno-strict-aliasing -DRECOMP_TSO=1 \
    -Iruntime/include -Igenerated/src -c "$1" -o "$SPIKE_OBJ/$b.o" > "$SPIKE_OBJ/$b.log" 2>&1 || echo "FAIL $b"
