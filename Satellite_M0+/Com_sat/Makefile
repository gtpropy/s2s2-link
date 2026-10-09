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

MAIN_SRC = Main_code/Main_Satellite/satellite_main.c \
           Main_code/Main_Satellite/satellite_app.c
TARGET   = satellite



# ==========================================================================
# BUILD OPTIONS (added on top of the reference Makefile)
# ==========================================================================

# A stray CPATH/C_INCLUDE_PATH (e.g. from a Python framework install) breaks
# the ARM toolchain's own header lookup.
unexport CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH

# Compiler selection: ARM_TOOLCHAIN_BIN (env or make var), else the official
# ARM GNU Toolchain if installed (Homebrew's 16.x arm-none-eabi-gcc cannot
# find its own newlib headers), else whatever is on PATH.
ARM_TOOLCHAIN_BIN ?=
ifeq ($(ARM_TOOLCHAIN_BIN),)
  ARM_TOOLCHAIN_BIN := $(firstword $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin))
endif
ifneq ($(ARM_TOOLCHAIN_BIN),)
  TC_PREFIX := $(ARM_TOOLCHAIN_BIN)/
endif

# AUTH=TRUE (default) / FALSE: keyed tag + replay counter on every wrapped
# packet (Mission/seal_wrap.c). The repair bytes stay on either way.
AUTH ?= TRUE
ifeq ($(AUTH),FALSE)
  AUTH_DEFINE = -DAUTH_ENABLE=0
else
  AUTH_DEFINE = -DAUTH_ENABLE=1
endif

# SEAL_TEST=TRUE adds the extended boot self-test (tag vector, damaged
# round trip, replay rejection); costs about 600 bytes of flash.
SEAL_TEST ?= FALSE
ifeq ($(SEAL_TEST),TRUE)
  SEAL_TEST_DEFINE = -DSEAL_FULL_SELFTEST=1
endif

# DEBUG=T (or 1/Y/YES/TRUE): -Og -g3 for stepping. DEBUG=F (default): -Os,
# the reference optimisation level.
DEBUG ?= F
ifneq ($(filter T t 1 Y y YES yes TRUE true,$(DEBUG)),)
  DEBUG_ON := 1
  OPT_FLAGS = -Og -g3
else
  DEBUG_ON := 0
  OPT_FLAGS = -Os
endif

# FRONTEND=NUCLEO (default) / CUSTOM: RF front end the satellite image targets.
# NUCLEO = stock Nucleo-WL55JC (RFO_HP PA, FE_CTRL1-3 switch, as the ground station);
# CUSTOM = reference satellite board (RFO_LP + external 3.3 V / 5 V PAs).
FRONTEND ?= NUCLEO
ifeq ($(FRONTEND),NUCLEO)
  FRONTEND_DEFINE = -DSAT_NUCLEO_FRONTEND=1
else
  FRONTEND_DEFINE =
endif

TIMER_IF_USE_RTC ?= 0

# LOG=1 (default): full satellite console log. LOG=0 compiles it out (see uart/uart_debug.h) and
# saves ~15 KB, but is NOT usable yet: with LOG=0 the downlink frames arrive corrupted (open bug).
LOG ?= 1
ifeq ($(LOG),0)
  # SAT_LOG_QUIET: uart1_puts/uart1_printf expand to ((void)0), arguments NOT evaluated (uart/uart_debug.h)
  LOG_DEFINE = -DSAT_LOG_QUIET=1
else
  LOG_DEFINE =
endif

# Objects do not encode these options, so a change forces a clean rebuild.
# Changing any option above must rebuild everything (the options are -D flags, not tracked per .o):
# the option string is stored in .build_opts and all objects are deleted when it changes.
ifneq ($(AUTH)-$(DEBUG_ON)-$(SEAL_TEST)-$(FRONTEND)-$(TIMER_IF_USE_RTC)-$(LOG),$(shell cat .build_opts 2>/dev/null))
  $(shell /usr/bin/find . -path ./V1.4 -prune -o -type f -name '*.o' -exec rm -f {} + ; echo '$(AUTH)-$(DEBUG_ON)-$(SEAL_TEST)-$(FRONTEND)-$(TIMER_IF_USE_RTC)-$(LOG)' > .build_opts)
