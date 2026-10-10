#!/bin/sh
# dec.sh rva [rva...] : Ghidra-decompile functions of RiftApart.exe (radare2 + r2ghidra)
export PATH=/home/claude/tools/r2/bin:$PATH LD_LIBRARY_PATH=/home/claude/tools/r2/lib
cmds=""
for a in "$@"; do cmds="$cmds s $((0x140000000 + $a)); af; echo ===== $a; pdg;"; done
cd "$(dirname "$0")" && timeout 900 r2 -q -e bin.cache=true -e scr.color=0 -c "$cmds" RiftApart.exe 2>&1
