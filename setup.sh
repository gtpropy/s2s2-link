#!/usr/bin/env bash
# setup.sh -- one-time environment setup for building/flashing/running
# the S2S2 CPU2 firmware + Python tooling on a fresh machine (macOS or
# Linux). Run once per machine: `./setup.sh`.
#
# What this fixes, concretely (found while debugging real build
# failures on this exact repo, not guessed):
#   1. CPU2/Makefile used to hardcode ARM_TOOLCHAIN_BIN to a macOS-only
#      absolute path (/Applications/ArmGNUToolchain/15.3.rel1/...) --
#      broke on every other machine. The Makefile now looks for
#      `arm-none-eabi-gcc` on PATH first, or a `.toolchain_path` file
#      this script writes if it has to fall back to an off-PATH install.
#   2. On macOS there are TWO different arm-none-eabi-gcc packages:
#      Homebrew's `arm-none-eabi-gcc` formula (bottled 16.x, confirmed
#      broken -- fails with "stray '#' in program" deep in its own
#      bundled stdint.h, but ONLY on a real build of this project's
#      actual sources -- a trivial stdint.h/stdio.h snippet passes on
#      it too, that's a dead end) and the `gcc-arm-embedded` CASK (the
#      official ARM GNU Toolchain release, confirmed working, but not
#      placed on PATH by casks). So the self-test below is a real
#      `make sat-debug`, not a synthetic snippet -- nothing else
#      reliably reproduces the failure.
#   3. No `compile_flags.txt` existed anywhere, so clangd/any IDE
#      couldn't resolve stm32wlxx_hal.h etc even when the STM32CubeWL
#      SDK was genuinely present on disk. This script runs
#      `make compile_flags` (CPU2/Makefile) to generate/refresh it,
#      derived from the Makefile's own CFLAGS so it can't drift.
#   4. A stray CPATH-family env var (seen: a Python installer setting
#      CPATH globally) leaks host headers into the cross-compile; the
#      Makefile now `unexport`s those for every recipe regardless of
#      what's polluting your shell.
#
# What this does NOT do: download the STM32CubeWL SDK. ST gates that
# behind a click-through license on their own site -- there's no stable
# direct-download URL to script against honestly, so this prints the
# exact path it expects and the page to get it from instead of guessing.
#
# Written for portability across bash versions (macOS ships bash 3.2)
# -- no associative arrays, no empty-array expansion under `set -u`.
#
# Usage:
#   ./setup.sh              # full setup
#   ./setup.sh --check      # report status only, change nothing
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPU2_DIR="$REPO_ROOT/CPU2"
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "  ${GREEN}OK${NC}    $1"; }
warn() { echo -e "  ${YELLOW}WARN${NC}  $1"; }
fail() { echo -e "  ${RED}FAIL${NC}  $1"; }
step() { echo -e "\n${BLUE}== $1 ==${NC}"; }

OS="$(uname -s)"
case "$OS" in
  Darwin) PLATFORM=macos ;;
  Linux)  PLATFORM=linux ;;
  *)      PLATFORM=other ;;
esac
echo "Platform: $OS ($PLATFORM)"

# =====================================================================
# STM32CubeWL SDK -- checked FIRST: the toolchain self-test below needs
# it present to do a real build. ST gates the SDK behind a click-
# through license on their own site -- no honest direct-download URL
# to script against, so this just checks + instructs.
# =====================================================================

step "STM32CubeWL SDK"
CUBEWL_MARKER="$HOME/sdks/STM32CubeWL/Drivers/STM32WLxx_HAL_Driver/Inc/stm32wlxx_hal.h"
CUBEWL_PRESENT=0
if [ -f "$CUBEWL_MARKER" ]; then
  ok "found at \$HOME/sdks/STM32CubeWL"
  CUBEWL_PRESENT=1
else
  fail "not found at \$HOME/sdks/STM32CubeWL"
  echo "     Download STM32CubeWL from ST (requires accepting their license on-site):"
  echo "       https://www.st.com/en/embedded-software/stm32cubewl.html"
  echo "     Extract it so this file exists:"
  echo "       \$HOME/sdks/STM32CubeWL/Drivers/STM32WLxx_HAL_Driver/Inc/stm32wlxx_hal.h"
  echo "     (the ARM-toolchain check below can only be a light version check until then)"
fi

# =====================================================================
# ARM toolchain -- the only reliable test is a real build of this
# project (see the header comment above for why a synthetic snippet
# isn't enough). Falls back to a version check if the SDK isn't
# present yet, since a real build isn't possible without it.
# =====================================================================

