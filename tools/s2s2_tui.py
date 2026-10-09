#!/usr/bin/env python3
"""
s2s2_tui.py -- Textual TUI for the S2S2 CPU2 firmware (satellite + ground station).

  tools/.venv/bin/python tools/s2s2_tui.py           interactive TUI         (make / make tui)
  python3 tools/s2s2_tui.py --memstat                print memory statistics (make memstat)
  python3 tools/s2s2_tui.py --probes                 list ST-Link probes     (make probes)
  python3 tools/s2s2_tui.py --batch build|flash|flash-all|clean              (make build ...)
  python3 tools/s2s2_tui.py --dump                   print config + stats as text

Tabs: Configure (targets, options, ST-Link selection, actions) / Memory (flash and RAM
bars, .bin sizes, largest symbols) / Output (live build and flash log).
Settings are stored in <repo>/.s2s2.config. Build/flash logic lives in s2s2_core.py.
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
    from textual.widgets import (Button, Checkbox, Footer, Header, Label, ProgressBar,
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
            Binding("s", "save", "Save"),
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
                    with Horizontal(id="cfg_row"):
                        with Vertical(classes="panel"):
                            yield Label("Targets", classes="title")
                            yield Checkbox("Satellite       (Satellite_M0+/Com_sat)", id="build_sat")
                            yield Checkbox("Ground station  (Ground_Station)", id="build_gs")
                            yield Label("Options", classes="title")
                            yield Checkbox("AUTH       integrity tag + replay counter", id="auth")
                            yield Checkbox("DEBUG      -Og -g3 (satellite will not fit 56 KB)", id="debug")
                            yield Checkbox("SEAL_TEST  extended boot self-test (+~600 B)", id="seal_test")
                        with Vertical(classes="panel"):
                            yield Label("ST-Link probes", classes="title")
                            yield Static("", id="probe_count")
                            yield Label("Satellite probe")
                            yield Select([], prompt="none selected", id="sat_serial", allow_blank=True)
                            yield Label("Ground station probe")
                            yield Select([], prompt="none selected", id="gs_serial", allow_blank=True)
                            yield Button("Rescan probes (p)", id="rescan")
                    with Horizontal(id="actions"):
                        yield Button("Build (b)", id="act_build", variant="primary")
                        yield Button("Flash CPU2 (f)", id="act_flash", variant="success")
                        yield Button("Flash CPU1 + boot option + CPU2 (a)", id="act_flash_all", variant="warning")
                        yield Button("Clean", id="act_clean")
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
            for key in ("build_sat", "build_gs", "auth", "debug", "seal_test"):
                self.query_one("#" + key, Checkbox).value = bool(self.cfg[key])
            self._loading = False
            await self.rescan_probes()
            await self.refresh_memory()

        # --------------------------------------------------------------- helpers
        def tabs(self) -> TabbedContent:
            return self.query_one("#tabs", TabbedContent)

        def log_widget(self) -> RichLog:
            return self.query_one("#log", RichLog)

        def set_status(self, text):
            self.query_one("#status", Static).update(text)

        def persist(self):
            C.save_config(self.cfg)

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
            if self._loading:
                return
            self.cfg[event.checkbox.id] = bool(event.value)
            self.persist()

        def on_select_changed(self, event: Select.Changed):
            if self._loading or event.select.id not in ("sat_serial", "gs_serial"):
                return
            v = event.value
            self.cfg[event.select.id] = "" if (v is BLANK or v is None) else str(v)
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