endif

# Probe selection: SERIAL=<st-link serial> (see `make probes`). Required when
# more than one ST-Link is connected.
SERIAL ?=
ifneq ($(SERIAL),)
  OPENOCD_HLA_SERIAL := -c "adapter serial $(SERIAL)"
  STM32PROG_SN := sn=$(SERIAL)
else
  OPENOCD_HLA_SERIAL :=
  STM32PROG_SN :=
endif

# ==========================================================================
# TOOLCHAIN
# ==========================================================================

CC      = $(TC_PREFIX)arm-none-eabi-gcc
AS      = $(TC_PREFIX)arm-none-eabi-gcc
OBJCOPY = $(TC_PREFIX)arm-none-eabi-objcopy
SIZE    = $(TC_PREFIX)arm-none-eabi-size


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
         $(OPT_FLAGS) \
         $(AUTH_DEFINE) \
         -DTIMER_IF_USE_RTC=$(TIMER_IF_USE_RTC) \
         $(SEAL_TEST_DEFINE) \
         $(FRONTEND_DEFINE) \
         $(LOG_DEFINE) \
         -MMD -MP \
         -Wall \
         -Wextra \
         -g \
         -fdata-sections \
         -ffunction-sections \
         -DUSE_HAL_DRIVER \
         -DSTM32WL55xx \
         -DCORE_CM0PLUS \
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
uart/obc_led.c \
ipc/Sring_buffer.c \
protocol/ax25/ax25.c \
protocol/ax25/crc16.c \
protocol/g3ruh/g3ruh.c \
Mission/protocol.c \
Mission/seal_wrap.c \
Mission/mini_printf.c \
Mission/radio_app.c \
protocol/stm32wlxx_it.c \
protocol/stm32_seq.c \
protocol/stm32_lpm.c \
core/stm32_mem.c \
core/system_stm32wlxx.c \
core/timer_if.c \
core/rtc.c \
core/ll_port.c \
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
hal/Src/stm32wlxx_hal_rcc.c \
bsp/stm32wlxx_nucleo_radio.c



# TIMER_IF_USE_RTC=1 builds the RTC-backed UTIL_TIMER tick (dormant in this firmware,
# see core/timer_if.c); default 0 leaves the HAL RTC / RCC-ext code out.
TIMER_IF_USE_RTC ?= 0
ifeq ($(TIMER_IF_USE_RTC),1)
C_SRCS += hal/Src/stm32wlxx_hal_rtc.c hal/Src/stm32wlxx_hal_rtc_ex.c hal/Src/stm32wlxx_hal_rcc_ex.c
endif
# ==========================================================================
# STARTUP
# ==========================================================================

S_SRCS = startup_cm0p.s \
         Mission/seal_core.S \
         Mission/seal_tables.S


# ==========================================================================
# OBJECT FILES
# ==========================================================================

C_OBJS = $(C_SRCS:.c=.o)

S_OBJS = $(patsubst %.S,%.o,$(patsubst %.s,%.o,$(S_SRCS)))

ALL_OBJS = $(C_OBJS) $(S_OBJS)


# ==========================================================================
# DEFAULT BUILD
# ==========================================================================

ifeq ($(MAKECMDGOALS),)
all: $(TARGET).bin
else ifeq ($(MAKECMDGOALS),all)
all: $(TARGET).bin
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

%.o: %.S
	$(AS) $(ASFLAGS) $(AUTH_DEFINE) -MMD -MP -c $< -o $@


# ==========================================================================
# PROGRAMMER
# ==========================================================================

