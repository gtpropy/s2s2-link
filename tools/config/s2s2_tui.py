#!/usr/bin/env python3
"""
s2s2_tui.py -- the configuration, build and flash TUI of the S2S2 CPU2 firmware (satellite + ground station).
Like NuttX's menuconfig it is THE place where everything is configured: targets, AUTH, build options, identity
(call signs, SSIDs), radio frequencies, CW / beacon timing, the shared AUTH key and the ST-Link probes. Every change is
saved to <repo>/.s2s2.config and regenerates <repo>/generated/{s2s2_config.h, s2s2.mk, secrets.h}, which both firmware
Makefiles use; secrets.h (random AUTH key) is created automatically the first time.

  python3 tools/config/s2s2_tui.py                    interactive TUI             (make / make menuconfig)
  python3 tools/config/s2s2_tui.py --configure        generate the files, no UI   (make configure)
  python3 tools/config/s2s2_tui.py --defconfig        reset the options to the defaults (make defconfig)
  python3 tools/config/s2s2_tui.py --newkey           new random AUTH key         (make newkey)
  python3 tools/config/s2s2_tui.py --memstat          print memory statistics     (make memstat)
  python3 tools/config/s2s2_tui.py --probes           list ST-Link probes         (make probes)
  python3 tools/config/s2s2_tui.py --batch build|flash|flash-all|clean            (make build ...)
  python3 tools/config/s2s2_tui.py --dump             print config + stats as text

Tabs: Configure / Memory (flash and RAM bars, .bin sizes, largest symbols) / Output (live build and flash log).
Build/flash/generation logic lives in s2s2_core.py.
"""
import asyncio
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s2s2_core as C  # noqa: E402


