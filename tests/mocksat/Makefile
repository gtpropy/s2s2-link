# Mock satellite for a stock Nucleo-WL55JC: answers every command like the satellite, with made-up data (see mock_main.c).
#   make -C tests/mocksat            build        -> build/mocksat/mocksat.bin
#   make -C tests/mocksat flash SERIAL=<st-link serial>    flash CPU2 only (CPU1 stays); the flash guard refuses a CUBUS board
#   make -C tests/mocksat size | clean
# To get the real satellite back: cd cpu2/satellite && make CUBUS=F flash SERIAL=<same probe>   (or make flash sat from the repo root)
SELF := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SAT  := $(SELF)/../../cpu2/satellite

.PHONY: all flash size clean
all flash size clean:
	$(MAKE) -C $(SAT) -f $(SELF)/mocksat.mk GUARD_IMAGE=nucleo $@
