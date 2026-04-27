# A/B secure bootloader for QEMU virt (ARMv7-A / Cortex-A15)
#
#   make            build bootloader, all app releases, flash images
#   make test       host crypto cross-check against Python `cryptography`
#   make run        boot in QEMU interactively (Ctrl-A X to quit)
#   make demo       scripted end-to-end OTA / rollback / attack scenarios
#   make clean      remove build/ (keeps keys/)

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size
PYTHON  ?= python3
QEMU    ?= qemu-system-arm
HOSTCC  ?= gcc

B := build

ARCHFLAGS := -mcpu=cortex-a15 -marm -mfloat-abi=soft -mno-unaligned-access
CFLAGS    := $(ARCHFLAGS) -std=c11 -O2 -g -Wall -Wextra -Werror \
             -ffreestanding -fno-common -ffunction-sections -fdata-sections \
             -Icommon -Icrypto -I$(B)
LDFLAGS   := $(ARCHFLAGS) -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segments

COMMON_SRC := common/util.c common/flash.c common/meta.c common/wdt.c
CRYPTO_SRC := crypto/sha512.c crypto/ed25519.c

SIGN_KEY     := keys/signing_key.pem
ATTACKER_KEY := keys/attacker_key.pem
PUBKEY_H     := $(B)/pubkey.h

# release name : version : fault injection
RELEASES := \
    app-1.0.0:1.0.0:FAULT_NONE \
    app-1.1.0:1.1.0:FAULT_NONE \
    app-1.2.0-hang:1.2.0:FAULT_HANG \
    app-1.2.1-crash:1.2.1:FAULT_CRASH \
    app-1.2.2-selftest:1.2.2:FAULT_SELFTEST \
    app-1.3.0:1.3.0:FAULT_NONE

rel_name = $(word 1,$(subst :, ,$1))
rel_ver  = $(word 2,$(subst :, ,$1))
rel_flt  = $(word 3,$(subst :, ,$1))

IMAGES := $(foreach r,$(RELEASES),$(B)/$(call rel_name,$r).img) \
          $(B)/app-1.3.0-tampered.img $(B)/app-1.3.0-wrongkey.img

all: $(B)/flash0.bin $(B)/flash1-factory.bin $(IMAGES)
	@$(SIZE) $(B)/bootloader.elf $(B)/app-1.0.0.elf

# ---------------- keys ----------------

$(SIGN_KEY):
	@mkdir -p keys
	$(PYTHON) tools/imgtool.py keygen $@

$(ATTACKER_KEY):
	@mkdir -p keys
	$(PYTHON) tools/imgtool.py keygen $@

$(PUBKEY_H): $(SIGN_KEY) | $(B)
	$(PYTHON) tools/imgtool.py pubhdr $< $@

$(B):
	@mkdir -p $(B)

# ---------------- bootloader ----------------

BL_SRC := bootloader/start.S bootloader/boot.c $(COMMON_SRC) $(CRYPTO_SRC)

$(B)/bootloader.elf: $(BL_SRC) bootloader/bootloader.ld $(PUBKEY_H) $(wildcard common/*.h crypto/*.h)
	$(CC) $(CFLAGS) $(LDFLAGS) -T bootloader/bootloader.ld $(BL_SRC) -lgcc -o $@

$(B)/bootloader.bin: $(B)/bootloader.elf
	$(OBJCOPY) -O binary $< $@

$(B)/flash0.bin: $(B)/bootloader.bin
	$(PYTHON) tools/imgtool.py mkflash0 $< $@

# ---------------- application releases ----------------

APP_SRC := app/start.S app/main.c $(COMMON_SRC)

define RELEASE_RULES
$(B)/$(call rel_name,$1).elf: $(APP_SRC) app/app.ld $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) $(LDFLAGS) -T app/app.ld \
	    -DAPP_FAULT=$(call rel_flt,$1) \
	    -DAPP_TAG='"$(if $(filter FAULT_NONE,$(call rel_flt,$1)),release,$(subst FAULT_,,$(call rel_flt,$1)))"' \
	    $(APP_SRC) -lgcc -o $$@

$(B)/$(call rel_name,$1).bin: $(B)/$(call rel_name,$1).elf
	$(OBJCOPY) -O binary $$< $$@

$(B)/$(call rel_name,$1).img: $(B)/$(call rel_name,$1).bin $(SIGN_KEY)
	$(PYTHON) tools/imgtool.py sign --key $(SIGN_KEY) --version $(call rel_ver,$1) $$< $$@
endef
$(foreach r,$(RELEASES),$(eval $(call RELEASE_RULES,$r)))

# attacker scenarios: payload modified after signing / signed with the wrong key
$(B)/app-1.3.0-tampered.img: $(B)/app-1.3.0.img
	$(PYTHON) -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[256+0x40]^=0x01; open(sys.argv[2],'wb').write(d)" $< $@

$(B)/app-1.3.0-wrongkey.img: $(B)/app-1.3.0.bin $(ATTACKER_KEY)
	$(PYTHON) tools/imgtool.py sign --key $(ATTACKER_KEY) --version 1.3.0 $< $@

# factory state: v1.0.0 provisioned in slot A, metadata blank
$(B)/flash1-factory.bin: $(B)/app-1.0.0.img
	$(PYTHON) tools/imgtool.py mkflash1 $@ --slot-a $<

# ---------------- run / test ----------------

QEMU_ARGS = -M virt,highmem=off,secure=off,virtualization=off -cpu cortex-a15 -m 128M \
            -nic none -display none -device i6300esb \
            -drive if=pflash,format=raw,unit=0,readonly=on,file=$(B)/flash0.bin \
            -drive if=pflash,format=raw,unit=1,file=$(B)/flash1.bin

$(B)/flash1.bin: | $(B)/flash1-factory.bin
	cp $(B)/flash1-factory.bin $@

run: all $(B)/flash1.bin
	$(QEMU) $(QEMU_ARGS) -serial mon:stdio

factory-reset: $(B)/flash1-factory.bin
	cp $(B)/flash1-factory.bin $(B)/flash1.bin

demo: all
	$(PYTHON) tools/demo.py

test: $(B)/crypto_test $(B)/vectors.bin
	$(B)/crypto_test $(B)/vectors.bin

$(B)/crypto_test: test/crypto_test.c $(CRYPTO_SRC) | $(B)
	$(HOSTCC) -O2 -Wall -Wextra -Icrypto $^ -o $@

$(B)/vectors.bin: test/gen_vectors.py | $(B)
	$(PYTHON) $< $@

clean:
	rm -rf $(B)

.PHONY: all run demo test clean factory-reset
.DELETE_ON_ERROR:
