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

APP ?= satellite

ifeq ($(APP),gs)
  MAIN_SRC = satellite/gs_main.c
  TARGET   = gs_m0plus
else
  MAIN_SRC = ipcc_m0plus_main.c
  TARGET   = ipcc_m0plus
endif


# ==========================================================================
# TOOLCHAIN
# ==========================================================================

CC      = arm-none-eabi-gcc
AS      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size


# ==========================================================================
# PATHS
# ==========================================================================

CUBEWL = /home/prem/Downloads/stm32cubewl-v1-6-0/STM32Cube_FW_WL_V1.6.0

SHARED_IPC = /home/prem/Desktop/nuttxspace/shareRAM/ipc

LDSCRIPT = ../scripts/wl55jc_cm0p.ld


# ==========================================================================
# CPU
# ==========================================================================

CPU = -mcpu=cortex-m0plus -mthumb


# ==========================================================================
# COMMON INCLUDE PATHS
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
         -I. \
         -IInc \
         -Icore \
         -Icore/Src \
         -Icore/Inc \
         -Iradio_driver \
         -Ihal \
         -Isubghz \
         -Ibsp \
         -Itarget \
         -I$(SHARED_IPC) \
         -I$(CUBEWL)/Drivers/CMSIS/Device/ST/STM32WLxx/Include \
         -I$(CUBEWL)/Drivers/CMSIS/Include \
         -I$(CUBEWL)/Drivers/STM32WLxx_HAL_Driver/Inc \
         -I$(CUBEWL)/Drivers/BSP/STM32WLxx_Nucleo \
         -I$(CUBEWL)/Drivers/BSP/Components \
         -I$(CUBEWL)/Projects/NUCLEO-WL55JC/Applications/SubGHz_Phy/SubGHz_Phy_PingPong_DualCore/Common/Board \
         -I$(CUBEWL)/Utilities/misc \
         -I$(CUBEWL)/Projects/NUCLEO-WL55JC/Applications/SubGHz_Phy/SubGHz_Phy_PingPong_DualCore/CM0PLUS/MbMux \
         -I$(CUBEWL)/Projects/NUCLEO-WL55JC/Applications/SubGHz_Phy/SubGHz_Phy_PingPong_DualCore/Common/MbMux \
         -I$(CUBEWL)/Projects/NUCLEO-WL55JC/Applications/SubGHz_Phy/SubGHz_Phy_PingPong_DualCore/CM0PLUS/Core/Inc \
         -I$(CUBEWL)/Projects/NUCLEO-WL55JC/Applications/SubGHz_Phy/SubGHz_Phy_PingPong_DualCore/CM0PLUS/SubGHz_Phy/Target \
         -I$(CUBEWL)/Utilities/trace/adv_trace \
         -I$(CUBEWL)/Projects/NUCLEO-WL55JC/Applications/SubGHz_Phy/SubGHz_Phy_PingPong_DualCore/Common/System \
         -I$(CUBEWL)/Utilities/timer


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
uart_debug.c \
$(SHARED_IPC)/Sring_buffer.c \
core/Src/ax25.c \
core/Src/crc16.c \
core/Src/g3ruh.c \
core/Src/protocol.c \
core/Src/radio_app.c \
core/Src/stm32wlxx_it.c \
core/stm32_seq.c \
core/stm32_lpm.c \
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
hal/stm32wlxx_hal.c \
hal/stm32wlxx_hal_subghz.c \
hal/stm32wlxx_hal_rtc.c \
hal/stm32wlxx_hal_rtc_ex.c \
hal/stm32wlxx_hal_rcc.c \
hal/stm32wlxx_hal_rcc_ex.c \
hal/stm32wlxx_hal_gpio.c \
hal/stm32wlxx_hal_cortex.c \
hal/stm32wlxx_hal_pwr.c \
hal/stm32wlxx_hal_pwr_ex.c \
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

all: $(TARGET).bin


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
	@echo " M0+ BUILD SUCCESS"
	@echo "========================================"
	@echo " ELF : $(TARGET).elf"
	@echo " BIN : $(TARGET).bin"
	@echo " MAP : $(TARGET).map"
	@echo " Flash address: 0x08020000"
	@echo "========================================"

	@$(SIZE) $(TARGET).elf


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
# FLASH CPU2
# ==========================================================================

flash: $(TARGET).bin
	openocd \
	  -f interface/stlink.cfg \
	  -f target/stm32wlx.cfg \
	  -c "init" \
	  -c "targets" \
	  -c "reset halt" \
	  -c "halt" \
	  -c "flash probe 0" \
	  -c "program $(TARGET).bin 0x08020000 verify" \
	  -c "reset run" \
	  -c "exit"

sat:
	$(MAKE) APP=satellite

flash_sat:
	$(MAKE) APP=satellite flash

gs:
	$(MAKE) APP=gs

flash_gs:
	$(MAKE) APP=gs flash


# ==========================================================================
# SIZE
# ==========================================================================

size: $(TARGET).elf
	$(SIZE) $<


# ==========================================================================
# CLEAN
# ==========================================================================

clean:
	rm -f $(ALL_OBJS)
	rm -f $(TARGET).elf
	rm -f $(TARGET).bin
	rm -f $(TARGET).map
	rm -f ipcc_m0plus.elf ipcc_m0plus.bin ipcc_m0plus.map
	rm -f gs_m0plus.elf gs_m0plus.bin gs_m0plus.map


.PHONY: all flash sat flash_sat gs flash_gs size clean