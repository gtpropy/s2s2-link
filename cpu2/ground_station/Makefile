# ============================================================================
# Ground station CPU2 image (STM32WL55 Cortex-M0+, flash at 0x08032000, 56 KB)
#   make [AUTH=..] [DEBUG=..]                           build        -> build/ground_station.bin
#   make flash SERIAL=<st-link>                         flash CPU2 only
# The shared rules, options and flash recipes are in ../common/cpu2.mk.
# ============================================================================

TARGET      = ground_station
TARGET_NAME = Ground station

TARGET_SRCS = app/gs_app.c \
              app/cw_rx.c \
              ../common/digipeater/digipeater.c      # TNC2 text -> frame for the DIGI console command
TARGET_DEFINES = -DMINI_PRINTF_FLOAT
TARGET_OPTS    = gs

include ../common/cpu2.mk
