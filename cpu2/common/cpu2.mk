# ============================================================================
# cpu2/common/cpu2.mk -- shared build rules of the two CPU2 (Cortex-M0+) images
#
# Included by cpu2/satellite/Makefile and cpu2/ground_station/Makefile, which set
#   TARGET          output name; the build goes to <repo>/build/<TARGET>/ (<TARGET>.bin/.elf/.map, objects, .opts)
#   TARGET_NAME     human readable name for the banner
#   TARGET_SRCS     sources of the image's own app/ directory (and anything else target specific)
#   TARGET_DEFINES  extra -D flags
#   TARGET_OPTS     option string that forces a clean rebuild when it changes (see build/<TARGET>/.opts)
#
# Targets: all (default) | flash | size | clean | probes | c2boot | flash-m4 | compile_flags
# Options come from the generated .mk of the configuration (make configure, TUI), command line wins:
#   AUTH=TRUE|FALSE  DEBUG=T|F  FRONTEND=NUCLEO|CUSTOM (satellite)  CUBUS=T|F (satellite: the RSMS CUBUS V.3 flight board)  LOG=1|0 (satellite)  DIGI=TRUE|FALSE (satellite)
#   TIMER_IF_USE_RTC=0|1  SERIAL=<probe>
# ============================================================================

COMMON  := ../common
REPO    := ../..
# ST-generated / imported headers (CMSIS, HAL, ST radio + utilities, board glue) live in <repo>/include/<group>/ ; our own headers stay next to the sources
INC     := $(REPO)/include
# every object and output of an image: <repo>/build/satellite/ or <repo>/build/ground_station/
BUILD   := $(REPO)/build/$(TARGET)

# stray CPATH/C_INCLUDE_PATH (e.g. from a Python framework install) breaks the ARM toolchain's own header lookup
unexport CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH

# ---------------------------------------------------------------------------
# generated configuration: generated/{s2s2_config.h, s2s2.mk, secrets.h} (created and refreshed here, so a plain
# `make` in this directory works without `make configure`)
# ---------------------------------------------------------------------------
GEN_DIR := $(REPO)/generated
GEN_OUT := $(shell python3 $(REPO)/tools/config/s2s2_core.py gen --quiet 2>&1)
ifneq ($(strip $(GEN_OUT)),)
  $(error configuration: $(GEN_OUT))
endif
-include $(GEN_DIR)/s2s2.mk

# ---------------------------------------------------------------------------
# toolchain: ARM_TOOLCHAIN_BIN (env or make var), else the official ARM GNU Toolchain if installed (Homebrew's
# 16.x arm-none-eabi-gcc cannot find its own newlib headers), else whatever is on PATH
# ---------------------------------------------------------------------------
ARM_TOOLCHAIN_BIN ?=
ifeq ($(ARM_TOOLCHAIN_BIN),)
  ARM_TOOLCHAIN_BIN := $(firstword $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin))
endif
ifneq ($(ARM_TOOLCHAIN_BIN),)
  TC_PREFIX := $(ARM_TOOLCHAIN_BIN)/
endif
CC      = $(TC_PREFIX)arm-none-eabi-gcc
AS      = $(TC_PREFIX)arm-none-eabi-gcc
OBJCOPY = $(TC_PREFIX)arm-none-eabi-objcopy
SIZE    = $(TC_PREFIX)arm-none-eabi-size

# ---------------------------------------------------------------------------
# options
# ---------------------------------------------------------------------------
# AUTH: integrity tag + replay counter on short packets (protocol/seal_wrap.c, seal_tag.S, key in generated/secrets.h)
AUTH ?= TRUE
ifneq ($(filter F FALSE 0 N NO f false n no,$(AUTH)),)
  AUTH_EFFECTIVE = FALSE
  AUTH_DEFINE = -DAUTH_ENABLE=0
else
  AUTH_EFFECTIVE = TRUE
  AUTH_DEFINE = -DAUTH_ENABLE=1
endif

# DEBUG=T: -Og -g3 for stepping (the satellite does not fit 56 KB with it). DEBUG=F (default): -Os.
DEBUG ?= F
ifneq ($(filter T t 1 Y y YES yes TRUE true,$(DEBUG)),)
  DEBUG_ON := 1
  OPT_FLAGS = -Og -g3
else
  DEBUG_ON := 0
  OPT_FLAGS = -Os
endif

# FRONTEND=NUCLEO (default) / CUSTOM: RF front end the satellite image targets (stock Nucleo-WL55JC vs the reference board)
FRONTEND ?= NUCLEO
ifeq ($(FRONTEND),NUCLEO)
  FRONTEND_DEFINE = -DSAT_NUCLEO_FRONTEND=1
endif

# CUBUS=T: build the satellite for the real flight board, RSMS CUBUS V.3 (COM + OBC + EPS bus board, schematic "RSMS CUBUS V.3 COM SCH").
# It selects the CUBUS board profile: RFO_LP -> ADL5324 PA (3.3 V for CW, 5 V for GMSK), RF switch FE_CTRL1/2, the 5 V rail (PA0) owned by CPU2 as in the reference,
# no Nucleo LEDs (PB15/PB9/PB11 are SPI lines there), no PB6 UART (kill switch line). CUBUS=T replaces FRONTEND. Satellite image only; the ground
# station is always a Nucleo. Details: docs/CUBUS.md.
CUBUS ?= F
CUBUS_ON := 0
ifeq ($(TARGET),satellite)
  ifneq ($(filter T t 1 Y y YES yes TRUE true,$(CUBUS)),)
    CUBUS_ON := 1
    ifneq ($(FRONTEND),CUBUS)
      ifneq ($(origin FRONTEND),file)
        $(warning CUBUS=T replaces FRONTEND=$(FRONTEND))
      endif
    endif
    override FRONTEND := CUBUS
    FRONTEND_DEFINE = -DSAT_CUBUS=1
  endif
endif

# LOG=1 (default): full satellite console log. LOG=0 compiles it out but is NOT usable yet (open bug: downlink corrupted)
LOG ?= 1
ifeq ($(LOG),0)
  LOG_DEFINE = -DSAT_LOG_QUIET=1
endif

TIMER_IF_USE_RTC ?= 0

# DIGI=TRUE (default): the satellite digipeater (cpu2/common/digipeater + app/satellite_digi.c); the satellite Makefile adds the sources
DIGI ?= TRUE
ifneq ($(filter F FALSE 0 N NO f false n no,$(DIGI)),)
  DIGI_ON = 0
else
  DIGI_ON = 1
endif

# The options are -D flags, not tracked per object: when they change, all objects are deleted.
BUILD_OPTS := $(AUTH_EFFECTIVE)-$(DEBUG_ON)-$(TIMER_IF_USE_RTC)-$(DIGI_ON)-$(TARGET_OPTS)
ifneq ($(BUILD_OPTS),$(shell cat $(BUILD)/.opts 2>/dev/null))
  $(shell mkdir -p $(BUILD); find $(BUILD) -name '*.o' -delete; echo '$(BUILD_OPTS)' > $(BUILD)/.opts)
endif

# probe selection: SERIAL=<st-link serial> (see `make probes`); required when more than one ST-Link is connected
SERIAL ?=
ifneq ($(SERIAL),)
  OPENOCD_HLA_SERIAL := -c "adapter serial $(SERIAL)"
  STM32PROG_SN := sn=$(SERIAL)
endif

# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------
CPU     = -mcpu=cortex-m0plus -mthumb
LDSCRIPT = boot/wl55jc_cm0p.ld

# identical in both images (cpu2/common)
COMMON_SRCS = \
	$(COMMON)/protocol/ax25/ax25.c \
	$(COMMON)/protocol/ax25/crc16.c \
	$(COMMON)/protocol/g3ruh/g3ruh.c \
	$(COMMON)/ipc/Sring_buffer.c \
	$(COMMON)/system/mini_printf.c \
	$(COMMON)/system/stm32_seq.c \
	$(COMMON)/system/stm32_lpm.c \
	$(COMMON)/system/stm32_mem.c \
	$(COMMON)/system/rtc.c \
	$(COMMON)/system/ll_port.c \
	$(COMMON)/system/error_handler.c \
	$(COMMON)/system/trace_stub.c \
	$(COMMON)/vendor/st_radio/radio_fw.c \
	$(COMMON)/vendor/st_radio/stm32_timer.c \
	$(COMMON)/vendor/stm32wlxx_hal/Src/stm32wlxx_hal_rcc.c \
	$(COMMON)/drivers/subghz/subghz.c

# same names in both image directories, different content (board glue, radio driver, mission protocol, uart)
LOCAL_SRCS = \
	mission/protocol.c \
	mission/radio_app.c \
	uart/uart_debug.c \
	system/stm32wlxx_it.c \
	system/system_stm32wlxx.c \
	system/timer_if.c \
	radio/radio.c \
	radio/radio_driver.c \
	radio/sys_app.c \
	board/radio_board_if.c \
	board/stm32wlxx_nucleo_radio.c

C_SRCS = $(TARGET_SRCS) $(LOCAL_SRCS) $(COMMON_SRCS)
S_SRCS = boot/startup_cm0p.s

# the RTC-backed UTIL_TIMER tick is dormant (see system/timer_if.c); TIMER_IF_USE_RTC=1 builds it
ifeq ($(TIMER_IF_USE_RTC),1)
  C_SRCS += $(addprefix $(COMMON)/vendor/stm32wlxx_hal/Src/,stm32wlxx_hal_rtc.c stm32wlxx_hal_rtc_ex.c stm32wlxx_hal_rcc_ex.c)
endif

# the integrity check is not linked at all with AUTH=FALSE
ifeq ($(AUTH_EFFECTIVE),TRUE)
  C_SRCS += $(COMMON)/protocol/seal_wrap.c
  S_SRCS += $(COMMON)/protocol/seal_tag.S
endif

# object of a source: ../common/x/y.c -> $(BUILD)/common/x/y.o ; app/z.c -> $(BUILD)/app/z.o
objof = $(addprefix $(BUILD)/,$(patsubst ../%,%,$(addsuffix .o,$(basename $(1)))))
ALL_OBJS = $(call objof,$(C_SRCS) $(S_SRCS))

# ---------------------------------------------------------------------------
# flags
# ---------------------------------------------------------------------------
INCLUDES = \
	-I. -Iapp -Imission -Iboard -Isystem -Iradio -Iuart -Iipc -I$(INC)/$(TARGET) -I$(GEN_DIR) \
	-I$(COMMON) -I$(COMMON)/protocol -I$(COMMON)/protocol/ax25 -I$(COMMON)/protocol/g3ruh \
	-I$(COMMON)/system/include -I$(COMMON)/system -I$(COMMON)/digipeater \
	-I$(INC)/st_system -I$(INC)/st_radio -I$(INC)/stm32wlxx_hal -I$(INC)/st_bsp -I$(COMMON)/ipc \
	-I$(INC)/st_utilities \
	-I$(INC)/cmsis/Device/ST/STM32WLxx/Include -I$(INC)/cmsis/Core/Include

CFLAGS = $(CPU) $(OPT_FLAGS) $(AUTH_DEFINE) -DTIMER_IF_USE_RTC=$(TIMER_IF_USE_RTC) -DDIGI_ENABLE=$(DIGI_ON) $(TARGET_DEFINES) $(FRONTEND_DEFINE) $(LOG_DEFINE) \
	-MMD -MP -Wall -Wextra -g -fdata-sections -ffunction-sections \
	-DUSE_HAL_DRIVER -DSTM32WL55xx -DCORE_CM0PLUS $(INCLUDES)
