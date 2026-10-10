#!/usr/bin/env bash
# setup.sh -- one-time environment setup for building/flashing/running
# the S2S2 CPU2 firmware + Python tooling on a fresh machine (macOS or
# Linux). Run once per machine: `./setup.sh`.
#
# What this fixes, concretely (found while debugging real build
# failures on this exact repo, not guessed):
#   1. Makefile used to hardcode ARM_TOOLCHAIN_BIN to a macOS-only
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
#      `make compile_flags` (Makefile) to generate/refresh it,
#      derived from the Makefile's own CFLAGS so it can't drift.
#   4. A stray CPATH-family env var (seen: a Python installer setting
#      CPATH globally) leaks host headers into the cross-compile; the
#      Makefile now `unexport`s those for every recipe regardless of
#      what's polluting your shell.
#   5. (Linux only) The ST-Link's raw USB device node and the Nucleo's
#      /dev/ttyACM*-style debug UART are both root-owned by default on
#      a fresh Linux install -- neither macOS nor a distro that already
#      has stlink-tools' own udev rules installed hits this, which is
#      why it's easy to miss. This script installs the missing udev
#      rules and adds you to the `dialout` group, interactively (both
#      need sudo, so it asks first rather than silently changing system
#      config).
#
# What this does NOT do: download the STM32CubeWL SDK. ST gates that
# behind a click-through license on their own site -- there's no stable
# direct-download URL to script against honestly, so this prints the
# exact path it expects and the page to get it from instead of guessing.
# Also does not build CPU1 (NuttX) by default -- kconfig-frontends is
# offered as an interactive opt-in near the end, since most work on
# this repo is CPU2-only and that build is a separate, heavier step.
#
# Written for portability across bash versions (macOS ships bash 3.2)
# -- no associative arrays, no empty-array expansion under `set -u`.
#
# Interactive: every step that would use sudo (installing udev rules,
# adding you to a group, or building+installing kconfig-frontends)
# explicitly asks first, defaulting to "no" on a bare Enter. Everything
# else (package manager installs of openocd/stlink-tools/arm toolchain,
# the Python venv, compile_flags.txt) proceeds without asking, matching
# how this script has always behaved for those steps.
#
# Usage:
#   ./setup.sh              # full setup
#   ./setup.sh --check      # report status only, change nothing
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$REPO_ROOT/cpu2/satellite"
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
  ( cd "$FW_DIR" && make $extra clean >/dev/null 2>&1 && make $extra >/dev/null 2>&1 )
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
[ -f "$FW_DIR/.toolchain_path" ] && EXISTING_PIN="$(cat "$FW_DIR/.toolchain_path")"

if [ -n "$EXISTING_PIN" ]; then
  if [ "$CUBEWL_PRESENT" = 1 ] && test_toolchain_real_build "$EXISTING_PIN"; then
    ok "pinned toolchain (.toolchain_path) builds this project: $EXISTING_PIN"
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
    printf '%s' "$TOOLCHAIN_BIN_DIR" > "$FW_DIR/.toolchain_path"
    echo "  wrote .toolchain_path -> $TOOLCHAIN_BIN_DIR"
  else
    rm -f "$FW_DIR/.toolchain_path"  # PATH version is good (or nothing worked); don't pin a stale override
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
# Interactive confirm helper -- used only for the Linux-specific,
# sudo-requiring steps below (udev rules, group membership, building
# kconfig-frontends from source). Skips with a warning instead of
# hanging if stdin isn't a real TTY (e.g. this script piped into bash),
# and always skips under --check (report-only, change nothing).
# =====================================================================

confirm() {
  local prompt="$1"
  if [ "$CHECK_ONLY" = 1 ]; then return 1; fi
  if [ ! -t 0 ]; then
    warn "non-interactive shell, skipping: $prompt (re-run ./setup.sh directly in a terminal to be asked)"
    return 1
  fi
  local reply
  read -r -p "  $prompt [y/N] " reply
  case "$reply" in
    [yY]|[yY][eE][sS]) return 0 ;;
    *) return 1 ;;
  esac
}

# =====================================================================
# Linux-only: ST-Link USB permissions (udev rules) + serial port group
# membership. The single biggest real-world Linux gotcha for this repo
# -- without these, every st-flash/openocd/JTAG command and every
# tools/*.py serial connection needs sudo, since USB/tty device
# nodes are root-owned by default. macOS/homebrew's stlink cask needs
# neither (its access model is already user-level), which is why this
# section is Linux-only.
# =====================================================================

