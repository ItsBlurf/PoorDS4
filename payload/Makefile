PS5_HOST ?=
PORT     ?= 9021
RC_VERSION := 36

TARGET        := PoorDS4rc$(RC_VERSION).elf
STATUS_TARGET := PoorDS4-status.elf
STOP_TARGET   := PoorDS4-stop.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -Wshadow -Wunreachable-code \
	-Werror -g -O2 -fPIC \
	-fno-stack-protector -ffunction-sections -fdata-sections
LDFLAGS += -lScePad -lSceUserService -lpthread -ldl -Wl,--gc-sections

.PHONY: all audit clean deploy status stop

all: $(TARGET)

$(TARGET): game_pad_bridge_main.c wireless_ds4.c wireless_ds4.h pad_types.h
	$(CC) $(CFLAGS) -DPOORDS4_AUTO_WATCH=1 \
		-DPOORDS4_RC_VERSION=$(RC_VERSION) -o $@ \
		game_pad_bridge_main.c wireless_ds4.c $(LDFLAGS)

status: $(STATUS_TARGET)

$(STATUS_TARGET): game_pad_bridge_status_main.c wireless_ds4.c \
	wireless_ds4.h pad_types.h
	$(CC) $(CFLAGS) -o $@ game_pad_bridge_status_main.c wireless_ds4.c \
		$(LDFLAGS)

stop: $(STOP_TARGET)

$(STOP_TARGET): stop_game_pad_bridge_main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

audit:
	powershell -NoProfile -ExecutionPolicy Bypass \
		-File ..\tests\static-audit.ps1

clean:
	rm -f PoorDS4rc*.elf $(STATUS_TARGET) $(STOP_TARGET)

deploy: $(TARGET)
ifndef PS5_HOST
	$(error PS5_HOST is not set)
endif
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
