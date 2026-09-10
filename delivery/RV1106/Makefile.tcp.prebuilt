# Makefile — збірка sender'а КЕРІВНИКА нашим перевіреним способом:
#   тулчейн Luckfox SDK + хедери RK_MPI/RGA зі SDK + готові .so з ПЛАТИ (prebuilt/).
# Не потребує повної збірки SDK media (на відміну від Makefile.sdk керівника,
# який тягне $(LUCKFOX_SDK)/media/Makefile.param і збудований RK_MEDIA_OUTPUT).
#
# Кроки:
#   1) три бібліотеки з плати у prebuilt/:
#        mkdir -p prebuilt
#        scp root@192.168.50.2:/oem/usr/lib/librockit.so        prebuilt/
#        scp root@192.168.50.2:/oem/usr/lib/librockchip_mpp.so  prebuilt/
#        scp root@192.168.50.2:/oem/usr/lib/librga.so           prebuilt/
#   2) make LUCKFOX_SDK_DIR=$HOME/luckfox-pico

LUCKFOX_SDK_DIR  ?= $(HOME)/luckfox-pico
TOOLCHAIN_BIN    ?= $(LUCKFOX_SDK_DIR)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
TOOLCHAIN_PREFIX ?= arm-rockchip830-linux-uclibcgnueabihf
CXX := $(TOOLCHAIN_BIN)/$(TOOLCHAIN_PREFIX)-g++

# Хедери RK_MPI (rockit) і RGA (im2d.hpp) — зі SDK.
RKMPI_INC ?= $(LUCKFOX_SDK_DIR)/media/rockit/rockit/mpi/sdk/include
RGA_INC   ?= $(LUCKFOX_SDK_DIR)/media/rga/release_rga_rk3588_aarch64-rockchip1031-linux-gnu/include/rga
# Готові .so з плати:
LIBDIR    ?= prebuilt

TARGET := rv1106_sender
CXXFLAGS := -O2 -g -std=gnu++11 -Wall -Wextra -DRV1106_RV1103 \
            -I$(RKMPI_INC) -I$(RGA_INC)
LDFLAGS  := -L$(LIBDIR) -lrockit -lrockchip_mpp -lrga -lpthread -lm \
            -Wl,--allow-shlib-undefined -Wl,-rpath-link=$(LIBDIR)

.PHONY: all clean check
all: check $(TARGET)

check:
	@[ -x "$(CXX)" ] || { echo "!!! нема компілятора: $(CXX) (постав LUCKFOX_SDK_DIR)"; exit 1; }
	@[ -f "$(RKMPI_INC)/rk_mpi_venc.h" ] || { echo "!!! нема rk_mpi_venc.h у RKMPI_INC=$(RKMPI_INC)"; exit 1; }
	@[ -f "$(RGA_INC)/im2d.hpp" ] || { echo "!!! нема im2d.hpp у RGA_INC=$(RGA_INC) — знайди: find \$$LUCKFOX_SDK_DIR -name im2d.hpp"; exit 1; }
	@[ -f "$(LIBDIR)/librockit.so" ] && [ -f "$(LIBDIR)/librockchip_mpp.so" ] && [ -f "$(LIBDIR)/librga.so" ] \
	  || { echo "!!! у $(LIBDIR)/ потрібні librockit.so, librockchip_mpp.so, librga.so (scp з плати)"; exit 1; }

$(TARGET): src/main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)
	@echo "Готово: $(TARGET)"
	@echo "На плату:  scp $(TARGET) root@192.168.50.2:/root/"

clean:
	$(RM) $(TARGET)