def build_app():
    from rich.text import Text
    from textual.app import App, ComposeResult
    from textual.binding import Binding
    from textual.containers import Horizontal, Vertical, VerticalScroll
    from textual.widgets import (Button, Checkbox, Footer, Header, Input, Label, ProgressBar,
                                 RichLog, Select, Static, TabbedContent, TabPane)

    BLANK = getattr(Select, "BLANK", getattr(Select, "NULL", None))

    class S2S2App(App):
        TITLE = "S2S2 CPU2 firmware"
        SUB_TITLE = "satellite + ground station"
        CSS = """
        #cfg_row { height: auto; }
        .panel { border: round $primary; padding: 0 1; margin: 0 1 1 0; height: auto; width: 1fr; }
        .panel > Label.title { text-style: bold; color: $accent; margin-bottom: 1; }
        #actions { height: auto; margin-top: 1; }
        #actions Button { margin-right: 1; }
        .btnrow { height: auto; }
        .btnrow Button { margin-right: 1; }
        .pair { height: auto; }
        .pair Input { width: 1fr; margin-right: 1; }
        Input { margin-bottom: 1; }
        #cfg_error { color: $error; }
        #digi_help { color: $text-muted; }
        #key_status { margin: 1 0; color: $text-muted; }
        Select { width: 100%; margin-bottom: 1; }
        .mem { border: round $primary; padding: 0 1; margin-bottom: 1; height: auto; }
        .mem .title { text-style: bold; color: $accent; }
        .memline { margin-top: 1; }
        .crit Bar > .bar--bar { color: $error; }
        .warn Bar > .bar--bar { color: $warning; }
        #log { height: 1fr; border: round $primary; }
        #status { height: 1; color: $text-muted; }
        """
        BINDINGS = [
            Binding("b", "build", "Build"),
            Binding("f", "flash", "Flash CPU2"),
            Binding("a", "flash_all", "Flash all"),
            Binding("p", "rescan", "Rescan probes"),
            Binding("s", "save", "Save + generate"),
            Binding("c", "tab('tab_cfg')", "Configure"),
            Binding("m", "tab('tab_mem')", "Memory"),
            Binding("o", "tab('tab_out')", "Output"),
            Binding("r", "refresh_stats", "Refresh stats"),
            Binding("q", "quit_app", "Quit"),
        ]

        def __init__(self):
            super().__init__()
            self.cfg = C.load_config()
            self.probes = []
            self.busy = False
            self._loading = False

        # ---------------------------------------------------------------- layout
        def compose(self) -> ComposeResult:
            yield Header()
            with TabbedContent(initial="tab_cfg", id="tabs"):
                with TabPane("Configure", id="tab_cfg"):
                    with VerticalScroll():
                        with Horizontal(id="cfg_row"):
                            with Vertical(classes="panel"):
                                yield Label("Targets", classes="title")
                                yield Checkbox("Satellite       (cpu2/satellite)", id="cfg_build_sat")
                                yield Checkbox("Ground station  (cpu2/ground_station)", id="cfg_build_gs")
                                yield Label("Build", classes="title")
                                yield Checkbox("DEBUG   -Og -g3 (satellite will not fit 56 KB)", id="cfg_debug")
                                yield Checkbox("LOG     satellite console log (off = broken, open bug)", id="cfg_log")
                                yield Checkbox("RTC     RTC-backed UTIL_TIMER (dormant)", id="cfg_timer_rtc")
                                yield Label("Satellite RF front end")
                                yield Select([(f, f) for f in C.FRONTENDS], allow_blank=False, id="cfg_frontend")
                                yield Label("Security", classes="title")
                                yield Checkbox("AUTH    integrity tag + replay counter", id="cfg_auth")
                                yield Static("", id="key_status")
                                with Horizontal(classes="btnrow"):
                                    yield Button("New AUTH key", id="act_newkey", variant="warning")
                                    yield Button("Generate files", id="act_gen")
                            with Vertical(classes="panel"):
                                yield Label("Identity", classes="title")
                                yield Label("Project name (CW prefix)")
                                yield Input(id="cfg_project", max_length=6)
                                yield Label("Satellite call sign / SSID")
                                with Horizontal(classes="pair"):
                                    yield Input(id="cfg_sat_call", max_length=6)
                                    yield Input(id="cfg_sat_ssid", max_length=2)
                                yield Label("Ground station call sign / SSID")
                                with Horizontal(classes="pair"):
                                    yield Input(id="cfg_gs_call", max_length=6)
                                    yield Input(id="cfg_gs_ssid", max_length=2)
                                yield Label("Radio and timing", classes="title")
                                yield Label("Downlink Hz (satellite -> ground)")
                                yield Input(id="cfg_downlink_hz", max_length=9)
                                yield Label("Uplink Hz (ground -> satellite)")
                                yield Input(id="cfg_uplink_hz", max_length=9)
                                yield Label("CW dit length ms (20-200)")
                                yield Input(id="cfg_cw_unit_ms", max_length=3)
                                yield Label("Beacon period s (30-600)")
                                yield Input(id="cfg_beacon_period_s", max_length=3)
                                yield Static("", id="cfg_error")
                            with Vertical(classes="panel"):
                                yield Label("ST-Link probes", classes="title")
                                yield Static("", id="probe_count")
                                yield Label("Satellite probe")
                                yield Select([], prompt="none selected", id="sat_serial", allow_blank=True)
                                yield Label("Ground station probe")
                                yield Select([], prompt="none selected", id="gs_serial", allow_blank=True)
                                yield Button("Rescan probes (p)", id="rescan")
                        with Horizontal(id="cfg_row2"):
                            with Vertical(classes="panel"):
                                yield Label("Satellite digipeater", classes="title")
                                yield Checkbox("DIGI    repeat other stations' AX.25 UI frames", id="cfg_digi")
                                yield Checkbox("WIDE    answer WIDE2-N paths", id="cfg_digi_wide")
                                yield Checkbox("TRACE   insert our call sign into WIDE2-N paths", id="cfg_digi_trace")
                                yield Label("Alias besides our call sign (empty = none)")
                                yield Input(id="cfg_digi_alias", max_length=6)
                            with Vertical(classes="panel"):
                                yield Label("Digipeater limits", classes="title")
                                yield Label("Highest WIDE2-N accepted (1-7)")
                                yield Input(id="cfg_digi_max_n", max_length=1)
                                yield Label("Duplicate window s (0 = off)")
                                yield Input(id="cfg_digi_dedupe_s", max_length=3)
                                yield Label("Repeats per minute (0 = unlimited) / burst")
                                with Horizontal(classes="pair"):
                                    yield Input(id="cfg_digi_rate_per_min", max_length=2)
                                    yield Input(id="cfg_digi_burst", max_length=1)
                                yield Label("Minimum seconds between two repeats of one station")
                                yield Input(id="cfg_digi_src_min_s", max_length=3)
                            with Vertical(classes="panel"):
                                yield Label("Store and forward", classes="title")
                                yield Checkbox("SAF     keep repeated frames, send them again later", id="cfg_saf")
                                yield Checkbox("NOW     also repeat right away", id="cfg_saf_immediate")
                                yield Label("Sends per message (1-8) / lifetime min")
                                with Horizontal(classes="pair"):
                                    yield Input(id="cfg_saf_max_fwd", max_length=1)
                                    yield Input(id="cfg_saf_ttl_min", max_length=4)
                                yield Label("Seconds between two sends of one message")
                                yield Input(id="cfg_saf_fwd_interval_s", max_length=5)
                                yield Static("Rules, hopping, tests: docs/DIGIPEATER.md. The digipeater and the CW beacon "
                                             "share one radio: other stations are heard only while no beacon is sent.", id="digi_help")
                        with Horizontal(id="actions"):
                            yield Button("Build (b)", id="act_build", variant="primary")
                            yield Button("Flash CPU2 (f)", id="act_flash", variant="success")
                            yield Button("Flash CPU1 + boot option + CPU2 (a)", id="act_flash_all", variant="warning")
                            yield Button("Clean", id="act_clean")
                            yield Button("Defaults", id="act_defaults")
                            yield Button("Save (s)", id="act_save")
                with TabPane("Memory", id="tab_mem"):
                    with VerticalScroll():
                        for k in ("sat", "gs"):
                            with Vertical(classes="mem", id="mem_" + k):
                                yield Label("", id="title_" + k, classes="title")
                                yield Static("", id="flash_label_" + k, classes="memline")
                                yield ProgressBar(total=100, show_eta=False, id="flash_" + k)
                                yield Static("", id="ram_label_" + k, classes="memline")
                                yield ProgressBar(total=100, show_eta=False, id="ram_" + k)
                                yield Static("", id="detail_" + k, classes="memline")
                        with Vertical(classes="mem"):
                            yield Label("CPU1 (NuttX)", classes="title")
                            yield Static("", id="nuttx_label", classes="memline")
                            yield ProgressBar(total=100, show_eta=False, id="nuttx_bar")
                with TabPane("Output", id="tab_out"):
                    yield RichLog(id="log", wrap=False, highlight=False, markup=False)
                    yield Static("idle", id="status")
            yield Footer()

        async def on_mount(self):
            self._loading = True
            self.load_widgets()
            self._loading = False
            self.regenerate()                      # creates generated/secrets.h the first time
            await self.rescan_probes()
            await self.refresh_memory()

        # --------------------------------------------------------------- helpers
        def tabs(self) -> TabbedContent:
            return self.query_one("#tabs", TabbedContent)

        def log_widget(self) -> RichLog:
            return self.query_one("#log", RichLog)

        def set_status(self, text):
            self.query_one("#status", Static).update(text)

        CHECKS = ("build_sat", "build_gs", "auth", "debug", "log", "timer_rtc", "digi", "digi_wide", "digi_trace", "saf", "saf_immediate")
        TEXTS = ("project", "sat_call", "sat_ssid", "gs_call", "gs_ssid", "downlink_hz", "uplink_hz",
                 "cw_unit_ms", "beacon_period_s", "digi_alias", "digi_max_n", "digi_dedupe_s", "digi_rate_per_min",
                 "digi_burst", "digi_src_min_s", "saf_max_fwd", "saf_fwd_interval_s", "saf_ttl_min")
        INTS = ("sat_ssid", "gs_ssid", "downlink_hz", "uplink_hz", "cw_unit_ms", "beacon_period_s", "digi_max_n",
                "digi_dedupe_s", "digi_rate_per_min", "digi_burst", "digi_src_min_s", "saf_max_fwd", "saf_fwd_interval_s", "saf_ttl_min")
        _newkey_armed = 0.0

        def load_widgets(self):
            for key in self.CHECKS:
                self.query_one("#cfg_" + key, Checkbox).value = bool(self.cfg[key])
            for key in self.TEXTS:
                self.query_one("#cfg_" + key, Input).value = str(self.cfg[key])
            self.query_one("#cfg_frontend", Select).value = self.cfg["frontend"]

        def persist(self):
            C.save_config(self.cfg)
            self.regenerate()

        def regenerate(self, new_key=False):
            """Write generated/{secrets.h, s2s2_config.h, s2s2.mk}; show the key fingerprint and any problem."""
            err = self.query_one("#cfg_error", Static)
            try:
                changed = C.generate(self.cfg, new_key=new_key)
            except ValueError as e:
                err.update("not generated: " + str(e))
                return []
            err.update("")
            key = C.read_key()
            self.query_one("#key_status", Static).update(
                "key %s   generated/secrets.h (git-ignored)\nboth boards must be built with the same key" %
                (C.key_fingerprint(key) if key else "?"))
            return changed

        def _probe_options(self, current):
            opts = [("%s   %s   %s" % (p["serial"], p["version"], p["chip"]), p["serial"]) for p in self.probes]
            if current and all(s != current for _, s in opts):
                opts.append(("%s   (not connected)" % current, current))
            return opts

        async def rescan_probes(self):
            self.probes = await asyncio.to_thread(C.scan_probes)
            self._loading = True
            for key in ("sat_serial", "gs_serial"):
                sel = self.query_one("#" + key, Select)
                cur = self.cfg[key]
                sel.set_options(self._probe_options(cur))
                if cur:
                    sel.value = cur
                else:
                    sel.clear()
            self.call_after_refresh(self._done_loading)
            self.query_one("#probe_count", Static).update(
                "%d ST-Link probe(s) connected" % len(self.probes))

        def _done_loading(self):
            self._loading = False

        async def refresh_memory(self):
            stats = await asyncio.to_thread(lambda: {k: C.target_stats(k) for k in C.TARGETS})
            for k, st in stats.items():
                title = self.query_one("#title_" + k, Label)
                fl, rl = self.query_one("#flash_label_" + k, Static), self.query_one("#ram_label_" + k, Static)
                fb, rb = self.query_one("#flash_" + k, ProgressBar), self.query_one("#ram_" + k, ProgressBar)
                dt = self.query_one("#detail_" + k, Static)
                title.update("%s   %s" % (st["name"], st["bin"]))
                if not st["built"]:
                    fl.update("not built yet  (press b)")
                    rl.update("")
                    dt.update("")
                    fb.update(total=100, progress=0)
                    rb.update(total=100, progress=0)
                    continue
                sec = st["sections"]
                data, bss = sec.get(".data", 0), sec.get(".bss", 0)
                binb = st["bin_bytes"]
                ram = data + bss
                ram_all = ram + C.HEAP_STACK_RESERVE
                fl.update("FLASH   .bin [%s B] of %s B   %.1f%%   free %s B" % (
                    format(binb, ","), format(C.CPU2_FLASH_SIZE, ","), 100.0 * binb / C.CPU2_FLASH_SIZE,
                    format(C.CPU2_FLASH_SIZE - binb, ",")))
                fb.update(total=C.CPU2_FLASH_SIZE, progress=binb)
                rl.update("RAM     static [%s B] + heap/stack reserve %s B of %s B   %.1f%%   free %s B" % (
                    format(ram, ","), format(C.HEAP_STACK_RESERVE, ","), format(C.CPU2_RAM_SIZE, ","),
                    100.0 * ram_all / C.CPU2_RAM_SIZE, format(C.CPU2_RAM_SIZE - ram_all, ",")))
                rb.update(total=C.CPU2_RAM_SIZE, progress=ram_all)
                for bar, frac in ((fb, binb / C.CPU2_FLASH_SIZE), (rb, ram_all / C.CPU2_RAM_SIZE)):
                    bar.set_class(frac > 0.95, "crit")
                    bar.set_class(0.85 < frac <= 0.95, "warn")
                lines = ["sections:  .isr_vector %s B   .text(+rodata) %s B   .data(load) %s B   .bss %s B" % (
                    format(sec.get(".isr_vector", 0), ","), format(sec.get(".text", 0), ","),
                    format(data, ","), format(bss, ","))]
                if st["top"]:
                    lines.append("largest symbols:")
                    lines += ["  %6d B  %s" % (sz, name[:60]) for sz, name in st["top"]]
                dt.update("\n".join(lines))
            nb = self.query_one("#nuttx_bar", ProgressBar)
            nl = self.query_one("#nuttx_label", Static)
            if os.path.exists(C.NUTTX_BIN):
                b = os.path.getsize(C.NUTTX_BIN)
                nl.update("nuttx.bin [%s B] of %s B   %.1f%%" % (
                    format(b, ","), format(C.CPU1_FLASH_SIZE, ","), 100.0 * b / C.CPU1_FLASH_SIZE))
                nb.update(total=C.CPU1_FLASH_SIZE, progress=b)
            else:
                nl.update("nuttx/nuttx.bin not built  (make nuttx)")
                nb.update(total=100, progress=0)

        # ----------------------------------------------------------------- events
        def on_checkbox_changed(self, event: Checkbox.Changed):
            key = (event.checkbox.id or "")[4:]
            if self._loading or key not in self.CHECKS:
                return
            self.cfg[key] = bool(event.value)
            self.persist()

        def on_select_changed(self, event: Select.Changed):
            if self._loading:
                return
            v = event.value
            if event.select.id in ("sat_serial", "gs_serial"):
                self.cfg[event.select.id] = "" if (v is BLANK or v is None) else str(v)
                self.persist()
            elif event.select.id == "cfg_frontend" and v is not BLANK and v is not None:
                self.cfg["frontend"] = str(v)
                self.persist()

        def on_input_changed(self, event: Input.Changed):
            key = (event.input.id or "")[4:]
            if self._loading or key not in self.TEXTS:
                return
            text = event.value.strip()
            try:
                value = int(text) if key in self.INTS else text.upper()
            except ValueError:
                self.query_one("#cfg_error", Static).update("%s must be a number" % key)
                return
            trial = dict(self.cfg, **{key: value})
            errs = C.validate_config(trial)
            if errs:
                self.query_one("#cfg_error", Static).update("; ".join(errs))
                return
            self.cfg[key] = value
            self.persist()

        async def on_button_pressed(self, event: Button.Pressed):
            bid = event.button.id
            if bid == "rescan":
                await self.action_rescan()
            elif bid == "act_build":
                self.action_build()
            elif bid == "act_flash":
                self.action_flash()
            elif bid == "act_flash_all":
                self.action_flash_all()
            elif bid == "act_clean":
                self.action_clean()
            elif bid == "act_save":
                self.action_save()
            elif bid == "act_gen":
                changed = self.regenerate()
                self.notify("generated: " + (", ".join(changed) if changed else "already up to date"))
            elif bid == "act_defaults":
                self.action_defaults()
            elif bid == "act_newkey":
                self.action_newkey()

        # ---------------------------------------------------------------- actions
        def show_tab(self, name: str):
            # A focused widget inside the old pane pulls the tab view back to that pane,
            # so drop focus before switching.
            self.set_focus(None)
            self.tabs().active = name

        def action_tab(self, name: str):
            self.show_tab(name)

        async def action_rescan(self):
            await self.rescan_probes()
            self.notify("%d ST-Link probe(s) found" % len(self.probes))

        async def action_refresh_stats(self):
            await self.refresh_memory()
            self.notify("Memory statistics refreshed")

        def action_defaults(self):
            keep = {k: self.cfg[k] for k in ("sat_serial", "gs_serial")}
            self.cfg = dict(C.DEFAULT_CONFIG, **keep)
            self._loading = True
            self.load_widgets()
            self._loading = False
            self.persist()
            self.notify("options reset to the defaults (probes kept)")

        def action_newkey(self):
            import time
            now = time.monotonic()
            if now - self._newkey_armed > 6.0:
                self._newkey_armed = now
                self.notify("Press 'New AUTH key' again within 6 s to confirm: both boards must be rebuilt and reflashed",
                            severity="warning")
                return
            self._newkey_armed = 0.0
            self.regenerate(new_key=True)
            self.notify("new AUTH key generated: rebuild and reflash BOTH boards", severity="warning")

        def action_save(self):
            self.persist()
            self.notify("Saved " + os.path.basename(C.CONFIG_PATH))

        def action_quit_app(self):
            self.persist()
            self.exit()

        def action_build(self):
            self.start("Build", C.build_steps(self.cfg))

        def action_clean(self):
            self.start("Clean", C.clean_steps(self.cfg))

        def action_flash(self):
            self._flash(False)

        def action_flash_all(self):
            self._flash(True)

        def _flash(self, with_cpu1):
            steps, problems = C.flash_steps(self.cfg, with_cpu1=with_cpu1)
            if problems:
                self.notify("; ".join(problems), severity="error")
                return
            if with_cpu1 and not os.path.exists(C.NUTTX_BIN):
                self.notify("nuttx/nuttx.bin missing (make nuttx first)", severity="error")
                return
            self.start("Flash everything" if with_cpu1 else "Flash CPU2", steps)

        def start(self, title, steps):
            if self.busy:
                self.notify("A command is already running", severity="warning")
                return
            if not steps:
                self.notify("Nothing to do: no target selected", severity="warning")
                return
            self.set_busy(True)
            self.run_worker(self.run_steps(title, steps), exclusive=False)

        def set_busy(self, busy):
            self.busy = busy
            for bid in ("act_build", "act_flash", "act_flash_all", "act_clean"):
                self.query_one("#" + bid, Button).disabled = busy

        async def run_steps(self, title, steps):
            self.show_tab("tab_out")
            log = self.log_widget()
            log.clear()
            self.set_status("running: " + title)
            env = dict(os.environ)
            pre = C.toolchain_prefix()
            if pre:
                env["PATH"] = pre + ":" + env.get("PATH", "")
            for k in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
                env.pop(k, None)
            ok = True
            for label, argv, cwd in steps:
                log.write(Text("=== " + label, style="bold cyan"))
                log.write(Text("$ " + " ".join(argv), style="dim"))
                try:
                    proc = await asyncio.create_subprocess_exec(
                        *argv, cwd=cwd, env=env,
                        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
                except OSError as e:
                    log.write(Text("ERROR: %s" % e, style="bold red"))
                    ok = False
                    break
                pending = ""
                while True:
                    chunk = await proc.stdout.read(4096)
                    if not chunk:
                        break
                    pending += chunk.decode(errors="replace")
                    parts = re.split(r"[\r\n]+", pending)
                    pending = parts.pop()
                    for ln in parts:
                        ln = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", ln)
                        if ln.strip():
                            log.write(Text(ln))
                if pending.strip():
                    log.write(Text(re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", pending)))
                rc = await proc.wait()
                if rc != 0:
                    log.write(Text("*** FAILED (exit %d): %s" % (rc, label), style="bold red"))
                    ok = False
                    break
            if ok:
                log.write(Text("*** DONE: " + title, style="bold green"))
            self.set_status(("done: " if ok else "FAILED: ") + title)
            await self.refresh_memory()
            self.set_busy(False)
            self.notify(("Done: " if ok else "Failed: ") + title, severity="information" if ok else "error")

    return S2S2App


def configure_cli(defaults=False, new_key=False):
    """make configure / defconfig / newkey: generate the files (and the key when missing) without a UI."""
    cfg = dict(C.DEFAULT_CONFIG, **{k: C.load_config()[k] for k in ("sat_serial", "gs_serial")}) if defaults else C.load_config()
    if defaults or not os.path.exists(C.CONFIG_PATH):
        C.save_config(cfg)
    try:
        changed = C.generate(cfg, new_key=new_key)
    except ValueError as e:
        print("invalid configuration: %s" % e)
        return 1
    key = C.read_key()
    print("configuration saved in .s2s2.config" if defaults else "configuration: .s2s2.config")
    print("generated: %s" % (", ".join(changed) if changed else "up to date"))
    print("AUTH key fingerprint: %s%s" % (C.key_fingerprint(key), "   (NEW: rebuild and reflash both boards)" if new_key else ""))
    return 0


def dump():
    cfg = C.load_config()
    print("config:", cfg)
    print("probes:", C.scan_probes())
    for k in C.TARGETS:
        print("\n".join(C.format_stats(C.target_stats(k))) + "\n")
    print(C.nuttx_line())


def main(argv):
    if "--dump" in argv:
        dump()
        return 0
    if "--configure" in argv:
        return configure_cli()
    if "--defconfig" in argv:
        return configure_cli(defaults=True)
    if "--newkey" in argv:
        return configure_cli(new_key=True)
    if "--memstat" in argv:
        for k in C.TARGETS:
            print("\n".join(C.format_stats(C.target_stats(k))) + "\n")
        print(C.nuttx_line())
        return 0
    if "--probes" in argv:
        ps = C.scan_probes()
        if not ps:
            print("no ST-Link probes found")
        for p in ps:
            print("%s  %s  %s" % (p["serial"], p["version"], p["chip"]))
        return 0
    if "--batch" in argv:
        return C.batch(argv[argv.index("--batch") + 1])
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("not a terminal -- use --batch build|flash|flash-all|clean, --memstat or --probes")
        return 2
    try:
        build_app()().run()
    except ImportError as e:
        print("Textual is not available (%s).\nRun with: tools/.venv/bin/python tools/s2s2_tui.py\n"
              "or: python3 -m pip install textual" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
