#!/bin/bash
set -e
cd "$(dirname "$0")"
K=../../kernel
W=${TMPDIR:-/tmp}/merden_xhci_test; mkdir -p $W
python3 mkfat.py $W/tmpl.img 64 --bps 512 --spc 8 --scheme mbr > /dev/null
gcc -m64 -no-pie -g -O1 -fsanitize=undefined -fno-sanitize-recover=undefined -Wall -Wno-unused-function \
  -DXHCI_HOSTTEST -DUSB_HOSTTEST -I$K/headers xhci_test.c emu_xhci.c $K/code/drivers/mushka/xhci.c \
  $K/code/drivers/mushka/usb_msc.c $K/code/drivers/mushka/xhci_hid.c $K/filesystems/fat32.c -o $W/xhci_test
cd $W && USBTEST_DIR=$OLDPWD ASAN_OPTIONS=detect_leaks=0 ./xhci_test tmpl.img ${1:-300}
