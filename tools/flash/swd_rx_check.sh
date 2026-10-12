#!/usr/bin/env bash
# Passive, read-only SWD check of the satellite receive path.
#
#   tools/flash/swd_rx_check.sh [-s SERIAL] [-e FIRMWARE.elf] [-n SAMPLES] [-i SECONDS] [-x rx|tx|any] [-w SYMBOL]... [-y] [-l] [-p]
#
#   -l  list the ST-Links and exit (USB registry only, no probe is opened)
#   -s  ST-Link serial, or loc:<usb-port> (e.g. loc:1-2.3) for probes with a junk serial (default: pick from the list)
#   -e  CPU2 ELF (default ./satellite.elf if present): resolves and reads app_state, rx_done_flag, last_rx_rssi, g_satRx ... (our and the reference firmware)
#   -n  samples (default 1), -i seconds between samples (default 2)
#   -x  expected RF switch state while sampling (default rx: PC4=1, PC5=0); use -x any for a timeline over CW and listen windows
#   -w  extra symbol to read (needs -e), repeatable; only RAM addresses are accepted
#   -y  do not ask for the final confirmation
#   -p  print the generated OpenOCD scripts and exit (no hardware access)
#
# SAFETY (this runs against an expensive board):
#   * It does NOT use target/stm32wlx.cfg. That file writes DBGMCU_CR / DBGMCU_APB1_FZR1 on attach and the Cortex-M target type
#     writes DHCSR / DEMCR. Here only a bare MEM-AP target is used: bus reads, no core is touched.
#   * No reset (reset_config none, no srst), no halt, no resume, no memory or register write, no flash command.
#   * Only an allow-list of addresses is read (a handful of GPIO / RCC / PWR / FLASH_SRRVR / DBGMCU_IDCODE registers, none of
#     them read-sensitive, plus RAM 0x20000000-0x2000FFFF). Any other address is refused inside the script.
#   * Before OpenOCD starts, the generated script is scanned for write / reset / halt / flash commands and refused if one is found.
#   * SWD clock 500 kHz. Attaching still powers up the debug domain of the chip (as any SWD tool does) and uses the MEM-AP's own
#     CSW / TAR registers; that is the only thing written.
#   * Only one tool may hold the ST-Link: stop the testbench profiler and any other OpenOCD first.
set -u

SERIAL=""; ELF=""; SAMPLES=1; INTERVAL=2; EXPECT="rx"; EXTRA=""; PRINT_ONLY=0; LIST_ONLY=0; ASSUME_YES=0

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; }

while getopts "s:e:n:i:x:w:plyh" o; do
  case "$o" in
    s) SERIAL="$OPTARG" ;;
    e) ELF="$OPTARG" ;;
    n) SAMPLES="$OPTARG" ;;
    i) INTERVAL="$OPTARG" ;;
    x) EXPECT="$OPTARG" ;;
    w) EXTRA="$EXTRA $OPTARG" ;;
    p) PRINT_ONLY=1 ;;
    l) LIST_ONLY=1 ;;
    y) ASSUME_YES=1 ;;
    h|*) usage; exit 2 ;;
  esac
done
# default ELF: ./satellite.elf when the script is run from the firmware build folder
if [ -z "$ELF" ] && [ -f ./satellite.elf ]; then ELF=./satellite.elf; echo "using ./satellite.elf"; fi
case "$SAMPLES" in ''|*[!0-9]*) echo "-n needs a number" >&2; exit 2 ;; esac
case "$INTERVAL" in ''|*[!0-9]*) echo "-i needs whole seconds" >&2; exit 2 ;; esac
case "$EXPECT" in rx|tx|any) ;; *) echo "-x needs rx, tx or any" >&2; exit 2 ;; esac
[ "$SAMPLES" -ge 1 ] || { echo "-n must be >= 1" >&2; exit 2; }

