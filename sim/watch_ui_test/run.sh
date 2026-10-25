#!/bin/sh
# Watch screens on the PC: the real watch_ui.c + LVGL 8.3.11, a scripted
# rod instead of Bluetooth. Saves each screen (w*.raw, RGB565) and taps
# every button.
#
#   git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git /tmp/lvgl
#   sh run.sh /tmp/lvgl
set -e
L=${1:-/tmp/lvgl}
W=../../watch_ble/StrikeSense_Watch
mkdir -p obj
for f in $(find $L/src -name '*.c'); do
  o=obj/$(echo $f | md5sum | cut -c1-12).o
  [ -f $o ] || gcc -O1 -c -w -DLV_CONF_INCLUDE_SIMPLE -I. -I$L $f -o $o
done
gcc -O1 -Wall -fsyntax-only -DLV_CONF_INCLUDE_SIMPLE -I. -I$L -I$W $W/watch_ui.c
# host.c includes watch_ui.c, so the test can look at the page numbers
gcc -O1 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I$L -I$W -c host.c -o host.o
gcc -o host host.o obj/*.o -lm
./host
# the PAIR YOUR ROD screens
gcc -O1 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I$L -I$W -c host_pair.c -o host_pair.o
gcc -o host_pair host_pair.o obj/*.o -lm
./host_pair
