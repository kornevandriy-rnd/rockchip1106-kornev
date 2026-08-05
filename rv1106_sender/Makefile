ifndef LUCKFOX_SDK
$(error Set LUCKFOX_SDK=/absolute/path/to/luckfox-pico)
endif

MEDIA_PARAM := $(LUCKFOX_SDK)/media/Makefile.param
include $(MEDIA_PARAM)

CXX := $(RK_MEDIA_CROSS)-g++
TARGET := rv1106_sender
SRC := src/main.cpp

CPPFLAGS += -I$(RK_MEDIA_OUTPUT)/include
CXXFLAGS += -O2 -g -std=gnu++11 -Wall -Wextra -DRV1106_RV1103
LDFLAGS += -Wl,-rpath-link,$(RK_MEDIA_OUTPUT)/lib
LDFLAGS += -L$(RK_MEDIA_OUTPUT)/lib -L$(RK_MEDIA_OUTPUT)/root/usr/lib
LDLIBS += -lrockit -lrockchip_mpp -lrga -lpthread -lm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	$(RM) $(TARGET)
