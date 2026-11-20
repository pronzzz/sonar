# Sonar OS — build and run
#
#   make                 build build/Image (the kernel; also a UEFI application)
#   make run             boot in QEMU, serial console in this terminal
#   make run-desktop     boot in QEMU with a display window: the Sonar desktop
#   make esp             build build/esp.img, a UEFI-bootable disk (USB stick image)
#   make run-uefi        boot build/esp.img through UEFI firmware (edk2) in QEMU
#   make smoke           boot once and check the shell, file system and CPUs
#   make test-host       build the host-side AI engine test and run the TCP tests
#   make test-crypto     check kernel/crypto against OpenSSL-made test vectors
#   make test-update     signed-update test in QEMU (UEFI): install, verify, reboot
#
#   MODEL=path/to/model.gguf   attach a DeepSeek GGUF model (run, run-desktop, esp)
#   RAM=8G CPUS=4              guest memory and processors
#   ESP_SYSTEM=1               make esp/run-uefi: put the system disk in its own
#                              partition (kept across boots, needed for updates)
#   VERSION=x.y.z              build with another version number (update tests)
#   UPDATE_KEY=path/key.h      build with another update-signing public key
#
# Requires clang + ld.lld (LLVM), qemu-system-aarch64 and python3.

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  BREW ?= /opt/homebrew
  LLVM_BIN ?= $(BREW)/opt/llvm/bin
  LLD_BIN ?= $(BREW)/opt/lld/bin
else
  LLVM_BIN ?= /usr/bin
  LLD_BIN ?= /usr/bin
endif

CC      := $(LLVM_BIN)/clang
LD      := $(LLD_BIN)/ld.lld
OBJCOPY := $(LLVM_BIN)/llvm-objcopy
READELF := $(LLVM_BIN)/llvm-readelf
HOSTCC  ?= clang

BUILD := build
KSRC_C := $(shell find kernel -name '*.c' | sort)
KSRC_S := $(shell find kernel -name '*.S' | sort)
KOBJ   := $(KSRC_C:%.c=$(BUILD)/%.o) $(KSRC_S:%.S=$(BUILD)/%.o)

COMMON := --target=aarch64-none-elf -march=armv8-a -ffreestanding -fno-builtin \
          -fPIE -fvisibility=hidden -fno-stack-protector -fno-omit-frame-pointer \
          -fno-strict-aliasing -fno-common -mno-outline-atomics -fno-unwind-tables \
          -fno-asynchronous-unwind-tables -g -std=gnu11 \
          -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -Werror=implicit-function-declaration -Werror=return-type \
          -Werror=incompatible-pointer-types -Werror=int-conversion \
          -Ikernel/include -MMD -MP

# Kernel core may run before the MMU is on (Device memory: no unaligned
# accesses), so it is built with -mstrict-align. The AI engine only runs
# with caches on and is built for speed.
KCFLAGS  := $(COMMON) -O2 -mstrict-align
AICFLAGS := $(COMMON) -O3 -fno-math-errno -DSONAR_KERNEL -Ikernel/ai

# The portable engine (also built on the host by test-host).
AI_ENGINE := aimath gguf quant qdot tokenizer model sampler chat

# Test builds of the updater (tools/test_update.py): another version number
# or signing key. Only taken from the command line.
ifeq ($(origin VERSION),command line)
  KCFLAGS += -DOS_VERSION='"$(VERSION)"'
endif
ifneq ($(UPDATE_KEY),)
  KCFLAGS += -I. -DUPDATE_KEY_FILE='"$(UPDATE_KEY)"'
endif
ifneq ($(TEST_FAIL_BOOT),)
  KCFLAGS += -DSONAR_TEST_FAIL_BOOT
endif

LDFLAGS := -T kernel/arch/arm64/linker.ld --pie --no-dynamic-linker -z notext \
           -z norelro --build-id=none --no-undefined -nostdlib -static

.PHONY: all clean run run-tcg run-desktop esp run-uefi smoke test-host test-net test-crypto \
        test-update
all: $(BUILD)/Image

