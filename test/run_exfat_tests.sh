#!/bin/bash
# Host-тести exFAT: exfat_test.c генерується з fat_test.c (fat_* -> xf_*), далі — ASan/UBSan,
# випадкові операції з моделлю та незалежна перевірка образу exfatcheck.py
set -e
cd "$(dirname "$0")"
W=${TMPDIR:-/tmp}/merden_exfat_test; mkdir -p $W
sed -E 's/fat_(append_file|flush|fread|fwrite|ftruncate|fclose|mkdir|mount|open|read_file|readdir|rename|rmdir|stat|statfs|unlink|write_file|create)\b/xf_\1/g; s/fat_vol_t/xf_vol_t/g; s/fat_file_t/xf_file_t/g; s/"fat32.h"/"exfat_fs.h"/; s/fat_stress/xf_stress/g' fat_test.c > $W/exfat_test.c
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -I../../kernel/headers $W/exfat_test.c \
    ../../kernel/filesystems/exfat_fs.c ../../kernel/filesystems/fat32.c -o $W/exfat_test
run() {  # name size_mb bps spc scheme seed nops
  local img=$W/$1.img
  python3 mkexfat.py $img $2 --bps $3 --spc $4 --scheme $5 > /dev/null
  python3 exfatcheck.py $img --quiet
  $W/exfat_test $img $3 $6 $7 $W/$1.model
  python3 exfatcheck.py $img --quiet --extract $W/$1.extract
  diff -r $W/$1.model $W/$1.extract && echo "== $1: OK (model == image)"
}
run xmbr512   64  512 8  mbr  1 600
run xmbr512b  64  512 1  mbr  2 600
run xnone512  32  512 4  none 3 500
run xgpt512   64  512 16 gpt  4 500
run xmbr4k    128 4096 1 mbr 5 500
run xbigclus  200 512 64 mbr  6 400
run xtiny     8   512 1  mbr  7 400
# суміжні файли/каталоги (NoFatChain) від інших ОС
for cfg in "512 8 mbr 32" "512 1 none 16" "4096 1 gpt 64"; do
  set -- $cfg
  python3 mkexfat.py $W/nf.img $4 --bps $1 --spc $2 --scheme $3 --prefill > /dev/null
  python3 exfatcheck.py $W/nf.img --quiet
  gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -I../../kernel/headers exfat_nofat_test.c \
      ../../kernel/filesystems/exfat_fs.c ../../kernel/filesystems/fat32.c -o $W/nofat_test
  $W/nofat_test $W/nf.img $1
  python3 exfatcheck.py $W/nf.img --quiet && echo "== nofat bps=$1 spc=$2 $3: OK"
done
# склейка usbfs.c: перемикання дисків, звичайні команди на USB, копіювання між FAT32 і exFAT
python3 mkfat.py $W/u0.img 32 --scheme mbr > /dev/null
python3 mkexfat.py $W/u1.img 32 --scheme mbr > /dev/null
dd if=/dev/zero of=$W/u2.img bs=1M count=4 2>/dev/null
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -I../../kernel/headers usbfs_test.c ../../system/shell/usbfs.c \
    ../../kernel/filesystems/fat32.c ../../kernel/filesystems/exfat_fs.c -o $W/usbfs_test 2>/dev/null
$W/usbfs_test $W/u0.img $W/u1.img $W/u2.img
python3 exfatcheck.py $W/u1.img --quiet && python3 fatcheck.py $W/u0.img --quiet && echo "== usbfs: OK (images consistent)"
