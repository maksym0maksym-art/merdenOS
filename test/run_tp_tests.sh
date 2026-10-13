#!/bin/bash
# Host-тест жестів тачпада (прокрутка, перетягування кнопкою + другим пальцем, тап-перетягування)
set -e
cd "$(dirname "$0")"
W=${TMPDIR:-/tmp}/merden_tp_test; mkdir -p $W
gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -I../../kernel/headers tp_test.c \
    ../../kernel/code/drivers/mushka/touchpad.c -o $W/tp_test
$W/tp_test