$(BUILD)/kernel/ai/%.o: kernel/ai/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(AICFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS  $<"
	@$(CC) $(KCFLAGS) -D__ASSEMBLY__ -c $< -o $@

$(BUILD)/kernel.elf: $(KOBJ) kernel/arch/arm64/linker.ld
	@echo "  LD  $@"
	@$(LD) $(LDFLAGS) -o $@ $(KOBJ)
	@# Boot code only applies R_AARCH64_RELATIVE; refuse anything else.
	@if $(READELF) -r $@ | grep -E '^[0-9a-f]+ ' | grep -v R_AARCH64_RELATIVE | grep -q .; then \
	  echo "error: kernel has non-RELATIVE dynamic relocations:"; \
	  $(READELF) -r $@ | grep -v R_AARCH64_RELATIVE; rm -f $@; exit 1; fi

$(BUILD)/Image: $(BUILD)/kernel.elf
	@echo "  BIN $@"
	@$(OBJCOPY) -O binary $< $@
	@ls -l $@ | awk '{printf "  Image: %d bytes\n", $$5}'

clean:
	rm -rf $(BUILD)

-include $(KOBJ:.o=.d)

# ---------------------------------------------------------------- running
QEMU    ?= qemu-system-aarch64
CPUS    ?= 4
RAM     ?= 2G
ACCEL   ?= $(shell if [ "$(UNAME)" = Darwin ] && [ "$$(uname -m)" = arm64 ]; then echo hvf; else echo tcg; fi)
ifeq ($(ACCEL),hvf)
  QCPU := -accel hvf -cpu host
else
  QCPU := -accel tcg,thread=multi -cpu cortex-a76
endif

# The system disk keeps files and settings between boots. It is created on
# first use and never overwritten by make. A UEFI boot holds it in RAM.
SYSTEM_IMG  ?= $(BUILD)/system.img
SYSTEM_SIZE ?= 512M
MODEL       ?=

QMACHINE := -machine virt,gic-version=3 $(QCPU) -smp $(CPUS) -m $(RAM) -no-reboot \
            -global virtio-mmio.force-legacy=false
QDISKS   := -drive file=$(SYSTEM_IMG),format=raw,if=none,id=sys -device virtio-blk-device,drive=sys
ifneq ($(MODEL),)
  QDISKS += -drive file=$(MODEL),format=raw,if=none,id=model,readonly=on \
            -device virtio-blk-device,drive=model
endif
QNET     := -netdev user,id=net0 -device virtio-net-device,netdev=net0 -device virtio-rng-device
QDISPLAY := -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device

$(SYSTEM_IMG):
	@mkdir -p $(dir $@)
	python3 tools/sonarfs.py mkimage $@ $(SYSTEM_SIZE) --label SYSTEM

# Serial console in this terminal (Ctrl-A X quits QEMU).
run: $(BUILD)/Image $(SYSTEM_IMG)
	$(QEMU) $(QMACHINE) -kernel $(BUILD)/Image -nographic $(QDISKS) $(QNET) $(QEMU_EXTRA)

run-tcg: $(BUILD)/Image
	$(MAKE) run ACCEL=tcg

# A display window with the desktop; the serial console stays in this
# terminal (Ctrl-A C switches to the QEMU monitor).
run-desktop: $(BUILD)/Image $(SYSTEM_IMG)
	$(QEMU) $(QMACHINE) -kernel $(BUILD)/Image -serial mon:stdio $(QDISKS) $(QNET) \
	  $(QDISPLAY) $(QEMU_EXTRA)

# UEFI boot disk: GPT + FAT32 EFI system partition holding the kernel as
# \EFI\BOOT\BOOTAA64.EFI, the system image and (with MODEL=) the model.
# Write it to a USB stick to boot a UEFI arm64 computer (docs/hardware.md).
ESP_IMG ?= $(BUILD)/esp.img
ESP_SYSTEM ?=
esp: $(BUILD)/Image $(SYSTEM_IMG)
	python3 tools/mkesp.py $(ESP_IMG) EFI/BOOT/BOOTAA64.EFI=$(BUILD)/Image \
	  $(if $(ESP_SYSTEM),--system $(SYSTEM_IMG),sonar/system.img=$(SYSTEM_IMG)) \
	  $(if $(MODEL),sonar/model.gguf=$(MODEL))

# UEFI firmware for QEMU's virt machine (edk2). QEMU ships one; distribution
# packages may put it elsewhere.
EDK2 ?= $(firstword $(wildcard $(BREW)/share/qemu/edk2-aarch64-code.fd \
          /usr/share/qemu/edk2-aarch64-code.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
          /usr/share/AAVMF/AAVMF_CODE.fd))

# acpi=off: Sonar reads the device tree, which edk2 only passes on when the
# machine has no ACPI tables. ramfb gives the firmware a GOP framebuffer.
run-uefi: esp
	@test -n "$(EDK2)" || { echo "no edk2 firmware found; set EDK2=path/to/edk2-aarch64-code.fd"; exit 1; }
	$(QEMU) -machine virt,gic-version=3,acpi=off $(QCPU) -smp $(CPUS) -m $(RAM) -no-reboot \
	  -global virtio-mmio.force-legacy=false -bios $(EDK2) -serial mon:stdio \
	  -drive file=$(ESP_IMG),format=raw,if=none,id=esp -device virtio-blk-device,drive=esp \
	  -device ramfb -device virtio-keyboard-device -device virtio-tablet-device $(QNET) $(QEMU_EXTRA)

# Boot with a RAM disk and check the basics over the serial console.
smoke: $(BUILD)/Image
	printf '#wait Press Enter\n#send \ninfo\nfstest\nsmptest\nls /\nuptime\n' | \
	  python3 tools/qemu_session.py \
	  --timeout 120 --expect 'Sonar 0.1.0 for aarch64' --expect 'all verified' \
	  --expect 'results match' -- $(QEMU) $(QMACHINE) -kernel $(BUILD)/Image -nographic
	@echo "smoke test passed"

# ------------------------------------------------------------- host tests
# Builds the same engine sources for the development machine so they can be
# checked against real GGUF files at full speed.
HOST_AIFLAGS := -O3 -g -std=gnu11 -fno-math-errno -Wall -Wextra -Wno-unused-parameter \
                -Wno-sign-compare -Ikernel/ai -pthread
$(BUILD)/aitest: $(AI_ENGINE:%=kernel/ai/%.c) tests/host/aitest.c tests/host/host_platform.c \
                 $(wildcard kernel/ai/*.h)
	@mkdir -p $(BUILD)
	@echo "  HOSTCC $@"
	@$(HOSTCC) $(HOST_AIFLAGS) -o $@ $(AI_ENGINE:%=kernel/ai/%.c) tests/host/aitest.c \
	  tests/host/host_platform.c

test-host: $(BUILD)/aitest test-net test-crypto
	@echo "built $(BUILD)/aitest; run: $(BUILD)/aitest MODEL.gguf --chat 'hello'"

# The kernel's TCP against a simulated lossy network (tests/host/tcptest.c).
$(BUILD)/tcptest: kernel/net/tcp.c tests/host/tcptest.c kernel/include/net.h kernel/net/netpriv.h
	@mkdir -p $(BUILD)
	@echo "  HOSTCC $@"
	@$(HOSTCC) -O1 -g -std=gnu11 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-macro-redefined \
	  -fsanitize=address,undefined -iquote kernel/include -o $@ kernel/net/tcp.c \
	  tests/host/tcptest.c

test-net: $(BUILD)/tcptest
	@$(BUILD)/tcptest

# kernel/crypto against vectors computed with OpenSSL (tools/gen_crypto_vectors.py).
CRYPTO_SRC := kernel/crypto/sha2.c kernel/crypto/chacha.c kernel/crypto/curve25519.c
$(BUILD)/cryptotest: $(CRYPTO_SRC) tests/host/cryptotest.c tests/host/crypto_vectors.h \
                     kernel/include/crypto.h
	@mkdir -p $(BUILD)
	@echo "  HOSTCC $@"
	@$(HOSTCC) -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	  -fsanitize=address,undefined -iquote kernel/include -iquote tests/host -o $@ \
	  $(CRYPTO_SRC) tests/host/cryptotest.c

test-crypto: $(BUILD)/cryptotest
	@$(BUILD)/cryptotest

# Signed updates end to end: two kernels signed with a throwaway key, a
# local stand-in for GitHub's release API over HTTPS, UEFI boots in QEMU.
test-update:
	python3 tools/test_update.py
