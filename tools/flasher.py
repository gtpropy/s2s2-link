#!/usr/bin/env python3
"""
flasher.py -- Textual TUI front-end for the CPU2/Makefile build+flash
workflow (M0+ satellite/gs firmware over ST-Link).

Wraps two already-working command-line tools -- it invents no new build
or flash logic, and (as of 2026-10-01) no longer has its own separate
flashing path:
  - `make` (CPU2/Makefile: APP=satellite|gs, BUILD=debug|production,
    BOARD=nucleo|gs_custom|sat_custom, SERIAL=<probe serial>) for both
    building AND flashing (`make flash_sat`/`flash_gs`) -- this used to
    shell out to `st-flash` directly instead, a second, independently-
    maintained flash path that (a) doesn't go through the Makefile's own
    `flash` target at all, so any future change there silently stops
    applying to this TUI, and (b) had no way to select a specific probe
    by serial when more than one ST-Link is connected -- openocd (what
    `make flash` actually uses) needs `-c "adapter serial ..."` for that,
    which `st-flash --serial` doesn't translate to; the Makefile now
    takes SERIAL=<serial> directly and this TUI just passes it through.
  - `st-info --probe` (enumerate connected ST-Link probes by serial) --
    still used for the probe list/picker, since that part worked fine.

Run: python3 tools/flasher.py
"""

import asyncio
import re
from dataclasses import dataclass
from pathlib import Path

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import (
    Header, Footer, Static, Button, RadioSet, RadioButton, Select,
    DataTable, RichLog, Label,
)
from textual.reactive import reactive

CPU2_DIR = Path(__file__).resolve().parent.parent
FLASH_ADDR = "0x08020000"

TARGETS = {
    "satellite": "ipcc_m0plus",
    "gs": "gs_m0plus",
}


@dataclass
class Probe:
    version: str = ""
    serial: str = ""
    flash: str = ""
    sram: str = ""
    chipid: str = ""
    descr: str = ""


@dataclass
class FirmwareInfo:
    text: int = 0
    data: int = 0
    bss: int = 0


def parse_probes(output: str) -> list[Probe]:
    probes: list[Probe] = []
    current: Probe | None = None
    for line in output.splitlines():
        m = re.match(r"\s*(version|serial|flash|sram|chipid|descr):\s*(.+)", line)
        if not m:
            continue
        key, val = m.group(1), m.group(2).strip()
        if key == "version":
            if current is not None:
                probes.append(current)
            current = Probe(version=val)
        elif current is not None:
            setattr(current, key, val)
    if current is not None:
        probes.append(current)
    return probes


def parse_size(output: str) -> FirmwareInfo | None:
    # arm-none-eabi-size output: "   text    data     bss     dec     hex filename"
    #                            "  44400     608    9280   54288    d410 ipcc_m0plus.elf"
    lines = output.strip().splitlines()
    for i, line in enumerate(lines):
        if line.split()[:3] == ["text", "data", "bss"] and i + 1 < len(lines):
            parts = lines[i + 1].split()
            if len(parts) >= 3:
                return FirmwareInfo(text=int(parts[0]), data=int(parts[1]), bss=int(parts[2]))
    return None


class StatusBar(Static):
    state = reactive("IDLE")

    STATE_COLORS = {
        "IDLE": ("IDLE", "#4b4b4b"),
        "BUILDING": ("BUILDING...", "#c9a227"),
        "FLASHING": ("FLASHING...", "#1e90ff"),
        "SUCCESS": ("SUCCESS", "green"),
        "FAILED": ("FAILED", "red"),
    }

    def watch_state(self, state: str) -> None:
        label, color = self.STATE_COLORS.get(state, (state, "#4b4b4b"))
        self.update(f" {label} ")
        self.styles.background = color


