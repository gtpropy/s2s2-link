# ============================================================================
# S2S2 CubeSat COM firmware -- top-level Makefile
#
#   make               open the configuration TUI (same as make menuconfig)
#   make menuconfig    TUI: configure everything, build, memory stats, flash
#   make configure     generate generated/{s2s2_config.h, s2s2.mk, secrets.h} from .s2s2.config (no UI);
#                      creates the random AUTH key in secrets.h when it does not exist yet
#   make defconfig     reset the options to the defaults (keeps the ST-Link choice) and generate
#   make newkey        replace the AUTH key (rebuild and reflash BOTH boards afterwards)
#   make build         build the selected targets with the saved configuration
#   make flash         flash CPU2 of the selected targets
#   make flash-all     flash CPU1 (NuttX) + CPU2 boot option bytes + CPU2
#   make memstat       print static memory statistics and .bin sizes
#   make probes        list connected ST-Link probes
#   make clean         remove build outputs of both firmwares
#   make nuttx         build the CPU1 NuttX image (nuttx/nuttx.bin)
#   make testbench     open the link-test GUI (link / image / SDR+Direwolf tests, proof reports)
#   make test          host tests (framing + integrity check)
#
# All settings (targets, AUTH, build options, call signs, frequencies, timing, ST-Link probes) live in
# .s2s2.config and are edited in the TUI. See README.md for the repository layout.
# ============================================================================

# Prefer the tools venv (it has Textual); fall back to the system python.
PY  ?= $(if $(wildcard tools/.venv/bin/python),tools/.venv/bin/python,python3)
TUI  = $(PY) tools/config/s2s2_tui.py

.DEFAULT_GOAL := menuconfig
.PHONY: menuconfig tui configure defconfig newkey build flash flash-all clean memstat probes nuttx testbench test

menuconfig tui:
	@$(TUI)

configure:
	@$(TUI) --configure

defconfig:
	@$(TUI) --defconfig

newkey:
	@$(TUI) --newkey

build:
	@$(TUI) --batch build

flash:
	@$(TUI) --batch flash

flash-all:
	@$(TUI) --batch flash-all

clean:
	@$(TUI) --batch clean

memstat:
	@$(TUI) --memstat

probes:
	@$(TUI) --probes

nuttx:
	@cd nuttx && gmake -j4

testbench:
	@$(PY) tools/testbench/server.py

test:
	@sh tests/host/run.sh