if [ "$PLATFORM" = linux ]; then
  step "Linux: ST-Link USB permissions (udev rules)"
  if compgen -G "/etc/udev/rules.d/*stlink*" >/dev/null 2>&1 \
     || compgen -G "/lib/udev/rules.d/*stlink*" >/dev/null 2>&1 \
     || compgen -G "/usr/lib/udev/rules.d/*stlink*" >/dev/null 2>&1; then
    ok "ST-Link udev rules already present (installed by the stlink-tools package, or a prior run of this script)"
  elif [ "$CHECK_ONLY" = 1 ]; then
    fail "no ST-Link udev rules found -- every st-flash/openocd command will need sudo until fixed"
  else
    echo "  Without udev rules, the ST-Link's raw USB device node is root-owned, so every"
    echo "  st-flash/openocd command needs sudo. VID:PID list below matches the same"
    echo "  variants OpenOCD's own interface/stlink.cfg already recognizes (V2/V2-1/V3)."
    if confirm "Install ST-Link udev rules to /etc/udev/rules.d/49-stlink.rules (needs sudo)?"; then
      sudo tee /etc/udev/rules.d/49-stlink.rules > /dev/null <<'EOF'
# ST-Link V2 / V2-1 / V3 -- USB permissions for non-root access.
# VID:PID list matches OpenOCD's interface/stlink.cfg hla_vid_pid entries.
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="3744", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="3748", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="374b", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="374d", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="374e", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="374f", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="3752", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="3753", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="3754", MODE="0666", GROUP="plugdev", TAG+="uaccess"
EOF
      sudo udevadm control --reload-rules 2>/dev/null
      sudo udevadm trigger 2>/dev/null
      ok "wrote /etc/udev/rules.d/49-stlink.rules -- unplug/replug the ST-Link for it to take effect"
    else
      warn "skipped -- st-flash/openocd will need sudo until you install these rules"
    fi
  fi

  step "Linux: serial port access (dialout group)"
  if groups "$USER" 2>/dev/null | grep -qw dialout; then
    ok "$USER is already in the 'dialout' group (needed for /dev/ttyACM*//dev/ttyUSB* access)"
  elif [ "$CHECK_ONLY" = 1 ]; then
    fail "$USER not in 'dialout' group -- opening the Nucleo's debug UART will need sudo until fixed"
  else
    echo "  Without this, tools/*.py (or a plain screen/minicom) can't open the"
    echo "  Nucleo's /dev/ttyACM0-style debug UART without sudo."
    if confirm "Add $USER to the 'dialout' group (needs sudo; requires logout/login to take effect)?"; then
      sudo usermod -aG dialout "$USER"
      ok "added $USER to 'dialout' -- LOG OUT AND BACK IN (or reboot) for this to take effect"
    else
      warn "skipped -- serial tools will need sudo until you add yourself to 'dialout'"
    fi
  fi
fi

# =====================================================================
# RTL-SDR system library -- pyrtlsdr (installed below via
# requirements.txt) is a binding to this; without the actual library
# and udev permissions, s2s2_test_suite.py's SDR pane connects to
# nothing. rtl_test's own probe is the real test (a Python import alone
# doesn't confirm the library is actually linkable/found).
# =====================================================================

step "RTL-SDR system library (librtlsdr)"
if command -v rtl_test >/dev/null 2>&1; then
  ok "librtlsdr found (rtl_test on PATH)"
elif [ "$CHECK_ONLY" = 1 ]; then
  fail "librtlsdr not found -- the SDR pane in s2s2_test_suite.py won't be able to connect"