# $1 = "" for PATH, or an absolute bin dir to try via ARM_TOOLCHAIN_BIN=
test_toolchain_real_build() {
  local bin_dir="$1"
  local extra=""
  [ -n "$bin_dir" ] && extra="ARM_TOOLCHAIN_BIN=$bin_dir"
  ( cd "$CPU2_DIR" && make $extra clean >/dev/null 2>&1 && make $extra sat-debug >/dev/null 2>&1 )
}

test_toolchain_version_only() {
  local gcc_bin="$1"
  command -v "$gcc_bin" >/dev/null 2>&1 || return 1
  "$gcc_bin" --version >/dev/null 2>&1
}

step "ARM toolchain (arm-none-eabi-gcc)"
TOOLCHAIN_BIN_DIR=""
TOOLCHAIN_OK=0

# A previously-written .toolchain_path is authoritative -- the Makefile
# prefers it over bare PATH, so that's what actually builds regardless
# of what bare `arm-none-eabi-gcc` resolves to. Check it FIRST so
# --check reports the tool that's actually in effect, not a stale
# "PATH works" that's really passing because the pin masks it.
EXISTING_PIN=""
[ -f "$CPU2_DIR/.toolchain_path" ] && EXISTING_PIN="$(cat "$CPU2_DIR/.toolchain_path")"

if [ -n "$EXISTING_PIN" ]; then
  if [ "$CUBEWL_PRESENT" = 1 ] && test_toolchain_real_build "$EXISTING_PIN"; then
    ok "pinned toolchain (CPU2/.toolchain_path) builds this project: $EXISTING_PIN"
    TOOLCHAIN_OK=1
  elif [ "$CUBEWL_PRESENT" = 0 ] && test_toolchain_version_only "$EXISTING_PIN/arm-none-eabi-gcc"; then
    warn "pinned toolchain present, can't real-build-test without the SDK above: $EXISTING_PIN"
    TOOLCHAIN_OK=1
  fi
fi

if [ "$TOOLCHAIN_OK" = 0 ] && [ "$CUBEWL_PRESENT" = 1 ]; then
  if command -v arm-none-eabi-gcc >/dev/null 2>&1 && test_toolchain_real_build ""; then
    ok "arm-none-eabi-gcc on PATH builds this project ($(arm-none-eabi-gcc --version | head -1))"
    TOOLCHAIN_OK=1
  fi
elif [ "$TOOLCHAIN_OK" = 0 ]; then
  if test_toolchain_version_only arm-none-eabi-gcc; then
    warn "arm-none-eabi-gcc on PATH looks present, but can't real-build-test it without the SDK above"
    TOOLCHAIN_OK=1
  fi
fi

if [ "$TOOLCHAIN_OK" = 0 ]; then
  if [ "$CHECK_ONLY" = 1 ]; then
    fail "no working arm-none-eabi-gcc found on PATH (run without --check to fix)"
  else
    warn "arm-none-eabi-gcc missing or broken on PATH -- trying a known-good install"
    if [ "$PLATFORM" = macos ]; then
      if ! command -v brew >/dev/null 2>&1; then
        fail "Homebrew not found -- install it from https://brew.sh, then re-run setup.sh"
      else
        brew list --cask gcc-arm-embedded >/dev/null 2>&1 || brew install --cask gcc-arm-embedded
        CANDIDATE=$(find /Applications/ArmGNUToolchain -maxdepth 3 -type d -name bin 2>/dev/null \
                    | sort -V | tail -1)
        if [ -n "$CANDIDATE" ]; then
          if [ "$CUBEWL_PRESENT" = 1 ] && test_toolchain_real_build "$CANDIDATE"; then
            ok "gcc-arm-embedded cask builds this project: $CANDIDATE"
            TOOLCHAIN_BIN_DIR="$CANDIDATE"
          elif [ "$CUBEWL_PRESENT" = 0 ] && test_toolchain_version_only "$CANDIDATE/arm-none-eabi-gcc"; then
            warn "gcc-arm-embedded cask installed, can't real-build-test without the SDK above: $CANDIDATE"
            TOOLCHAIN_BIN_DIR="$CANDIDATE"
          else
            fail "gcc-arm-embedded cask still doesn't build this project -- see" \
                 "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads and install manually"
          fi
        else
          fail "gcc-arm-embedded cask install didn't produce a bin/ dir under /Applications/ArmGNUToolchain"
        fi
      fi
    elif [ "$PLATFORM" = linux ]; then
      if command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update -y && sudo apt-get install -y gcc-arm-none-eabi
        if command -v arm-none-eabi-gcc >/dev/null 2>&1 \
           && { [ "$CUBEWL_PRESENT" = 0 ] || test_toolchain_real_build ""; }; then
          ok "gcc-arm-none-eabi (apt) installed"
        else
          fail "apt's gcc-arm-none-eabi still doesn't build this project -- see" \
               "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads and install manually"
        fi
      else
        fail "no apt-get -- install arm-none-eabi-gcc via your package manager, or manually from" \
             "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
      fi
    else
      fail "unrecognized platform -- install arm-none-eabi-gcc manually from" \
           "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
    fi
  fi
