# Збірка для RV1106 (статично, uclibc):  make   ;  локально для тесту:  make host
LUCKFOX_SDK_DIR ?= $(HOME)/luckfox-pico
CC_ARM := $(LUCKFOX_SDK_DIR)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
CFLAGS ?= -O2 -Wall -Wextra
all: rv1106_ctl
rv1106_ctl: rv1106_ctl.c
	$(CC_ARM) $(CFLAGS) -static -o $@ $<
host: rv1106_ctl.c
	$(CC) $(CFLAGS) -o rv1106_ctl_host $<
clean:
	rm -f rv1106_ctl rv1106_ctl_host
