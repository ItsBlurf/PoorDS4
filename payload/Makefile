PS5_HOST ?=
PORT     ?= 9021
TARGET        := PoorDS4.elf
EXP_TARGET    := PoorDS4exp.elf
STATUS_TARGET := PoorDS4-status.elf
STOP_TARGET   := PoorDS4-stop.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ifeq ($(OS),Windows_NT)
CC := ps5-clang.cmd
STRIP := llvm-strip
endif

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -Wshadow -Wunreachable-code \
	-Werror -Wno-unused-but-set-global -g -O2 -fPIC \
	-fno-stack-protector -fno-jump-tables -ffunction-sections -fdata-sections \
	-fdebug-compilation-dir=.
LDFLAGS      += -Wl,--gc-sections,--build-id=sha1
RUNTIME_LIBS := -nodefaultlibs -lc -lkernel_web -lSceLibcInternal
BRIDGE_LIBS  := $(RUNTIME_LIBS) -lSceUserService

COMMON_HEADERS := $(wildcard *.h)
HOST_CC ?= gcc
PYTHON ?= python3

.PHONY: all audit clean deploy release status stop

all: $(TARGET)

$(TARGET): game_pad_bridge_main.c wireless_ds4.c $(COMMON_HEADERS)
	$(CC) $(CFLAGS) -DPOORDS4_AUTO_WATCH=1 \
		-o $@ \
		game_pad_bridge_main.c wireless_ds4.c $(LDFLAGS) $(BRIDGE_LIBS)

$(EXP_TARGET): $(TARGET)
	cp -f $(TARGET) $(EXP_TARGET)

status: $(STATUS_TARGET)

$(STATUS_TARGET): game_pad_bridge_status_main.c wireless_ds4.c $(COMMON_HEADERS)
	$(CC) $(CFLAGS) -o $@ game_pad_bridge_status_main.c wireless_ds4.c \
		$(LDFLAGS) $(RUNTIME_LIBS)

stop: $(STOP_TARGET)

$(STOP_TARGET): stop_game_pad_bridge_main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(RUNTIME_LIBS)

release: all status stop
	$(STRIP) --strip-debug $(TARGET) $(STATUS_TARGET) $(STOP_TARGET)

audit:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -File ../tests/static-audit.ps1 -HostCompiler "$(HOST_CC)"
else
	POORDS4_HOST_CC="$(HOST_CC)" $(PYTHON) -m unittest discover -s ../tests -p 'test_*.py' -v
endif

clean:
	rm -f $(TARGET) $(EXP_TARGET) $(STATUS_TARGET) $(STOP_TARGET)

deploy: $(TARGET)
ifndef PS5_HOST
	$(error PS5_HOST is not set)
endif
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
