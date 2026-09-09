# ---------------------------------------------------------------------------
# nanoCH32H417 USB 3.0 microSD card reader
#
# Builds the two cores (V3F boot core + V5F application core) with the
# MounRiver RISC-V GCC 12 toolchain and merges them into build/merge.bin,
# which is the image WCH-LinkE / MounRiver expects at flash offset 0x00000000
# (V3F image at 0x00000, V5F image at 0x10000).
# ---------------------------------------------------------------------------

TOOLCHAIN ?= /Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/Toolchain/RISC-V Embedded GCC12/bin
OPENOCD_DIR ?= /Users/wasdwasd0105/ch32h417/MRS_Toolchain_MAC_V240/OpenOCD/OpenOCD/bin

CC      := "$(TOOLCHAIN)/riscv-wch-elf-gcc"
OBJCOPY := "$(TOOLCHAIN)/riscv-wch-elf-objcopy"
SIZE    := "$(TOOLCHAIN)/riscv-wch-elf-size"
OBJDUMP := "$(TOOLCHAIN)/riscv-wch-elf-objdump"

BUILD := build

# Same ISA options MounRiver uses for CH32H417 (rv32imac + Zb* + WCH "xw" extension)
ARCH_FLAGS := -march=rv32imac_zba_zbb_zbc_zbs_xw -mabi=ilp32 -msmall-data-limit=8 -msave-restore

# Clock tree: 400 = SYSCLK 400/V5F 400/HCLK 100 MHz (SD clock max 50 MHz)
#             480 = SYSCLK 480/V5F 240/HCLK 120 MHz (SD clock max 60 MHz)
#             600 = SYSCLK 600/V5F 300/HCLK 150 MHz (SD clock max 75 MHz)
CLOCK ?= 350
CLOCK_DEF_400 := -DSYSCLK_400M_CoreCLK_V5F_400M_V3F_100M_HSE=400000000
CLOCK_DEF_480 := -DSYSCLK_480M_CoreCLK_V5F_240M_V3F_120M_HSE=480000000
CLOCK_DEF_600 := -DSYSCLK_600M_CoreCLK_V5F_300M_V3F_150M_HSE=600000000
# Performance-mode variants (LDO_VDDK raised; datasheet: TA <= 70 C, well cooled):
#   350  -> V5F 350 MHz, HCLK 175 MHz, SDIO_CK 87.5 MHz
#   400P -> V5F 400 MHz, HCLK 200 MHz (datasheet max), SDIO_CK 100 MHz
CLOCK_DEF_350  := -DSYSCLK_350M_CoreCLK_V5F_350M_V3F_175M_HSE=350000000
CLOCK_DEF_400P := -DSYSCLK_400M_CoreCLK_V5F_400M_V3F_200M_HSE=400000000
# VIO18 I/O supply for the SD-card pins (3=3.3V, 2=2.5V, 1=1.8V, -1=leave strap default)
VIO18_LEVEL ?= 3
# Delay before the boot core touches power/IO settings or starts the V5F core (debugger attach window)
BOOT_DELAY_MS ?= 2500

COMMON_CFLAGS := $(ARCH_FLAGS) -Os -g -std=gnu99 -MMD -MP -fmessage-length=0 -fsigned-char \
                 -ffunction-sections -fdata-sections -fno-common -Wall -Wno-unused-function -Wno-comment \
                 -DRun_Core=1

COMMON_LDFLAGS := $(ARCH_FLAGS) -nostartfiles -Xlinker --gc-sections --specs=nano.specs --specs=nosys.specs

