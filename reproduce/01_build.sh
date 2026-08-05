#!/bin/sh
# 01_build.sh — зібрати sender (плата) і receiver (ноут) на чистому ноуті.
# Env: LUCKFOX_SDK_DIR (деф. ~/luckfox-pico), BOARD_IP (деф. 192.168.50.2)
set -e

: "${LUCKFOX_SDK_DIR:=$HOME/luckfox-pico}"
: "${BOARD_IP:=192.168.50.2}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

TC="$LUCKFOX_SDK_DIR/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
RGA_INC="$LUCKFOX_SDK_DIR/media/rga/release_rga_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rga"

echo "== перевірки =="
if [ ! -x "$TC/bin/arm-rockchip830-linux-uclibcgnueabihf-g++" ]; then
    echo "!!! Немає тулчейна: $TC"
    echo "    Клонуй SDK:  git clone --depth 1 https://github.com/LuckfoxTECH/luckfox-pico.git $LUCKFOX_SDK_DIR"
    exit 1
fi
if [ ! -f "$RGA_INC/im2d.hpp" ]; then
    echo "!!! Немає RGA-хедерів rv1106: $RGA_INC"
    echo "    Знайди: find $LUCKFOX_SDK_DIR -name im2d.hpp"
    exit 1
fi
export PATH="$TC/bin:$PATH"

echo "== бібліотеки з плати -> rv1106_sender/prebuilt/ =="
mkdir -p "$REPO/rv1106_sender/prebuilt"
for lib in librockit.so librockchip_mpp.so librga.so; do
    if [ ! -f "$REPO/rv1106_sender/prebuilt/$lib" ]; then
        echo "  scp $lib з плати..."
        scp root@"$BOARD_IP":/oem/usr/lib/"$lib" "$REPO/rv1106_sender/prebuilt/"
    fi
done

echo "== збірка sender (RGA-хедери rv1106) =="
make -C "$REPO/rv1106_sender" -f Makefile.prebuilt \
    LUCKFOX_SDK_DIR="$LUCKFOX_SDK_DIR" RGA_INC="$RGA_INC"

echo "== збірка receiver =="
make -C "$REPO/fedora_receiver"

echo
echo "ГОТОВО:"
echo "  $REPO/rv1106_sender/rv1106_sender   (ARM, на плату)"
echo "  $REPO/fedora_receiver/fedora_receiver  (x86, на ноуті)"
echo "Далі:  ./reproduce/02_deploy.sh  &&  ./reproduce/03_run_board.sh"
