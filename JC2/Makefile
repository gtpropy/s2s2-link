############################################################################
# boards/arm/stm32wl5/nucleo-wl55jc/cpu2/Makefile
#
# STM32WL55 CPU2 / Cortex-M0+ standalone firmware
#
# CPU1 = Cortex-M4 / NuttX
# CPU2 = Cortex-M0+ / standalone firmware
#
# IPCC:
#
#   CH0 : M4 -> M0+
#   CH1 : M0+ -> M4
#
# Shared SRAM2:
#
#   M4 writes 8 x 80-byte packets
#   M0+ reads the packets
#
############################################################################

# A bare-metal cross build must never see the HOST toolchain's own header
# search path -- CPATH/C_INCLUDE_PATH/CPLUS_INCLUDE_PATH/OBJC_INCLUDE_PATH
# leak in from the calling shell regardless of how the compiler is invoked,
# and a stray one (e.g. a Python installer setting CPATH globally) silently
# pulls host macOS/Linux headers into an ARM Cortex-M0+ compile -- or, with
# some toolchain builds, breaks sysroot header lookup outright ("stdio.h:
# No such file or directory" even though the toolchain is installed fine).
unexport CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH

# Two different `arm-none-eabi-gcc` packages exist on macOS: Homebrew's
# formula (symlinked onto PATH, but its 16.x bottle fails to find its own
# bundled newlib headers -- the "stdio.h: No such file or directory"
# failure above) and the official ARM GNU Toolchain release (installs to
# /Applications/ArmGNUToolchain/ but isn't put on PATH). Resolution order:
# 1) ARM_TOOLCHAIN_BIN if the caller set one (env var or
#    `make ARM_TOOLCHAIN_BIN=/some/bin/dir`), 2) `.toolchain_path` if
#    ../setup.sh wrote one, 3) bare `arm-none-eabi-gcc` on PATH.
ARM_TOOLCHAIN_BIN ?=
ifeq ($(ARM_TOOLCHAIN_BIN),)
  ifneq ($(wildcard .toolchain_path),)
    ARM_TOOLCHAIN_BIN := $(shell cat .toolchain_path)
  endif
endif

APP ?= satellite

# AUTH=TRUE (default) or AUTH=FALSE -- toggles whether the light FEC
# wrap's keyed tag + replay counter (Mission/protocol.c) are computed/
# checked at all. The FEC/correction half stays on regardless; this is
# for bench testing against a peer that doesn't share the key yet.
AUTH ?= TRUE
ifeq ($(AUTH),FALSE)
  AUTH_DEFINE = -DAUTH_ENABLE=0
else
  AUTH_DEFINE = -DAUTH_ENABLE=1
endif

# Object files don't encode AUTH in their names, so switching AUTH=TRUE
# <-> AUTH=FALSE without this would silently keep stale .o files built
# under the old setting. .auth_tag records the last value used and
# forces a clean rebuild whenever it changes.
ifneq ($(AUTH),$(shell cat .auth_tag 2>/dev/null))
  $(shell find . -path "./V1.4" -prune -o -type f -name "*.o" -exec rm -f {} + ; echo $(AUTH) > .auth_tag)
endif

ifeq ($(APP),gs)
  MAIN_SRC = Main_code/Main_GS/gs_main.c
  TARGET   = gs
else
  MAIN_SRC = Main_code/Main_Satellite/satellite_main.c \
             Main_code/Main_Satellite/satellite_app.c
  TARGET   = satellite
endif


# ==========================================================================
# TOOLCHAIN
# ==========================================================================

ifeq ($(ARM_TOOLCHAIN_BIN),)
  CC      = arm-none-eabi-gcc
  AS      = arm-none-eabi-gcc
  OBJCOPY = arm-none-eabi-objcopy
  SIZE    = arm-none-eabi-size
else
  CC      = $(ARM_TOOLCHAIN_BIN)/arm-none-eabi-gcc
  AS      = $(ARM_TOOLCHAIN_BIN)/arm-none-eabi-gcc
  OBJCOPY = $(ARM_TOOLCHAIN_BIN)/arm-none-eabi-objcopy
  SIZE    = $(ARM_TOOLCHAIN_BIN)/arm-none-eabi-size
endif


# ==========================================================================
# LINKER SCRIPT (SELF-CONTAINED INSIDE CPU2)
# ==========================================================================

LDSCRIPT = scripts/wl55jc_cm0p.ld


# ==========================================================================
# CPU
# ==========================================================================

CPU = -mcpu=cortex-m0plus -mthumb


# ==========================================================================
# COMMON INCLUDE PATHS (100% LOCAL TO CPU2)
# ==========================================================================

CFLAGS = $(CPU) \
         -O2 \
         -Wall \
         -Wextra \
         -g \
         -fdata-sections \
         -ffunction-sections \
         -DUSE_HAL_DRIVER \
         -DSTM32WL55xx \
         -DCORE_CM0PLUS \
         $(AUTH_DEFINE) \
         -I. \
         -IMain_code/Main_Satellite \
         -IInc \
         -IMission \
         -Iprotocol \
         -Iprotocol/ax25 \
         -Iprotocol/g3ruh \
         -Icore \
         -Icore/Src \
         -Icore/Inc \
         -Iradio_driver \
         -Ihal \
         -Ihal/Inc \
         -Isubghz \
         -Ibsp \
         -Itarget \
         -Iipc \
         -Iutilities \
         -Iuart \
         -Idrivers/CMSIS/Device/ST/STM32WLxx/Include \
         -Idrivers/CMSIS/Core/Include


# ==========================================================================
# ASSEMBLER
# ==========================================================================

ASFLAGS = $(CPU) -g


# ==========================================================================
# LINKER
# ==========================================================================

LDFLAGS = $(CPU) \
          -T $(LDSCRIPT) \
          -Wl,--gc-sections \
          -Wl,-Map=$(TARGET).map \
          --specs=nano.specs \
          --specs=nosys.specs \
          -nostartfiles


# ==========================================================================
# C SOURCE FILES
# ==========================================================================

C_SRCS = \
$(MAIN_SRC) \
uart/uart_debug.c \
ipc/Sring_buffer.c \
protocol/ax25/ax25.c \
protocol/ax25/crc16.c \
protocol/g3ruh/g3ruh.c \
Mission/protocol.c \
Mission/radio_app.c \
protocol/stm32wlxx_it.c \
protocol/stm32_seq.c \
protocol/stm32_lpm.c \
core/stm32_mem.c \
core/system_stm32wlxx.c \
core/timer_if.c \
core/rtc.c \
core/error_handler.c \
core/stm32_adv_trace.c \
core/stm32_tiny_vsnprintf.c \
core/stm32_trace_driver.c \
radio_driver/radio.c \
radio_driver/radio_driver.c \
radio_driver/radio_fw.c \
radio_driver/stm32_timer.c \
radio_driver/sys_app.c \
target/radio_board_if.c \
subghz/subghz.c \
hal/Src/stm32wlxx_hal.c \
hal/Src/stm32wlxx_hal_subghz.c \
hal/Src/stm32wlxx_hal_rtc.c \
hal/Src/stm32wlxx_hal_rtc_ex.c \
hal/Src/stm32wlxx_hal_rcc.c \
hal/Src/stm32wlxx_hal_rcc_ex.c \
hal/Src/stm32wlxx_hal_gpio.c \
hal/Src/stm32wlxx_hal_cortex.c \
hal/Src/stm32wlxx_hal_pwr.c \
hal/Src/stm32wlxx_hal_pwr_ex.c \
bsp/stm32wlxx_nucleo_radio.c


# ==========================================================================
# STARTUP
# ==========================================================================

S_SRCS = startup_cm0p.s


# ==========================================================================
# OBJECT FILES
# ==========================================================================

C_OBJS = $(C_SRCS:.c=.o)

S_OBJS = $(S_SRCS:.s=.o)

ALL_OBJS = $(C_OBJS) $(S_OBJS)


# ==========================================================================
# DEFAULT BUILD
# ==========================================================================

ifeq ($(MAKECMDGOALS),)
all: sat gs
else ifeq ($(MAKECMDGOALS),all)
all: sat gs
else
all: $(TARGET).bin
endif


# ==========================================================================
# LINK ELF
# ==========================================================================

$(TARGET).elf: $(ALL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^


# ==========================================================================
# CREATE BIN
# ==========================================================================

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

	@echo ""
	@echo "========================================"
	@echo " M0+ BUILD SUCCESS: $(TARGET)"
	@echo "========================================"
	@echo " ELF : $(TARGET).elf"
	@echo " BIN : $(TARGET).bin"
	@echo " MAP : $(TARGET).map"
	@echo " Flash address: 0x08032000"
	@echo "========================================"

	@$(SIZE) $(TARGET).elf | awk 'NR==2 { \
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


# ==========================================================================
# COMPILE C
# ==========================================================================

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@


# ==========================================================================
# ASSEMBLE
# ==========================================================================

%.o: %.s
	$(AS) $(ASFLAGS) -c $< -o $@


# ==========================================================================
# PROGRAMMER
# ==========================================================================

STM32_PROG ?= $(shell which STM32_Programmer_CLI 2>/dev/null || ls $(HOME)/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI 2>/dev/null || ls /home/*/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI 2>/dev/null | head -n 1 || echo STM32_Programmer_CLI)

# ==========================================================================
# FLASH CPU2 (CORTEX-M0+ ONLY: 0x08032000 - 0x0803FFFF)
# This writes ONLY to sectors 100-127 (56 KB max).
# CPU1 / Cortex-M4 at 0x08000000 (sectors 0-99) is strictly preserved!
# ==========================================================================

flash: $(TARGET).bin
	@if [ -x "$$(which $(STM32_PROG) 2>/dev/null)" ] || [ -x "$(STM32_PROG)" ]; then \
	  echo "Ensuring Option Bytes: C2BOOTADD0 / SBRV -> 0x08032000..."; \
	  $(STM32_PROG) -c port=SWD mode=UR -ob C2BOOTADD0=0x08032000 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD mode=UR -ob SBRV=0x00032000 2>/dev/null || true; \
	  echo "Flashing $(TARGET).bin to 0x08032000 using STM32_Programmer_CLI (CPU1 preserved)..."; \
	  $(STM32_PROG) -c port=SWD mode=UR -w $(TARGET).bin 0x08032000 -v -hardRst 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD mode=HOTPLUG -w $(TARGET).bin 0x08032000 -v -hardRst 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD -w $(TARGET).bin 0x08032000 -v -hardRst || \
	  (echo "STM32_Programmer_CLI failed. Trying OpenOCD fallback..." && \
	  openocd \
	    -f interface/stlink.cfg \
	    -c "transport select hla_swd" \
	    -c "reset_config none" \
	    -f target/stm32wlx.cfg \
	    -c "init" \
	    -c "targets" \
	    -c "reset halt" \
	    -c "flash probe 0" \
	    -c "program $(TARGET).bin 0x08032000 verify" \
	    -c "reset run" \
	    -c "exit"); \
	else \
	  echo "Flashing $(TARGET).bin to 0x08032000 using OpenOCD (CPU1 preserved)..."; \
	  openocd \
	    -f interface/stlink.cfg \
	    -c "transport select hla_swd" \
	    -c "reset_config none" \
	    -f target/stm32wlx.cfg \
	    -c "init" \
	    -c "targets" \
	    -c "reset halt" \
	    -c "flash probe 0" \
	    -c "program $(TARGET).bin 0x08032000 verify" \
	    -c "reset run" \
	    -c "exit"; \
	fi

sat SAT:
	$(MAKE) APP=satellite satellite.bin

flash_sat flash_SAT flash_m0 flash_cpu2:
	$(MAKE) APP=satellite flash

gs GS:
	$(MAKE) APP=gs gs.bin

flash_gs flash_GS:
	$(MAKE) APP=gs flash


# ==========================================================================
# SIZE
# ==========================================================================

size: $(TARGET).elf
	@$(SIZE) $< | awk 'NR==2 { \
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


# ==========================================================================
# CLEAN
# ==========================================================================

clean:
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).map satellite.elf satellite.bin satellite.map gs.elf gs.bin gs.map
	find . -path "./V1.4" -prune -o -type f \( -name "*.o" -o -name "*.d" \) -exec rm -f {} +


.PHONY: all flash sat SAT flash_sat flash_SAT gs GS flash_gs flash_GS size clean