#!/bin/bash
# Assemble a patch_asm source at its ".set BASE" address (eboot vaddr) and print the bytes as hex.
set -euo pipefail
src=$1; base=$(sed -n 's/^ *\.set BASE, *\(0x[0-9a-fA-F]*\).*/\1/p' "$src")
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
as --64 -o "$tmp/o.o" "$src"
ld -m elf_x86_64 -Ttext="$base" -e 0 -o "$tmp/o.elf" "$tmp/o.o"
objcopy -O binary -j .text "$tmp/o.elf" "$tmp/o.bin"
xxd -p "$tmp/o.bin" | tr -d '\n'; echo