# ---------------------------------------------------------------- ST-Links from the USB registry ("serial|product"); never opens a probe
list_probes() {
  case "$(uname -s)" in
    Darwin)
      ioreg -p IOUSB -l -w0 2>/dev/null | awk '
        function flush() {
          if (ser != "" && (vid == 1155 || toupper(name) ~ /ST-?LINK/)) {
            if (ser !~ /^[A-Za-z0-9]+$/ || length(ser) < 8) print "any:" ser "|" (name == "" ? "ST device" : name) " (odd serial, no selector: needs to be the only ST-Link)"
            else print ser "|" (name == "" ? "ST device" : name)
          }
          name = ""; ser = ""; vid = 0 }
        /\+-o /               { flush() }
        /"USB Product Name"/  { n = $0; sub(/.*= "/, "", n); sub(/".*/, "", n); name = n }
        /"USB Serial Number"/ { s = $0; sub(/.*= "/, "", s); sub(/".*/, "", s); ser = s }
        /"idVendor" =/        { v = $0; sub(/.*= /, "", v); vid = v + 0 }
        END                   { flush() }'
      ;;
    *)
      for d in /sys/bus/usb/devices/*; do
        [ "$(cat "$d/idVendor" 2>/dev/null)" = "0483" ] || continue
        loc="$(basename "$d")"
        case "$loc" in *:*) continue ;; esac
        ser="$(tr -d '\000' < "$d/serial" 2>/dev/null)"
        prod="$(cat "$d/product" 2>/dev/null)"
        if printf '%s' "$ser" | grep -qE '^[A-Za-z0-9]{8,}$'; then
          echo "$ser|$prod"
        else
          # clone / old V2 probes report a short or binary serial: select by USB port instead
          echo "loc:$loc|$prod (odd serial '$(printf '%s' "$ser" | tr -c '[:print:]' '?')', selected by USB port $loc)"
        fi
      done
      ;;
  esac
}

known_name() {
  case "$1" in
    002D001B4741500920383733) echo "known: satellite Nucleo (ST-Link V3)" ;;
    002E0035*)                echo "known: Nucleo used as satellite/GS stand-in" ;;
    303030303030303030303031) echo "known: ground-station Nucleo (ST-Link V2)" ;;
    *)                        echo "" ;;
  esac
}

SERIALS=(); NAMES=()
while IFS='|' read -r s n; do
  [ -n "$s" ] || continue
  SERIALS+=("$s"); NAMES+=("$n")
done <<EOF
$(list_probes | sort -u)
EOF
NP=${#SERIALS[@]}

show_probes() {
  local i
  if [ "$NP" -eq 0 ]; then echo "no ST-Link found in the USB registry"; return; fi
  echo "$NP ST-Link(s) found (USB descriptors only, nothing opened):"
  for ((i = 0; i < NP; i++)); do
    printf '  %d) %-26s %-18s %s\n' "$((i + 1))" "${SERIALS[$i]}" "${NAMES[$i]}" "$(known_name "${SERIALS[$i]}")"
  done
}

if [ "$LIST_ONLY" -eq 1 ]; then show_probes; exit 0; fi

if [ -z "$SERIAL" ] && [ "$PRINT_ONLY" -eq 0 ]; then
  show_probes
  [ "$NP" -gt 0 ] || exit 2
  [ -r /dev/tty ] || { echo "no terminal to select on: give -s <serial>" >&2; exit 2; }
  if [ "$NP" -eq 1 ]; then
    printf 'Select probe number [1] (Enter = 1, q = quit): ' > /dev/tty
  else
    printf 'Select probe number 1-%d (q = quit): ' "$NP" > /dev/tty
  fi
  read -r ans < /dev/tty
  if [ -z "$ans" ] && [ "$NP" -eq 1 ]; then ans=1; fi
  case "$ans" in
    ''|q|Q) echo "nothing selected"; exit 2 ;;
    *[!0-9]*) echo "not a number" >&2; exit 2 ;;
  esac
  if [ "$ans" -lt 1 ] || [ "$ans" -gt "$NP" ]; then echo "no such probe" >&2; exit 2; fi
  SERIAL="${SERIALS[$((ans - 1))]}"
fi
case "$SERIAL" in any:*) if [ "$NP" -gt 1 ]; then echo "this probe has no usable serial and several ST-Links are attached: unplug the others" >&2; exit 2; fi ;; esac

# ---------------------------------------------------------------- symbols (name addr size) from the ELF
WANT="app_state rx_done_flag rx_timeout_flag rx_error_flag rx_frame_size last_rx_rssi last_rx_snr s_cmd_service_active s_dcdc_hold g_system_tick_ms g_boardId g_satRx$EXTRA"
SYMS=""
if [ -n "$ELF" ]; then
  [ -f "$ELF" ] || { echo "ELF not found: $ELF" >&2; exit 2; }
  NM="$(command -v arm-none-eabi-nm || ls /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-nm 2>/dev/null | tail -1)"
  [ -n "$NM" ] || { echo "arm-none-eabi-nm not found" >&2; exit 2; }
  SYMS="$("$NM" -S "$ELF" | awk -v want="$WANT" '
    BEGIN { n = split(want, a, " "); for (i = 1; i <= n; i++) w[a[i]] = 1 }
    { name = $NF; if (!(name in w)) next
      if (NF == 4) printf "%s 0x%s 0x%s ", name, $1, $2
      else         printf "%s 0x%s 0x4 ", name, $1 }')"
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/swdcheck.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
TCL="$TMP/check.tcl"
CFG="$TMP/passive.cfg"

# bare MEM-AP target: no Cortex-M target, so no DHCSR / DEMCR / DBGMCU access, and not the stock stm32wlx.cfg (which writes on attach)
cat > "$CFG" <<'CFG'
source [find target/swj-dp.tcl]
swj_newdap chip cpu -irlen 4 -ircapture 0x1 -irmask 0xf -expected-id 0x6ba02477
dap create chip.dap -chain-position chip.cpu
target create chip.ahb mem_ap -dap chip.dap -ap-num 0
CFG

{
  echo "set EXPECT {$EXPECT}"
  echo "set SAMPLES $SAMPLES"
  echo "set INTERVAL_MS $((INTERVAL * 1000))"
  echo "set SYMS {$SYMS}"
  cat <<'TCL'
set fails 0
set warns 0

# read allow-list: exact register addresses (none has a read side effect) + RAM
set ALLOWED {0xE0042000 0x5800040C 0x58000414 0x5800014C 0x58000164 0x58004084 0x48000000 0x48000010 0x48000014 0x48000800 0x48000810 0x48000814}

proc out {lvl name text} {
    global fails warns
    if {$lvl eq "FAIL"} { incr fails }
    if {$lvl eq "WARN"} { incr warns }
    puts [format "\[%-4s\] %-26s %s" $lvl $name $text]
}
proc rd32 {a} {
    global ALLOWED
    set a [expr {$a + 0}]
    set ok 0
    if {$a >= 0x20000000 && $a <= 0x2000FFFC} { set ok 1 }
    foreach p $ALLOWED { if {[expr {$p + 0}] == $a} { set ok 1 } }
    if {!$ok} { error "read of [format 0x%08X $a] refused: not on the allow-list" }
    return [expr {[lindex [read_memory $a 32 1] 0] & 0xFFFFFFFF}]
}
proc hex8 {v} { return [format "0x%08X" $v] }
proc bitv {v n} { return [expr {($v >> $n) & 1}] }

proc rd_sym {addr size} {
    set base [expr {$addr & ~3}]
    set sh [expr {($addr & 3) * 8}]
    set w [rd32 $base]
    if {$size >= 4} { return $w }
    return [expr {($w >> $sh) & ((1 << (8 * $size)) - 1)}]
}
proc signed {v size} {
    set top [expr {1 << (8 * $size - 1)}]
    if {$v & $top} { return [expr {$v - (1 << (8 * $size))}] }
    return $v
}

set RX_FIELDS {magic rx_done rx_timeout rx_error bad_len decode_fail decode_ok digi validate_fail own_echo empty auth_fail not_tc incomplete accepted ring_ok ring_fail last_tick}
array set first {}
array set last {}
array set seen {}
set TL {}

proc check_static {} {
    set id [rd32 0xE0042000]
    set dev [expr {$id & 0xFFF}]
    if {$dev == 0x497} {
        out PASS "device id" "[hex8 $id] = STM32WL5x/WLEx"
    } else {
        out FAIL "device id" "[hex8 $id], DEV_ID 0x[format %X $dev] is not 0x497 (STM32WL5x)"
    }

    set cr4 [rd32 0x5800040C]
    if {[bitv $cr4 15]} {
        out PASS "CPU2 released" "PWR_CR4 [hex8 $cr4], C2BOOT set"
    } else {
        out FAIL "CPU2 released" "PWR_CR4 [hex8 $cr4], C2BOOT clear: CPU1 never started CPU2"
    }
    set srrvr [rd32 0x58004084]
    set sbrv [expr {$srrvr & 0xFFFF}]
    out INFO "CPU2 boot address" "FLASH_SRRVR [hex8 $srrvr], SBRV 0x[format %X $sbrv] -> 0x[format %08X [expr {0x08000000 + $sbrv * 4}]]"

    set ahb2 [rd32 0x5800014C]
    set apb3 [rd32 0x58000164]
    foreach {nm v pos} [list "CPU2 clock GPIOA" $ahb2 0 "CPU2 clock GPIOC" $ahb2 2 "CPU2 clock SUBGHZSPI" $apb3 0] {
        if {[bitv $v $pos]} {
            out PASS $nm "enabled"
        } else {
            out FAIL $nm "off: CPU2 has not initialised this block (CPU2 not running or stopped before the radio setup)"
        }
    }
}

proc check_sample {k} {
    global EXPECT SYMS RX_FIELDS first last seen TL
    puts ""
    puts "---- sample $k  ([clock format [clock seconds] -format %H:%M:%S]) ----"

    set sr2 [rd32 0x58000414]
    set busys [bitv $sr2 1]
    set busym [bitv $sr2 2]
    # ST HAL (HAL_SUBGHZ_WaitOnBusy): the radio is busy only while RFBUSYS and RFBUSYMS are both set
    if {$busys && $busym} {
        out WARN "radio BUSY" "PWR_SR2 [hex8 $sr2]: RFBUSYS=1 and RFBUSYMS=1 (if this stays so in every sample the radio is hung)"
    } else {
        out PASS "radio BUSY" "not busy (RFBUSYS=$busys RFBUSYMS=$busym)"
    }
    out INFO "PWR_SR2" "[hex8 $sr2]: C2BOOTS=[bitv $sr2 0] SMPSRDY=[bitv $sr2 3] LDORDY=[bitv $sr2 4] RFEOLF=[bitv $sr2 5] REGMRS=[bitv $sr2 6] FLASHRDY=[bitv $sr2 7] REGLPS=[bitv $sr2 8]"
    if {[bitv $sr2 5]} {
        out WARN "radio EOL flag" "PWR_SR2.RFEOLF=1 (radio end-of-life flag, see RM0453): check the radio supply voltage"
    }

    set am [rd32 0x48000000]; set ai [rd32 0x48000010]
    set cm [rd32 0x48000800]; set ci [rd32 0x48000810]; set co [rd32 0x48000814]
    foreach pin {2 3 4 5} {
        set mode [expr {($cm >> (2 * $pin)) & 3}]
        if {$mode != 1} { out FAIL "PC$pin mode" "MODER field $mode, not output (CPU2 has not set the pin up)" }
    }
    set pa0m [expr {$am & 3}]
    set c1 [bitv $ci 4]; set c2 [bitv $ci 5]
    set pc2 [bitv $ci 2]; set pc3 [bitv $ci 3]; set pa0 [bitv $ai 0]
    out INFO "pins (IDR)" "PA0(5V rail)=$pa0 PC2(3V3 PA)=$pc2 PC3(5V PA)=$pc3 PC4(FE_CTRL1)=$c1 PC5(FE_CTRL2)=$c2   ODR PC4=[bitv $co 4] PC5=[bitv $co 5]"
    if {[bitv $co 4] != $c1 || [bitv $co 5] != $c2} {
        out WARN "switch ODR vs IDR" "pin level differs from what CPU2 drives: pin shorted or driven by something else"
    }
    if {$pa0m != 1} { out WARN "PA0 mode" "MODER field $pa0m, not output" }

    if {$c1 == 1 && $c2 == 0} { set state RX } elseif {$c1 == 0 && $c2 == 1} { set state TX } elseif {$c1 == 1 && $c2 == 1} { set state "both-high" } else { set state "both-low" }
    set ts [clock format [clock seconds] -format %H:%M:%S]
    set appst "-"; set rxf "-"
    if {![info exists seen($state)]} { set seen($state) 0 }
    incr seen($state)
    switch $EXPECT {
        rx {
            if {$state eq "RX"} {
                out PASS "RF switch" "RX (PC4=1, PC5=0)"
            } elseif {$state eq "TX"} {
                out FAIL "RF switch" "TX state while listening: CW beacon running, or the switch was left in TX (deaf)"
            } else {
                if {$state eq "both-low"} {
                    out FAIL "RF switch" "OFF (PC4=0, PC5=0): normal between CW elements, during CW setup and at boot; if it stays so in every sample, RX is never armed. Sample longer: -n 15 -i 2, with -e"
                } else {
                    out FAIL "RF switch" "$state (PC4=$c1, PC5=$c2): invalid combination"
                }
            }
            if {$pc3} { out FAIL "5 V PA enable (PC3)" "high in RX: 5 V PA enabled while listening" }
            if {$pa0} { out WARN "5 V rail (PA0)" "high in RX (expected low unless a GMSK session holds it)" }
        }
        tx  {
            if {$state eq "TX"} { out PASS "RF switch" "TX (PC4=0, PC5=1)" } else { out FAIL "RF switch" "$state, expected TX" }
        }
        default { out INFO "RF switch" $state }
    }

    # ---- firmware variables (RAM only)
    set tick0 ""
    foreach {name addr size} $SYMS {
        if {$name eq "g_satRx"} { continue }
        set s [expr {$size}]
        if {$s > 4 || $s == 3} { set s 4 }
        if {[catch {rd_sym [expr {$addr}] $s} v]} { out FAIL $name $v; continue }
        set txt "[format 0x%X $v]  ($v)"
        if {[string match *rssi* $name] || [string match *snr* $name]} { set txt "$txt  signed [signed $v $s]" }
        if {$name eq "g_boardId"} {
            switch [format %08X $v] {
                B0A2D001 { set txt "$txt  Nucleo" }
                B0A2D002 { set txt "$txt  custom board" }
                B0A2D003 { set txt "$txt  CUBUS" }
            }
        }
        if {$name eq "g_system_tick_ms"} { set tick0 $v }
        if {$name eq "app_state"} { set appst $v }
        if {$name eq "rx_done_flag"} { set rxf $v }
        if {$name eq "s_dcdc_hold" && $v} {
            out WARN $name "1: RBI_SWITCH_RX is skipped while the 5 V PA hold is set"
        } else {
            out INFO $name $txt
        }
    }
    lappend TL "$ts  [format %-9s $state] PA0=$pa0 PC2=$pc2 PC3=$pc3  app_state=$appst rx_done_flag=$rxf"
    if {$tick0 ne ""} {
        foreach {name addr size} $SYMS {
            if {$name eq "g_system_tick_ms"} {
                sleep 300
                set tick1 [rd_sym [expr {$addr}] 4]
                set d [expr {($tick1 - $tick0) & 0xFFFFFFFF}]
                if {$d > 100 && $d < 5000} { out PASS "CPU2 alive" "system tick +${d} ms in 300 ms" } else { out FAIL "CPU2 alive" "system tick moved $d ms in 300 ms: CPU2 stopped or stuck" }
            }
        }
    }

    # ---- receive counters (our firmware)
    foreach {name addr size} $SYMS {
        if {$name ne "g_satRx"} continue
        set a [expr {$addr}]
        set i 0
        foreach f $RX_FIELDS {
            set v [rd32 [expr {$a + 4 * $i}]]
            set rx($f) $v
            if {![info exists first($f)]} { set first($f) $v }
            set last($f) $v
            incr i
        }
        if {$rx(magic) != 0x52583031} {
            out WARN "g_satRx" "magic [hex8 $rx(magic)]: counters not initialised (older firmware or not booted)"
            continue
        }
        set line ""
        foreach f [lrange $RX_FIELDS 1 end] { append line "$f=$rx($f) " }
        out INFO "g_satRx" $line
        if {$rx(rx_done) == 0} {
            out WARN "receive: radio" "rx_done=0, the radio completed no packet (RF path, switch, clock, preamble/sync config)"
        } elseif {$rx(decode_ok) == 0} {
            out FAIL "receive: decode" "rx_done=$rx(rx_done) but decode_ok=0 (bit alignment / framing)"
        } elseif {$rx(auth_fail) > 0 && $rx(accepted) == 0} {
            out FAIL "receive: AUTH" "auth_fail=$rx(auth_fail), accepted=0 (AUTH setting or key differs between the boards)"
        } elseif {$rx(accepted) == 0} {
            out WARN "receive: validate" "decode_ok=$rx(decode_ok) accepted=0 (validate_fail=$rx(validate_fail) own_echo=$rx(own_echo) not_tc=$rx(not_tc) incomplete=$rx(incomplete))"
        } elseif {$rx(ring_fail) > 0} {
            out FAIL "receive: hand-off" "accepted=$rx(accepted) ring_fail=$rx(ring_fail): commands not handed to CPU1"
        } else {
            out PASS "receive" "accepted=$rx(accepted) ring_ok=$rx(ring_ok)"
        }
    }
}

proc main {} {
    global SAMPLES INTERVAL_MS SYMS fails warns first last seen TL
    if {[catch {targets chip.ahb; rd32 0xE0042000} e]} {
        out FAIL attach "no answer from the chip ($e): wrong probe serial, another tool holds the ST-Link, SWD wiring, or board unpowered"
        puts "SWDCHECK_FAILS=$fails WARNS=$warns"
        return
    }
    out PASS attach "read-only access through the MEM-AP, no core touched"
    if {$SYMS eq ""} { out INFO symbols "no ELF given (-e): firmware variables and receive counters not read" }
    if {[catch {check_static} e]} { out FAIL static $e }
    for {set k 1} {$k <= $SAMPLES} {incr k} {
        if {[catch {check_sample $k} e]} { out FAIL "sample $k" $e }
        if {$k < $SAMPLES} { sleep $INTERVAL_MS }
    }
    puts ""
    puts "---- timeline (one line per sample) ----"
    foreach l $TL { puts "  $l" }
    puts ""
    puts "---- summary ----"
    set st ""
    foreach s [lsort [array names seen]] { append st "$s x$seen($s)  " }
    out INFO "switch states seen" $st
    if {$SAMPLES > 1 && [info exists first(rx_done)]} {
        foreach f {rx_done rx_timeout rx_error decode_ok decode_fail validate_fail auth_fail accepted ring_ok ring_fail} {
            out INFO "delta $f" "[expr {$last($f) - $first($f)}] over $SAMPLES samples"
        }
        if {$last(rx_done) == $first(rx_done)} { out WARN "delta rx_done" "no packet completed while sampling: send commands from the GS during the listen window" }
    }
    puts "SWDCHECK_FAILS=$fails WARNS=$warns"
}
main
TCL
} > "$TCL"

# ---------------------------------------------------------------- pre-flight: refuse to run a script that could change the target
FORBIDDEN='mww|mwh|mwb|mwd|mmw|mdw_x|write_memory|write_image|load_image|restore_image|reset|halt|resume|step|flash|program|verify|bp|wp|rbp|rwp|arp_[a-z_]+|apreg|dpreg|dap|irscan|drscan|pathmove|jtag|swd|cortex_m|exec|open|socket|shutdown|init|adapter|transport'
if grep -nwE "$FORBIDDEN" "$TCL" > "$TMP/forbidden.txt" 2>/dev/null; then
  echo "refusing to run: the generated script contains a forbidden command:" >&2
  cat "$TMP/forbidden.txt" >&2
  exit 3
fi

if [ "$PRINT_ONLY" -eq 1 ]; then
  echo "# ---- passive.cfg"; cat "$CFG"; echo "# ---- check.tcl"; cat "$TCL"; exit 0
fi

command -v openocd >/dev/null || { echo "openocd not found" >&2; exit 2; }

echo
echo "Probe       : $SERIAL"
echo "Access      : read-only MEM-AP reads of an allow-list of registers + RAM, 500 kHz"
echo "Not done    : no reset, no halt, no memory/register write, no flash access, no core debug registers"
echo "Expecting   : RF switch $EXPECT, $SAMPLES sample(s) every ${INTERVAL}s"
if [ "$ASSUME_YES" -ne 1 ]; then
  [ -r /dev/tty ] || { echo "no terminal to confirm on; use -y" >&2; exit 2; }
  printf 'Connect now? [y/N] ' > /dev/tty
  read -r yn < /dev/tty
  case "$yn" in y|Y|yes) ;; *) echo "aborted"; exit 2 ;; esac
fi

ERR="$TMP/openocd.err"
case "$SERIAL" in
  loc:*) ADAPTER_SEL="adapter usb location ${SERIAL#loc:}" ;;
  any:*) ADAPTER_SEL="echo using_the_only_attached_ST-Link" ;;
  *)     ADAPTER_SEL="adapter serial $SERIAL" ;;
esac
openocd -f interface/stlink-dap.cfg \
  -c "transport select dapdirect_swd" -c "$ADAPTER_SEL" \
  -c "adapter speed 500" -c "reset_config none" \
  -f "$CFG" -c init \
  -f "$TCL" -c shutdown 2>"$ERR" | tee "$TMP/out.txt"
RC=${PIPESTATUS[0]}
if [ "$RC" -ne 0 ] || ! grep -q '^SWDCHECK_FAILS=' "$TMP/out.txt"; then
  echo "openocd failed (exit $RC), last lines:" >&2
  grep -v '^Info :' "$ERR" | tail -8 >&2
  exit 2
fi
FAILS=$(sed -n 's/^SWDCHECK_FAILS=\([0-9]*\).*/\1/p' "$TMP/out.txt" | tail -1)
[ "${FAILS:-1}" -eq 0 ]