else
  if [ "$PLATFORM" = macos ] && command -v brew >/dev/null 2>&1; then
    brew install librtlsdr
  elif [ "$PLATFORM" = linux ] && command -v apt-get >/dev/null 2>&1; then
    sudo apt-get install -y librtlsdr-dev rtl-sdr
    if [ ! -f /etc/modprobe.d/blacklist-rtl.conf ]; then
      echo "  The kernel's own dvb_usb_rtl28xxu driver auto-claims RTL2832U dongles by"
      echo "  default on Linux, blocking rtl_test/pyrtlsdr from opening them at all."
      if confirm "Blacklist dvb_usb_rtl28xxu so the RTL-SDR dongle is available to userspace tools (needs sudo)?"; then
        echo "blacklist dvb_usb_rtl28xxu" | sudo tee /etc/modprobe.d/blacklist-rtl.conf > /dev/null
        warn "wrote /etc/modprobe.d/blacklist-rtl.conf -- unplug/replug the RTL-SDR dongle (or reboot) for it to take effect"
      fi
    fi
  else
    warn "install librtlsdr manually for your platform (e.g. librtlsdr-dev on most Linux distros)"
  fi
  command -v rtl_test >/dev/null 2>&1 && ok "librtlsdr installed" \
    || warn "still not found -- only matters if you use the SDR pane with a real RTL-SDR dongle"
fi

# =====================================================================
# Python tooling (tools/*.py)
# =====================================================================

step "Python tooling"
if [ "$CHECK_ONLY" = 1 ]; then
  command -v python3 >/dev/null 2>&1 && ok "python3 found" || fail "python3 not found"
else
  if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 not found -- install it first (brew install python3 / apt install python3)"
  else
    VENV_DIR="$REPO_ROOT/tools/.venv"
    if [ ! -d "$VENV_DIR" ]; then
      python3 -m venv "$VENV_DIR"
    fi
    # shellcheck disable=SC1091
    source "$VENV_DIR/bin/activate"
    pip install --quiet --upgrade pip
    pip install --quiet -r "$REPO_ROOT/tools/requirements.txt"
    deactivate
    ok "venv ready at tools/.venv (activate it: source tools/.venv/bin/activate)"
  fi
fi

# =====================================================================
# clangd / IDE support -- compile_flags.txt, derived from the real
# Makefile CFLAGS so it can't drift out of sync with the actual build.
# =====================================================================

step "compile_flags.txt (clangd / IDE header resolution)"
if [ "$CHECK_ONLY" = 1 ]; then
  [ -f "$FW_DIR/compile_flags.txt" ] && ok "exists" || fail "missing -- run without --check"
else
  EXTRA=""
  [ -f "$FW_DIR/.toolchain_path" ] && EXTRA="ARM_TOOLCHAIN_BIN=$(cat "$FW_DIR/.toolchain_path")"
  if ( cd "$FW_DIR" && make $EXTRA compile_flags ); then
    ok "wrote compile_flags.txt"
  else
    fail "failed to generate compile_flags.txt"
  fi
fi

# =====================================================================
# Optional: NuttX Kconfig tools (kconfig-tweak, kconfig-mconf) -- only
# needed if you also build CPU1 (the NuttX OBC firmware under nuttx/ +
# apps/), not for CPU2 alone. Plain upstream kconfig-frontends is too
# old to parse modern NuttX Kconfig's `imply` keyword (see MASTER.md),
# so this builds the maintained fork from source -- no standard distro
# package provides a new-enough version.
# =====================================================================

step "Optional: NuttX Kconfig tools (only needed to build CPU1)"
if command -v kconfig-tweak >/dev/null 2>&1 && command -v kconfig-mconf >/dev/null 2>&1; then
  ok "kconfig-tweak/kconfig-mconf already on PATH"
elif [ "$CHECK_ONLY" = 1 ]; then
  warn "kconfig-tweak/kconfig-mconf not found (only matters if you plan to build CPU1/NuttX)"
else
  if confirm "Build+install kconfig-frontends from source (github.com/patacongo/tools) -- only needed for CPU1/NuttX, skip if you're only working on CPU2?"; then
    if [ "$PLATFORM" = linux ] && command -v apt-get >/dev/null 2>&1; then
      sudo apt-get install -y build-essential gperf flex bison libncurses5-dev \
        pkg-config autoconf automake libtool
    elif [ "$PLATFORM" = macos ] && command -v brew >/dev/null 2>&1; then
      brew install gperf flex bison ncurses autoconf automake libtool pkg-config
    fi
    KCONFIG_TMP="$(mktemp -d)"
    if git clone --depth 1 https://github.com/patacongo/tools.git "$KCONFIG_TMP"; then
      ( cd "$KCONFIG_TMP/kconfig-frontends" \
        && ./configure --enable-mconf --disable-gconf --disable-qconf \
        && make && sudo make install )
      [ "$PLATFORM" = linux ] && sudo ldconfig 2>/dev/null
    fi
    rm -rf "$KCONFIG_TMP"
    if command -v kconfig-tweak >/dev/null 2>&1; then
      ok "kconfig-frontends installed"
    else
      fail "kconfig-frontends build failed -- see https://github.com/patacongo/tools for manual steps"
    fi
  else
    warn "skipped -- fine if you're only building CPU2, needed before building CPU1"
  fi