STM32_PROG ?= $(shell which STM32_Programmer_CLI 2>/dev/null || \
	ls /Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI 2>/dev/null || \
	ls $(HOME)/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI 2>/dev/null || \
	ls /home/*/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI 2>/dev/null | head -n 1 || \
	ls /opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_*/tools/bin/STM32_Programmer_CLI 2>/dev/null | head -n 1 || \
	ls /opt/st/stm32cubeprogrammer_*/bin/STM32_Programmer_CLI 2>/dev/null | head -n 1 || \
	echo STM32_Programmer_CLI)

# ==========================================================================
# FLASH CPU2 (CORTEX-M0+ ONLY: 0x08032000 - 0x0803FFFF)
# This writes ONLY to sectors 100-127 (56 KB max).
# CPU1 / Cortex-M4 at 0x08000000 (sectors 0-99) is strictly preserved!
# ==========================================================================

flash: $(TARGET).bin
	@if [ -x "$$(which $(STM32_PROG) 2>/dev/null)" ] || [ -x "$(STM32_PROG)" ]; then \
	  echo "Flashing $(TARGET).bin to 0x08032000 using STM32_Programmer_CLI (CPU1 preserved)..."; \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=UR -w $(TARGET).bin 0x08032000 -v -hardRst 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=HOTPLUG -w $(TARGET).bin 0x08032000 -v -hardRst 2>/dev/null || \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) -w $(TARGET).bin 0x08032000 -v -hardRst || \
	  (echo "STM32_Programmer_CLI failed. Trying OpenOCD fallback..." && \
	  openocd -f interface/stlink.cfg -c "transport select hla_swd" $(OPENOCD_HLA_SERIAL) -f target/stm32wlx.cfg -c "reset_config none" -c "program $(TARGET).bin 0x08032000 verify reset exit" 2>/dev/null || \
	  openocd -f interface/stlink-dap.cfg $(OPENOCD_HLA_SERIAL) -f target/stm32wlx.cfg -c "reset_config none" -c "program $(TARGET).bin 0x08032000 verify reset exit"); \
	  echo "Booting CPU2 (Cortex-M0+) via PWR_CR4_C2BOOT..."; \
	  $(STM32_PROG) -c port=SWD $(STM32PROG_SN) -w32 0x5800040C 0x00008000 2>/dev/null || true; \
	else \
	  echo "Flashing $(TARGET).bin to 0x08032000 using OpenOCD (CPU1 preserved)..."; \
	  openocd -f interface/stlink.cfg -c "transport select hla_swd" $(OPENOCD_HLA_SERIAL) -f target/stm32wlx.cfg -c "reset_config none" -c "program $(TARGET).bin 0x08032000 verify reset exit" 2>/dev/null || \
	  openocd -f interface/stlink-dap.cfg $(OPENOCD_HLA_SERIAL) -f target/stm32wlx.cfg -c "reset_config none" -c "program $(TARGET).bin 0x08032000 verify reset exit"; \
	fi


sat SAT: $(TARGET).bin

flashsatM0 flash_satM0 flashsat flash_sat flash_SAT flash_m0 flash_M0 flash_cpu2 flash_CPU2: flash

flash_gs flash_GS:
	$(MAKE) -C ../../Ground_Station flash_gs


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
	rm -f $(ALL_OBJS:.o=.d) .build_opts
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).map
	find . -path "./V1.4" -prune -o -type f \( -name "*.o" -o -name "*.d" \) -exec rm -f {} +


.PHONY: all flash sat SAT flashsatM0 flash_satM0 flashsat flash_sat flash_SAT flash_m0 flash_M0 flash_cpu2 flash_CPU2 size clean

# ==========================================================================
# EXTRA TARGETS (probes, option bytes, CPU1 image, one-shot flashing)
# ==========================================================================

NUTTX_BIN ?= ../../nuttx/nuttx.bin
SAT_SERIAL_DEFAULT ?= 002D001B4741500920383733
GS_SERIAL_DEFAULT  ?= 303030303030303030303031
SAT_DIR ?= .
GS_DIR  ?= ../../Ground_Station

