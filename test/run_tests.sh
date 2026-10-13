#!/bin/bash
# Host-тести FAT32: збірка з ASan/UBSan, випадкові операції, незалежна перевірка образу fatcheck.py
set -e
cd "$(dirname "$0")"
W=${TMPDIR:-/tmp}/merden_fat_test; mkdir -p $W
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -I../../kernel/headers fat_test.c ../../kernel/filesystems/fat32.c -o $W/fat_test
run() {  # name size_mb bps spc scheme seed nops
  local img=$W/$1.img
  python3 mkfat.py $img $2 --bps $3 --spc $4 --scheme $5 > /dev/null
  $W/fat_test $img $3 $6 $7 $W/$1.model
  python3 fatcheck.py $img --quiet --extract $W/$1.extract
  diff -r $W/$1.model $W/$1.extract && echo "== $1: OK (model == image)"
}
run mbr512   64  512 8  mbr  1 600
run mbr512b  64  512 1  mbr  2 600
run none512  32  512 4  none 3 500
run gpt512   64  512 16 gpt  4 500
run mbr4k    128 4096 1 mbr  5 500
run bigclus  200 512 64 mbr  6 400
