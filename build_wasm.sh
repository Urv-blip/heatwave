#!/bin/sh
# Rebuild heatwave.wasm from heatwave.c (needs clang, lld, wasi-libc).
# Ubuntu/WSL: sudo apt install clang lld wasi-libc libclang-rt-18-dev-wasm32
clang --target=wasm32-wasi --sysroot=/usr -mexec-model=reactor -DHW_WASM -O2 \
      -Wl,--strip-all heatwave.c -o heatwave.wasm -lm