fi

# =====================================================================
# Known gotchas -- real issues hit and root-caused on real hardware/a
# real machine this project, that setup.sh can't fix automatically
# (either they're one-time per-BOARD state, not per-machine, or the
# root cause is still open). Documented here once, so the next person
# doesn't have to re-derive them from scratch.
# =====================================================================

step "Known gotchas (read once, nothing to run here)"

echo "  * \"#warning user to provide its board definitions pins\" during every CPU2"
echo "    build (target/radio_board_if.h): this is expected and harmless. It's a"
echo "    stock STM32CubeMX-generated placeholder that fires whenever USE_BSP_DRIVER"
echo "    is undefined -- this project defines pins directly elsewhere instead of"
echo "    through ST's BSP layer, so the placeholder is simply never filled in and"
echo "    never needs to be. You do NOT need to run STM32CubeMX yourself for CPU2;"
echo "    only the STM32CubeWL SDK checked for above is required."
echo
echo "  * Freshly-flashed board never produces any UART output at all, even though"
echo "    flashing itself reports success: CPU2 (the M0+ core) only starts running"
echo "    if FLASH_SRRVR's SBRV field already points at 0x08020000 (this firmware's"
echo "    link address) -- on a board previously used with a different CPU2 image at"
echo "    a different flash offset (e.g. an ST demo), it can be stale, and CPU2 just"
echo "    stays held in reset forever with no error. This is PER-BOARD state in the"
echo "    chip's own option bytes, not per-machine -- setup.sh can't detect or fix it"
echo "    without a board connected. If you hit this: tools/boot_cpu2.cfg is a"
echo "    ready-made OpenOCD script that rewrites SBRV correctly (one-time per"
echo "    board); it needs real dual-core SWD-DAP access, i.e. interface/stlink-dap.cfg"
echo "    with 'transport select dapdirect_swd' and 'set DUAL_CORE 1' -- plain"
echo "    interface/stlink.cfg (HLA transport, what plain st-flash/make flash_sat"
echo "    use) cannot see CPU2 as a target at all, by design (HLA doesn't support"
echo "    multicore debugging)."
echo
echo "  * Building CPU1 (NuttX OBC, cd nuttx && make) can silently pick up a"
echo "    different, broken arm-none-eabi-gcc than the one this script just"
echo "    verified: NuttX's own build system reads plain PATH, it does NOT consult"
echo "    .toolchain_path. If \`make\` inside nuttx/ fails with something like"
echo "    \"math.h: No such file\" deep in libc, run \`which arm-none-eabi-gcc\` first --"
echo "    if it's Homebrew's (/opt/homebrew or /usr/local) rather than the ARM GNU"
echo "    Toolchain cask, put the cask's bin dir earlier in PATH before building CPU1."
echo
echo "  * apps/.config / apps/import/scripts/Make.defs missing, causing"
echo "    'apps/libapps.a: No such file or directory' when building CPU1: hit this"
echo "    once and did NOT find a fix in the time available -- \`make context\` from"
echo "    nuttx/ did not regenerate apps/.config on that occasion. Unresolved; if you"
echo "    hit this, you're in NuttX's own build-system territory, not this repo's."

# =====================================================================
# Summary
# =====================================================================

step "Next steps"
echo "  Build + flash satellite:  cd JC2 && make flash_sat"
echo "  Build + flash GS:         cd JC2 && make flash_gs"
echo "  Same via TUI:              python3 tools/flasher.py   (after: source tools/.venv/bin/activate)"
echo "  Mission control dashboard: python3 tools/mission_control_api.py"
echo "  S2S2 Test Suite (web GUI): python3 tools/s2s2_test_suite.py"
echo "  Build CPU1 (NuttX OBC):    cd nuttx && ./tools/configure.sh nucleo-wl55jc:demo && make -j4"
echo "  Re-check without changing anything: ./setup.sh --check"
if [ "$PLATFORM" = linux ]; then
  echo
  echo "  If this just added you to the 'dialout' group: log out and back in"
  echo "  (or reboot) before /dev/ttyACM0-style ports work without sudo."
fi
