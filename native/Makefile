# Makefile — крос-компіляція thermal_stream під Rockchip RV1106 (Luckfox Pico Ultra).
# На платі компілятора НЕМА — збираємо на ноуті, кидаємо бінар через scp.
#
# Потрібні три речі зі SDK / плати:
#   1) ТУЛЧЕЙН   — arm-rockchip830-linux-uclibcgnueabihf (є в Luckfox SDK)
#   2) RK_MPI    — хедери (rk_mpi_venc.h …) + librockit.so  (rockit, апаратний VENC)
#   3) RGA       — хедери (im2d.h) + librga.so             (колірна конверсія/масштаб)
#
# Найпростіший запуск (усе автоматично від кореня SDK):
#
#   export LUCKFOX_SDK_DIR=$HOME/luckfox-pico
#   make RKMPI_INC=$(find $LUCKFOX_SDK_DIR -name rk_mpi_venc.h -printf '%h\n' | head -1) \
#        RKMPI_LIB=/path/to/lib_з_librockit.so \
#        RGA_DIR=/path/до/librga
#
# Якщо librockit.so / librga.so немає в SDK — скопіюй їх із ПЛАТИ (див. README):
#   scp root@192.168.50.2:/oem/usr/lib/librockit.so   native/prebuilt/
#   scp root@192.168.50.2:/oem/usr/lib/librga.so      native/prebuilt/
# і вкажи RKMPI_LIB=native/prebuilt RGA_LIB=native/prebuilt

# ── Тулчейн ───────────────────────────────────────────────────────────────────
LUCKFOX_SDK_DIR  ?= $(HOME)/luckfox-pico
TOOLCHAIN_BIN    ?= $(LUCKFOX_SDK_DIR)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
TOOLCHAIN_PREFIX ?= arm-rockchip830-linux-uclibcgnueabihf
CXX := $(TOOLCHAIN_BIN)/$(TOOLCHAIN_PREFIX)-g++

# ── Шляхи до RK_MPI (rockit) ──────────────────────────────────────────────────
# RKMPI_INC — тека, де лежить rk_mpi_venc.h (та сусідні rk_mpi_*.h / rk_comm_*.h)
# RKMPI_LIB — тека з librockit.so
RKMPI_INC ?= $(LUCKFOX_SDK_DIR)/media/rockit/rockit/include
RKMPI_LIB ?= $(LUCKFOX_SDK_DIR)/media/rockit/rockit/lib

# ── Шляхи до RGA ──────────────────────────────────────────────────────────────
# За замовчуванням беремо librga з каркаса rv1106-upscale (third_party).
RGA_DIR   ?= ../third_party/librga
RGA_INC   ?= $(RGA_DIR)/include
RGA_LIB   ?= $(RGA_DIR)/lib/gcc-uclib-armhf

# ── Прапорці ──────────────────────────────────────────────────────────────────
BUILD_DIR := build
BIN       := $(BUILD_DIR)/thermal_stream

CXXFLAGS := -O2 -Wall -std=c++11 \
            -I$(RKMPI_INC) -I$(RGA_INC)
# --allow-shlib-undefined: не вимагати транзитивні залежності librockit під час
# лінковки (librockchip_mpp тощо) — вони підтягнуться на платі у рантаймі.
LDFLAGS  := -L$(RKMPI_LIB) -L$(RGA_LIB) \
            -lrockit -lrga -lpthread \
            -Wl,--allow-shlib-undefined \
            -Wl,-rpath-link=$(RKMPI_LIB) -Wl,-rpath-link=$(RGA_LIB)

.PHONY: all clean check
all: check $(BIN)

check:
	@[ -x "$(CXX)" ] || { echo "!!! нема компілятора: $(CXX)"; \
	  echo "    постав LUCKFOX_SDK_DIR або TOOLCHAIN_BIN (див. коментар у Makefile)"; exit 1; }
	@[ -f "$(RKMPI_INC)/rk_mpi_venc.h" ] || { echo "!!! нема rk_mpi_venc.h у RKMPI_INC=$(RKMPI_INC)"; \
	  echo "    знайди: find \$$LUCKFOX_SDK_DIR -name rk_mpi_venc.h"; exit 1; }
	@[ -f "$(RGA_INC)/im2d.h" ] || { echo "!!! нема im2d.h у RGA_INC=$(RGA_INC)"; exit 1; }

$(BIN): thermal_stream.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)
	@echo "Готово: $(BIN)"
	@echo "Кинути на плату:  scp $(BIN) root@192.168.50.2:/root/"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
