# Mock satellite image (tests/mocksat/mock_main.c). Included by the Makefile next to it, which runs make in cpu2/satellite with this file,
# so the board glue, radio layer and ../common code are the very files the real satellite uses. Output: build/mocksat/mocksat.bin
# Fixed on purpose: stock Nucleo front end, no CUBUS, no digipeater, -Os. These cannot be overridden from the command line.
override CUBUS    := F
override FRONTEND := NUCLEO
override DIGI     := FALSE
override DEBUG    := F

MOCK_DIR    := ../../tests/mocksat
TARGET      = mocksat
TARGET_NAME = Mock satellite (Nucleo only)
TARGET_SRCS = mock_main.c app/satellite_app.c
TARGET_DEFINES = -I$(MOCK_DIR) -I$(REPO)/include/satellite -DS2S2_MOCKSAT=1 -DRADIO_TX_POWER_DBM=15
TARGET_OPTS = mocksat
vpath %.c $(MOCK_DIR)

include ../common/cpu2.mk