class FlasherApp(App):
    CSS = """
    Screen {
        layout: vertical;
    }
    #body {
        height: 1fr;
    }
    #sidebar {
        width: 44;
        border: round $primary;
        padding: 1;
    }
    #sidebar Label {
        margin-top: 1;
        text-style: bold;
        color: $accent;
    }
    #mode_set, #build_set {
        height: auto;
        border: round $secondary;
        padding: 0 1;
    }
    #probe_table {
        height: 8;
        border: round $secondary;
    }
    #actions {
        height: auto;
        padding-top: 1;
    }
    #actions Button {
        width: 1fr;
        margin-bottom: 1;
    }
    #main {
        border: round $primary;
        padding: 0 1;
    }
    #fw_info {
        height: auto;
        padding: 1;
        border-bottom: solid $secondary;
    }
    #status_bar {
        height: 1;
        content-align: center middle;
        text-style: bold;
    }
    RichLog {
        height: 1fr;
    }
    """

    BINDINGS = [
        ("r", "refresh_probes", "Refresh probes"),
        ("b", "build", "Build"),
        ("f", "flash", "Flash"),
        ("shift+b", "build_and_flash", "Build+Flash"),
        ("q", "quit", "Quit"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self.probes: list[Probe] = []
        self.last_fw: FirmwareInfo | None = None
        # st-info and st-flash both need exclusive libusb access to the
        # ST-Link -- running them concurrently (e.g. pressing 'r' to
        # refresh probes while a flash is in progress) is a real, common
        # cause of intermittent LIBUSB_ERROR_ACCESS/BUSY. Textual's
        # per-group `exclusive=True` doesn't help here since probe-
        # refresh and build/flash/reset/verify are different worker
        # groups (refresh cancelling a running flash mid-write would be
        # worse -- could corrupt the write). This lock serializes every
        # USB-touching operation regardless of which worker group it's in.
        self._usb_lock = asyncio.Lock()

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal(id="body"):
            with Vertical(id="sidebar"):
                yield Label("Mode")
                with RadioSet(id="mode_set"):
                    yield RadioButton("Satellite (CPU2 flight)", value=True, id="mode_satellite")
                    yield RadioButton("Ground Station (CPU2 GS)", id="mode_gs")

                yield Label("Build / Board")
                with Vertical(id="build_set"):
                    yield Select(
                        [("debug", "debug"), ("production", "production")],
                        value="debug", id="build_select", allow_blank=False,
                    )
                    yield Select(
                        [("nucleo", "nucleo"), ("gs_custom", "gs_custom"), ("sat_custom", "sat_custom")],
                        value="nucleo", id="board_select", allow_blank=False,
                    )

                yield Label("ST-Link probes  (r: refresh)")
                yield DataTable(id="probe_table")

                yield Label("Actions")
                with Vertical(id="actions"):
                    yield Button("Build  (b)", id="btn_build", variant="primary")
                    yield Button("Flash  (f)", id="btn_flash", variant="warning")
                    yield Button("Build + Flash  (B)", id="btn_build_flash", variant="success")
                    yield Button("Reset MCU", id="btn_reset")
                    yield Button("Read-back Verify", id="btn_verify")

            with Vertical(id="main"):
                yield Static("", id="fw_info")
                yield RichLog(id="log", highlight=True, markup=True, wrap=False)
        yield StatusBar(id="status_bar")
        yield Footer()

    def on_mount(self) -> None:
        table = self.query_one("#probe_table", DataTable)
        table.add_columns("Serial", "Version", "Flash", "Descr")
        table.cursor_type = "row"
        self.refresh_fw_info()
        self.action_refresh_probes()
        self.log_line("[bold cyan]FLASHER ready.[/] CPU2 dir: " + str(CPU2_DIR))
        self.log_line("Pick a mode, pick a probe (or leave unselected for the only/first one), then Build/Flash.")

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def log_line(self, text: str) -> None:
        self.query_one("#log", RichLog).write(text)

    def set_status(self, state: str) -> None:
        self.query_one(StatusBar).state = state

    def _fw_info_text(self) -> str:
        mode = self.current_mode()
        target = TARGETS[mode]
        if self.last_fw is None:
            return f"target: [b]{target}[/b]   flash addr: [b]{FLASH_ADDR}[/b]   (not built yet)"
        fw = self.last_fw
        total = fw.text + fw.data
        return (
            f"target: [b]{target}[/b]   flash addr: [b]{FLASH_ADDR}[/b]   "
            f"text={fw.text}  data={fw.data}  bss={fw.bss}  "
            f"[b]flash used={total} bytes[/b]"
        )

    def refresh_fw_info(self) -> None:
        self.query_one("#fw_info", Static).update(self._fw_info_text())

    def current_mode(self) -> str:
        radio = self.query_one("#mode_set", RadioSet)
        pressed = radio.pressed_button
        if pressed is not None and pressed.id == "mode_gs":
            return "gs"
        return "satellite"

    def current_build(self) -> str:
        return str(self.query_one("#build_select", Select).value)

    def current_board(self) -> str:
        return str(self.query_one("#board_select", Select).value)

    def selected_probe_serial(self) -> str | None:
        table = self.query_one("#probe_table", DataTable)
        if table.cursor_row is None or table.row_count == 0:
            return None
        try:
            row = table.get_row_at(table.cursor_row)
        except Exception:
            return None
        serial = str(row[0])
        return serial if serial and serial != "-" else None

    def bin_path(self) -> Path:
        return CPU2_DIR / f"{TARGETS[self.current_mode()]}.bin"

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def action_refresh_probes(self) -> None:
        self.run_worker(self._refresh_probes_worker(), exclusive=True, group="probes")

    async def _refresh_probes_worker(self) -> None:
        table = self.query_one("#probe_table", DataTable)
        async with self._usb_lock:
            table.clear()
            try:
                proc = await asyncio.create_subprocess_exec(
                    "st-info", "--probe",
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
                )
                out, _ = await proc.communicate()
            except FileNotFoundError:
                self.log_line("[red]st-info not found on PATH -- install stlink-tools (brew install stlink)[/red]")
                return
            text = out.decode(errors="replace")
            self.probes = parse_probes(text)
            if not self.probes:
                table.add_row("-", "-", "-", "no probes found (plug in ST-Link, press r)")
            for p in self.probes:
                table.add_row(p.serial, p.version, p.flash, p.descr)
            self.log_line(f"[dim]probe scan:[/dim] found {len(self.probes)} ST-Link probe(s)")

    def action_build(self) -> None:
        self.run_worker(self._build_worker(), exclusive=True, group="job")

    def action_flash(self) -> None:
        self.run_worker(self._flash_worker(), exclusive=True, group="job")

    def action_build_and_flash(self) -> None:
        self.run_worker(self._build_and_flash_worker(), exclusive=True, group="job")

    def _log_usb_error_hint(self, output: str) -> None:
        """openocd/st-info wrap libusb errors (or, for openocd, a bare
        "Error: open failed" with no further detail at all) in terse,
        easy-to-miss lines. Surface a concrete next step instead of just
        the raw error -- this can't be diagnosed further without real
        hardware attached, so give the most common, actionable causes."""
        text = output.upper()
        if "ERROR: OPEN FAILED" in text:
            self.log_line(
                "[yellow]hint: openocd's \"open failed\" with no other detail almost always means "
                "either (a) more than one ST-Link is connected and no probe is selected -- pick a "
                "row in the probe table above, or (b) no ST-Link is connected at all -- press 'r' "
                "to rescan.[/yellow]")
        elif "LIBUSB_ERROR_ACCESS" in text or "PERMISSION" in text:
            self.log_line(
                "[yellow]hint: LIBUSB_ERROR_ACCESS -- on Linux this usually means missing udev "
                "rules for the ST-Link (see stlink-tools' 49-stlinkv*.rules); on macOS, check no "
                "other process (another flasher.py instance, openocd, a leftover st-flash) still "
                "has the device open.[/yellow]")
        elif "LIBUSB_ERROR_BUSY" in text or "BUSY" in text:
            self.log_line(
                "[yellow]hint: device busy -- another process has the ST-Link open right now. "
                "Close any other flasher.py/openocd/st-util instance and retry.[/yellow]")
        elif "LIBUSB_ERROR_NO_DEVICE" in text or "NO ST-LINK" in text or "COULDN'T FIND" in text:
            self.log_line(
                "[yellow]hint: no ST-Link detected -- check the USB cable (some are charge-only), "
                "try a different port, and press 'r' to rescan.[/yellow]")
        elif "LIBUSB_ERROR_IO" in text or "LIBUSB_ERROR" in text:
            self.log_line(
                "[yellow]hint: low-level USB I/O error -- often a flaky cable/hub, or the ST-Link "
                "firmware needs updating (ST-LinkUpgrade). Try a direct USB port (no hub) and a "
                "different cable before assuming it's software.[/yellow]")

    async def _run_stream(self, *cmd: str, cwd: Path | None = None) -> int:
        self.log_line(f"[dim]$ {' '.join(cmd)}[/dim]")
        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd, cwd=str(cwd) if cwd else None,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
            )
        except FileNotFoundError:
            # Without this, a missing `make`/`openocd`/`st-info` on PATH
            # raised here, inside a Textual worker coroutine, with no
            # visible error in the log at all -- the status bar would just
            # sit on FAILED (or not even update) with zero explanation,
            # which is exactly what "it always fails" looks like from the
            # outside. Surface it plainly instead.
            self.log_line(f"[bold red]'{cmd[0]}' not found on PATH -- is it installed? "
                           f"(see ../../setup.sh, or `which {cmd[0]}`)[/bold red]")
            self._last_output = ""
            self.set_status("FAILED")
            return 127
        except OSError as exc:
            self.log_line(f"[bold red]failed to launch '{cmd[0]}': {exc}[/bold red]")
            self._last_output = ""
            self.set_status("FAILED")
            return 1
        assert proc.stdout is not None
        full_output = []
        async for raw_line in proc.stdout:
            line = raw_line.decode(errors="replace").rstrip("\n")
            full_output.append(line)
            self.log_line(line)
        await proc.wait()
        self._last_output = "\n".join(full_output)
        return proc.returncode or 0

    async def _build_worker(self) -> bool:
        mode = self.current_mode()
        build = self.current_build()
        board = self.current_board()
        self.set_status("BUILDING")
        self.log_line(f"[bold yellow]== BUILD == mode={mode} build={build} board={board}[/bold yellow]")
        app_name = "satellite" if mode == "satellite" else "gs"
        rc = await self._run_stream(
            "make", f"APP={app_name}", f"BUILD={build}", f"BOARD={board}",
            cwd=CPU2_DIR,
        )
        if rc == 0:
            self.last_fw = parse_size(self._last_output)
            self.refresh_fw_info()
            self.log_line("[bold green]BUILD OK[/bold green]")
            self.set_status("SUCCESS")
        else:
            self.log_line("[bold red]BUILD FAILED[/bold red]")
            self.set_status("FAILED")
        return rc == 0

    async def _flash_worker(self) -> bool:
        bin_path = self.bin_path()
        if not bin_path.exists():
            self.log_line(f"[bold red]{bin_path.name} does not exist -- build first[/bold red]")
            self.set_status("FAILED")
            return False

        mode = self.current_mode()
        app_name = "satellite" if mode == "satellite" else "gs"
        build = self.current_build()
        board = self.current_board()
        serial = self.selected_probe_serial()

        # Goes through the Makefile's own `flash` target (openocd program+
        # verify in one step) rather than a separate st-flash invocation --
        # see the module docstring for why. SERIAL is required (not just
        # "nice to have") whenever more than one ST-Link is connected;
        # `make flash` itself refuses with a clear error if it's missing
        # and ambiguous, same guard this TUI would otherwise have to
        # duplicate.
        cmd = ["make", f"APP={app_name}", f"BUILD={build}", f"BOARD={board}", "flash"]
        if serial:
            cmd.append(f"SERIAL={serial}")

        self.set_status("FLASHING")
        self.log_line(f"[bold cyan]== FLASH == {bin_path.name} -> {FLASH_ADDR} "
                       f"(probe: {serial or 'auto/only'})[/bold cyan]")
        async with self._usb_lock:
            rc = await self._run_stream(*cmd, cwd=CPU2_DIR)
        ok = rc == 0 and "** Verified OK **" in self._last_output
        if ok:
            self.log_line("[bold green]FLASH OK -- written and verified[/bold green]")
            self.set_status("SUCCESS")
        else:
            self._log_usb_error_hint(self._last_output)
            self.log_line("[bold red]FLASH FAILED[/bold red]")
            self.set_status("FAILED")
        return ok

    async def _build_and_flash_worker(self) -> None:
        if await self._build_worker():
            await self._flash_worker()

    def _openocd_serial_args(self) -> list[str]:
        serial = self.selected_probe_serial()
        return ["-c", f"adapter serial {serial}"] if serial else []

    async def _reset_worker(self) -> None:
        serial = self.selected_probe_serial()
        cmd = [
            "openocd", *self._openocd_serial_args(),
            "-f", "interface/stlink.cfg", "-f", "target/stm32wlx.cfg",
            "-c", "init", "-c", "reset run", "-c", "exit",
        ]
        self.log_line(f"[bold cyan]== RESET == probe: {serial or 'auto/only'}[/bold cyan]")
        async with self._usb_lock:
            rc = await self._run_stream(*cmd, cwd=CPU2_DIR)
        if rc != 0:
            self._log_usb_error_hint(self._last_output)
        self.set_status("SUCCESS" if rc == 0 else "FAILED")

    async def _verify_worker(self) -> None:
        bin_path = self.bin_path()
        if not bin_path.exists():
            self.log_line(f"[bold red]{bin_path.name} does not exist -- build first[/bold red]")
            return
        serial = self.selected_probe_serial()
        # openocd's own verify_image against the flash address -- reuses
        # the same interface/target config as the Makefile's `flash`
        # target, no separate read-back/compare logic to keep in sync
        # with it.
        cmd = [
            "openocd", *self._openocd_serial_args(),
            "-f", "interface/stlink.cfg", "-f", "target/stm32wlx.cfg",
            "-c", "init", "-c", "reset halt",
            "-c", f"verify_image {bin_path.name} {FLASH_ADDR}",
            "-c", "reset run", "-c", "exit",
        ]
        self.log_line(f"[bold cyan]== VERIFY == {bin_path.name} @ {FLASH_ADDR} "
                       f"(probe: {serial or 'auto/only'})[/bold cyan]")
        async with self._usb_lock:
            rc = await self._run_stream(*cmd, cwd=CPU2_DIR)
        if rc == 0 and "verified" in self._last_output.lower():
            self.log_line("[bold green]VERIFY OK -- matches on-chip flash[/bold green]")
            self.set_status("SUCCESS")
        else:
            self._log_usb_error_hint(self._last_output)
            self.log_line("[bold red]VERIFY FAILED (mismatch, or probe/IO error above)[/bold red]")
            self.set_status("FAILED")

    # ------------------------------------------------------------------
    # Event wiring
    # ------------------------------------------------------------------

    def on_button_pressed(self, event: Button.Pressed) -> None:
        bid = event.button.id
        if bid == "btn_build":
            self.action_build()
        elif bid == "btn_flash":
            self.action_flash()
        elif bid == "btn_build_flash":
            self.action_build_and_flash()
        elif bid == "btn_reset":
            self.run_worker(self._reset_worker(), exclusive=True, group="job")
        elif bid == "btn_verify":
            self.run_worker(self._verify_worker(), exclusive=True, group="job")

    def on_radio_set_changed(self, event: RadioSet.Changed) -> None:
        if event.radio_set.id == "mode_set":
            self.last_fw = None
            self.refresh_fw_info()


if __name__ == "__main__":
    FlasherApp().run()