LIB_INC  := -Ilib/Core -Ilib/Debug -Ilib/Peripheral/inc -Ilib/Startup
LIB_SRCS := lib/Core/core_riscv.c lib/Debug/debug.c $(wildcard lib/Peripheral/src/*.c)

# ----------------------------- V3F (boot core) -----------------------------
V3F_SRCS := $(wildcard v3f/*.c) $(LIB_SRCS)
V3F_ASM  := lib/Startup/startup_ch32h417_v3f.S
V3F_OBJS := $(patsubst %.c,$(BUILD)/v3f/%.o,$(V3F_SRCS)) $(patsubst %.S,$(BUILD)/v3f/%.o,$(V3F_ASM))
V3F_CFLAGS := $(COMMON_CFLAGS) -DCore_V3F -DDEBUG=1 -DAPP_VIO18_LEVEL=$(VIO18_LEVEL) -DAPP_BOOT_DELAY_MS=$(BOOT_DELAY_MS) $(CLOCK_DEF_$(CLOCK)) -Iv3f $(LIB_INC)
V3F_LD := lib/Ld/V3F/Link_v3f.ld

# --------------------------- V5F (application core) ------------------------
V5F_SRCS := $(wildcard v5f/*.c) $(wildcard v5f/fatfs/*.c) $(LIB_SRCS)
V5F_ASM  := lib/Startup/startup_ch32h417_v5f.S
V5F_OBJS := $(patsubst %.c,$(BUILD)/v5f/%.o,$(V5F_SRCS)) $(patsubst %.S,$(BUILD)/v5f/%.o,$(V5F_ASM))
# DEBUG=1 selects USART1 (PA9), which is the UART wired to the on-board WCH-LinkE
V5F_CFLAGS := $(COMMON_CFLAGS) -DCore_V5F -DAPP_VIO18_LEVEL=$(VIO18_LEVEL) -DDEBUG=1 -Iv5f -Iv5f/fatfs $(LIB_INC)
V5F_LD := lib/Ld/V5F/Link_v5f.ld

.PHONY: all clean flash monitor size

all: $(BUILD)/merge.bin size

# Build-configuration stamp.  CLOCK / VIO18_LEVEL / BOOT_DELAY_MS are command-line
# variables, so changing them touches no file and make would happily relink stale
# objects (this silently flashed a 60 MHz build while CLOCK=600 was requested).
# The stamp holds the current settings and is rewritten only when they change, so
# every object depends on the configuration that produced it.
CONFIG_SIG := CLOCK=$(CLOCK) VIO18_LEVEL=$(VIO18_LEVEL) BOOT_DELAY_MS=$(BOOT_DELAY_MS)
CONFIG_STAMP := $(BUILD)/.config

.PHONY: FORCE
FORCE:

$(CONFIG_STAMP): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CONFIG_SIG)' | cmp -s - $@ || { \
	    printf '%s\n' '$(CONFIG_SIG)' > $@; \
	    echo "config changed -> $(CONFIG_SIG)"; }

$(BUILD)/v3f/%.o: %.c Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(V3F_CFLAGS) -c $< -o $@

$(BUILD)/v3f/%.o: %.S Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(V3F_CFLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/v5f/%.o: %.c Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(V5F_CFLAGS) -c $< -o $@

$(BUILD)/v5f/%.o: %.S Makefile $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(V5F_CFLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/v3f.elf: $(V3F_OBJS) $(V3F_LD)
	$(CC) $(COMMON_LDFLAGS) -T $(V3F_LD) -Wl,-Map,$(BUILD)/v3f.map $(V3F_OBJS) -o $@

$(BUILD)/v5f.elf: $(V5F_OBJS) $(V5F_LD)
	$(CC) $(COMMON_LDFLAGS) -T $(V5F_LD) -Wl,-Map,$(BUILD)/v5f.map $(V5F_OBJS) -o $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.hex: %.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/merge.bin: $(BUILD)/v3f.bin $(BUILD)/v5f.bin $(BUILD)/v3f.hex $(BUILD)/v5f.hex
	python3 tools/merge_bin.py $(BUILD)/v3f.bin $(BUILD)/v5f.bin $@

size: $(BUILD)/v3f.elf $(BUILD)/v5f.elf
	@echo "--- V3F ---"; $(SIZE) $(BUILD)/v3f.elf
	@echo "--- V5F ---"; $(SIZE) $(BUILD)/v5f.elf

flash: $(BUILD)/merge.bin
	OPENOCD_DIR="$(OPENOCD_DIR)" ./tools/flash.sh $(abspath $(BUILD)/merge.bin)

monitor:
	python3 tools/monitor.py

clean:
	rm -rf $(BUILD)

-include $(V3F_OBJS:.o=.d) $(V5F_OBJS:.o=.d)