fi

if [ "$CHECK_ONLY" != 1 ]; then
  if [ -n "$TOOLCHAIN_BIN_DIR" ]; then
    printf '%s' "$TOOLCHAIN_BIN_DIR" > "$CPU2_DIR/.toolchain_path"
    echo "  wrote CPU2/.toolchain_path -> $TOOLCHAIN_BIN_DIR"
  else
    rm -f "$CPU2_DIR/.toolchain_path"  # PATH version is good (or nothing worked); don't pin a stale override
  fi
fi

# =====================================================================
# Flashing tools: openocd (Makefile's `flash` target) + stlink-tools
# (flasher.py's st-info/st-flash)
# =====================================================================

step "Flashing tools (openocd, st-info/st-flash)"
if command -v openocd >/dev/null 2>&1; then
  ok "openocd found"
elif [ "$CHECK_ONLY" = 1 ]; then
  fail "openocd not found"
else
  if [ "$PLATFORM" = macos ] && command -v brew >/dev/null 2>&1; then
    brew install openocd
  elif [ "$PLATFORM" = linux ] && command -v apt-get >/dev/null 2>&1; then
    sudo apt-get install -y openocd
  else
    warn "install openocd manually for your platform"
  fi
  command -v openocd >/dev/null 2>&1 && ok "openocd installed" || fail "openocd still not found"
fi

if command -v st-info >/dev/null 2>&1 && command -v st-flash >/dev/null 2>&1; then
  ok "st-info/st-flash found"
elif [ "$CHECK_ONLY" = 1 ]; then
  fail "st-info/st-flash not found"
else
  if [ "$PLATFORM" = macos ] && command -v brew >/dev/null 2>&1; then
    brew install stlink
  elif [ "$PLATFORM" = linux ] && command -v apt-get >/dev/null 2>&1; then
    sudo apt-get install -y stlink-tools
  else
    warn "install stlink-tools manually for your platform"
  fi
  { command -v st-info >/dev/null 2>&1 && command -v st-flash >/dev/null 2>&1; } \
    && ok "st-info/st-flash installed" || fail "st-info/st-flash still not found"
fi

# =====================================================================
# Python tooling (CPU2/tools/*.py)
# =====================================================================

step "Python tooling"
if [ "$CHECK_ONLY" = 1 ]; then
  command -v python3 >/dev/null 2>&1 && ok "python3 found" || fail "python3 not found"
else
  if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 not found -- install it first (brew install python3 / apt install python3)"
  else
    VENV_DIR="$CPU2_DIR/tools/.venv"
    if [ ! -d "$VENV_DIR" ]; then
      python3 -m venv "$VENV_DIR"
    fi
    # shellcheck disable=SC1091
    source "$VENV_DIR/bin/activate"
    pip install --quiet --upgrade pip
    pip install --quiet -r "$CPU2_DIR/tools/requirements.txt"
    deactivate
    ok "venv ready at CPU2/tools/.venv (activate it: source CPU2/tools/.venv/bin/activate)"
  fi
fi

# =====================================================================
# clangd / IDE support -- compile_flags.txt, derived from the real
# Makefile CFLAGS so it can't drift out of sync with the actual build.
# =====================================================================

step "compile_flags.txt (clangd / IDE header resolution)"
if [ "$CHECK_ONLY" = 1 ]; then
  [ -f "$CPU2_DIR/compile_flags.txt" ] && ok "exists" || fail "missing -- run without --check"
else
  EXTRA=""
  [ -f "$CPU2_DIR/.toolchain_path" ] && EXTRA="ARM_TOOLCHAIN_BIN=$(cat "$CPU2_DIR/.toolchain_path")"
  if ( cd "$CPU2_DIR" && make $EXTRA compile_flags ); then
    ok "wrote CPU2/compile_flags.txt"
  else
    fail "failed to generate compile_flags.txt"
  fi
fi

# =====================================================================
# Summary
# =====================================================================

step "Next steps"
echo "  Build + flash satellite:  cd CPU2 && make flash-sat-debug"
echo "  Build + flash GS:         cd CPU2 && make flash-gs-debug"
echo "  Same via TUI:              python3 CPU2/tools/flasher.py   (after: source CPU2/tools/.venv/bin/activate)"
echo "  Mission control dashboard: python3 CPU2/tools/mission_control_api.py"
echo "  Re-check without changing anything: ./setup.sh --check"
