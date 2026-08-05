CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean

all: fedora_receiver

fedora_receiver: src/main.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	$(RM) fedora_receiver