ASFLAGS = $(CPU) -g $(AUTH_DEFINE)
LDFLAGS = $(CPU) -T $(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs --specs=nosys.specs -nostartfiles

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
all: $(BUILD)/$(TARGET).bin

$(BUILD)/$(TARGET).elf: $(ALL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo ""
	@echo "========================================"
	@echo " M0+ BUILD SUCCESS: $(TARGET_NAME)"
	@echo "========================================"
	@echo " ELF : $(BUILD)/$(TARGET).elf"
	@echo " BIN : $(BUILD)/$(TARGET).bin"
	@echo " MAP : $(BUILD)/$(TARGET).map"
	@echo " Flash address: 0x08032000"
	@$(if $(filter satellite,$(TARGET)),echo " Board        : $(if $(filter 1,$(CUBUS_ON)),RSMS CUBUS V.3 (CUBUS=T),$(FRONTEND) front end)",true)
	@echo "========================================"
	@$(MEMORY_SUMMARY)

# CPU2 memory summary: 56 KB flash at 0x08032000, 28 KB RAM
define MEMORY_SUMMARY
$(SIZE) $(BUILD)/$(TARGET).elf | awk 'NR==2 { \
	text=$$1; data=$$2; bss=$$3; \
	flash=text+data; ram=data+bss; \
	c2_flash=56*1024; c2_ram=28*1024; \
	f_used=flash; f_free=c2_flash-flash; f_pct=f_used*100/c2_flash; f_free_pct=100-f_pct; \
	r_used=ram; r_free=c2_ram-ram; r_pct=r_used*100/c2_ram; r_free_pct=100-r_pct; \
	printf "\n==================================== CPU2 (M0+) MEMORY USAGE SUMMARY ====================================\n"; \
	printf "  %-18s  %-19s  %-21s  %-20s  %7s  %7s\n", "Memory Region", "Total Capacity", "Used Space", "Free / Available", "Used %", "Free %"; \
	printf "  ------------------  -------------------  ---------------------  --------------------  -------  -------\n"; \
	printf "  %-18s  %5.1f KB (%6d B)  %5.1f KB (%6d B)  %5.1f KB (%6d B)   %5.1f%%   %5.1f%%\n", "CPU2 Flash (M0+)", c2_flash/1024, c2_flash, f_used/1024, f_used, f_free/1024, f_free, f_pct, f_free_pct; \
	printf "  %-18s  %5.1f KB (%6d B)  %5.1f KB (%6d B)  %5.1f KB (%6d B)   %5.1f%%   %5.1f%%\n", "CPU2 RAM (SRAM2)", c2_ram/1024, c2_ram, r_used/1024, r_used, r_free/1024, r_free, r_pct, r_free_pct; \
	printf "  %-18s  %5.1f KB (%6d B)  [  Reserved for M4 <-> M0+ Shared Ringbuffer at 0x20008000  ]\n", "Shared IPC SRAM2", 4.0, 4096; \
	printf "==========================================================================================================\n\n"; \
}'
endef

size: $(BUILD)/$(TARGET).elf
	@$(MEMORY_SUMMARY)

# local sources -> $(BUILD)/<dir>/x.o ; common sources -> $(BUILD)/common/<dir>/x.o
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/common/%.o: $(COMMON)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/common/%.o: $(COMMON)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -MMD -MP -c $< -o $@

# ---------------------------------------------------------------------------
# flash CPU2 only (0x08032000 - 0x0803FFFF = flash sectors 100-127, 56 KB); CPU1 / Cortex-M4 at 0x08000000 is preserved
# ---------------------------------------------------------------------------
STM32_PROG ?= $(shell which STM32_Programmer_CLI 2>/dev/null || \
	ls /Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI 2>/dev/null || \
	ls $(HOME)/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI 2>/dev/null || \
	ls /opt/st/stm32cubeprogrammer_*/bin/STM32_Programmer_CLI 2>/dev/null | head -n 1 || \
	echo STM32_Programmer_CLI)

BIN = $(BUILD)/$(TARGET).bin
OPENOCD_FLASH = openocd -f interface/stlink.cfg -c "transport select hla_swd" $(OPENOCD_HLA_SERIAL) -f target/stm32wlx.cfg \
	-c "reset_config none" -c "program $(BIN) 0x08032000 verify reset exit"
OPENOCD_FLASH_DAP = openocd -f interface/stlink-dap.cfg $(OPENOCD_HLA_SERIAL) -f target/stm32wlx.cfg \
	-c "reset_config none" -c "program $(BIN) 0x08032000 verify reset exit"

flash: $(BIN)
	@if [ -x "$$(which $(STM32_PROG) 2>/dev/null)" ] || [ -x "$(STM32_PROG)" ]; then \
	  echo "Flashing $(TARGET_NAME) $(BIN) to 0x08032000 using STM32_Programmer_CLI (CPU1 preserved)..."; \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=UR -w $(BIN) 0x08032000 -v -hardRst 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=HOTPLUG -w $(BIN) 0x08032000 -v -hardRst 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) -w $(BIN) 0x08032000 -v -hardRst || \
	  (echo "STM32_Programmer_CLI failed. Trying OpenOCD fallback..." && $(OPENOCD_FLASH) 2>/dev/null || $(OPENOCD_FLASH_DAP)); \
	  echo "Booting CPU2 (Cortex-M0+) via PWR_CR4_C2BOOT..."; \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) -w32 0x5800040C 0x00008000 2>/dev/null || true; \
	else \
	  echo "Flashing $(TARGET_NAME) $(BIN) to 0x08032000 using OpenOCD (CPU1 preserved)..."; \
	  $(OPENOCD_FLASH) 2>/dev/null || $(OPENOCD_FLASH_DAP); \
	fi