# compile_flags.txt for clangd / IDE header resolution
compile_flags:
	@printf '%s\n' $(filter -I% -D%,$(CFLAGS)) > compile_flags.txt
	@echo "wrote compile_flags.txt"

probes:
	@echo "Connected ST-Link probes (st-info):"
	@st-info --probe 2>&1 || echo "  (st-info not found or no probes detected)"

# CPU2 boot vector: C2OPT=1 (flash) and SBRV=0xC800 -> 0x08032000.
c2boot:
	@$(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=UR -ob C2OPT=1 SBRV=0xC800

# CPU1 (Cortex-M4 / NuttX) image: boots CPU2 and runs the telemetry source.
flash-m4 flash_m4:
	@test -f $(NUTTX_BIN) || (echo "Missing $(NUTTX_BIN). Build NuttX first (cd nuttx && gmake -j4)" && exit 1)
	@echo "Flashing $(NUTTX_BIN) to 0x08000000 (CPU1 / M4)..."
	@$(STM32_PROG) -c port=SWD $(STM32PROG_SN) mode=UR -w $(NUTTX_BIN) 0x08000000 -v -hardRst 2>/dev/null || \
	  openocd -f interface/stlink.cfg -c "transport select hla_swd" $(OPENOCD_HLA_SERIAL) \
	    -f target/stm32wlx.cfg -c "reset_config none" -c "init" -c "reset halt" \
	    -c "program $(NUTTX_BIN) 0x08000000 verify" -c "reset run" -c "exit"

flash-sat:
	$(MAKE) -C $(SAT_DIR) SERIAL="$(SERIAL)" DEBUG=F AUTH=$(AUTH) flash

flash-sat-debug:
	$(MAKE) -C $(SAT_DIR) SERIAL="$(SERIAL)" DEBUG=T AUTH=$(AUTH) flash

flash-gs:
	$(MAKE) -C $(GS_DIR) SERIAL="$(SERIAL)" DEBUG=F AUTH=$(AUTH) flash

flash-gs-debug:
	$(MAKE) -C $(GS_DIR) SERIAL="$(SERIAL)" DEBUG=T AUTH=$(AUTH) flash

# Flash both boards: asks for each probe serial (Enter = default in brackets).
flash-all:
	@st-info --probe 2>&1 | grep -i "serial" || echo "(no probes detected)"
	@printf "Satellite (Nucleo) serial [$(SAT_SERIAL_DEFAULT)]: "; read SAT; SAT=$${SAT:-$(SAT_SERIAL_DEFAULT)}; \
	printf "Ground station serial [$(GS_SERIAL_DEFAULT)]: "; read GS; GS=$${GS:-$(GS_SERIAL_DEFAULT)}; \
	if [ "$$SAT" = "$$GS" ]; then echo "ERROR: satellite and GS serials are the same probe"; exit 1; fi; \
	PROBES=$$(st-info --probe 2>&1); \
	for P in "$$SAT" "$$GS"; do \
	  echo "$$PROBES" | grep -q "$$P" || { echo "ERROR: probe $$P is not connected (see make probes)"; exit 1; }; \
	done; \
	$(MAKE) flash-m4 SERIAL=$$SAT && $(MAKE) c2boot SERIAL=$$SAT && \
	$(MAKE) flash-sat SERIAL=$$SAT AUTH=$(AUTH) && \
	$(MAKE) flash-m4 SERIAL=$$GS && $(MAKE) c2boot SERIAL=$$GS && \
	$(MAKE) flash-gs SERIAL=$$GS AUTH=$(AUTH) && \
	echo "flash-all: satellite ($$SAT) and GS ($$GS) done"

-include $(ALL_OBJS:.o=.d)

.PHONY: compile_flags probes c2boot flash-m4 flash_m4 flash-sat flash-sat-debug flash-gs flash-gs-debug flash-all
