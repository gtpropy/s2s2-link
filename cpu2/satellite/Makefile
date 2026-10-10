# ============================================================================
# Satellite CPU2 image (STM32WL55 Cortex-M0+, flash at 0x08032000, 56 KB)
#   make [AUTH=..] [DEBUG=..] [FRONTEND=..] [LOG=..]    build        -> build/satellite.bin
#   make flash SERIAL=<st-link>                         flash CPU2 only
# The shared rules, options and flash recipes are in ../common/cpu2.mk.
# ============================================================================

TARGET      = satellite
TARGET_NAME = Satellite

TARGET_SRCS = app/satellite_main.c \
              app/satellite_app.c \
              uart/obc_led.c
TARGET_DEFINES =
TARGET_OPTS    = sat-$(FRONTEND)-$(LOG)

include ../common/cpu2.mk