# ---------------------------------------------------------------------------
# probes, option bytes, CPU1 image
# ---------------------------------------------------------------------------
NUTTX_BIN ?= $(REPO)/nuttx/nuttx.bin

# read from the USB registry: `st-info --probe` would open every ST-Link and reset the board behind it
probes:
	@echo "Connected ST-Link probes (USB registry):"
	@python3 $(REPO)/tools/config/s2s2_tui.py --probes

# CPU2 boot vector: C2OPT=1 (flash) and SBRV=0xC800 -> 0x08032000
c2boot:
	@$(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=UR -ob C2OPT=1 SBRV=0xC800

# CPU1 (Cortex-M4 / NuttX) image: boots CPU2 and runs the telemetry / image source
flash-m4:
	@test -f $(NUTTX_BIN) || (echo "Missing $(NUTTX_BIN). Build NuttX first (make nuttx)" && exit 1)
	@echo "Flashing $(NUTTX_BIN) to 0x08000000 (CPU1 / M4)..."
	@$(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=UR -w $(NUTTX_BIN) 0x08000000 -v -hardRst 2>/dev/null || \
	  openocd -f interface/stlink.cfg -c "transport select hla_swd" $(OPENOCD_HLA_SERIAL) \
	    -f target/stm32wlx.cfg -c "reset_config none" -c "init" -c "reset halt" \
	    -c "program $(NUTTX_BIN) 0x08000000 verify" -c "reset run" -c "exit"

# compile_flags.txt for clangd / IDE header resolution
compile_flags:
	@printf '%s\n' $(filter -I% -D%,$(CFLAGS)) > compile_flags.txt
	@echo "wrote compile_flags.txt"

clean:
	rm -rf $(BUILD)

-include $(ALL_OBJS:.o=.d)

.PHONY: all size flash probes c2boot flash-m4 compile_flags clean
