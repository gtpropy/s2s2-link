# ============================================================================
# S2S2 CPU2 firmware -- top-level Makefile
#
#   make            open the configuration TUI (build, memory stats, flash)
#   make tui        same (alias: make menuconfig)
#   make build      build the selected targets with the saved configuration
#   make flash      flash CPU2 of the selected targets
#   make flash-all  flash CPU1 (NuttX) + CPU2 boot option bytes + CPU2
#   make memstat    print static memory statistics and .bin sizes
#   make probes     list connected ST-Link probes
#   make clean      remove build outputs of both firmwares
#   make nuttx      build the CPU1 NuttX image (nuttx/nuttx.bin)
#   make testbench  open the link test GUI (link / image / SDR+Direwolf tests, proof reports)
#
# Settings (targets, AUTH, DEBUG, SEAL_TEST, probe serials) live in
# .s2s2.config and are edited in the TUI.
# ============================================================================

# Prefer the tools venv (it has Textual); fall back to the system python.
PY  ?= $(if $(wildcard tools/.venv/bin/python),tools/.venv/bin/python,python3)
TUI  = $(PY) tools/s2s2_tui.py

.DEFAULT_GOAL := tui
.PHONY: tui menuconfig build flash flash-all clean memstat probes nuttx testbench

tui menuconfig:
	@$(TUI)

testbench:
	@$(PY) tools/testbench/server.py

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
