#!/usr/bin/env python3
"""
s2s2_tui.py -- menuconfig for the S2S2 CPU2 firmware (satellite + ground station), in the style of NuttX's menuconfig.

The menu tree is the single place where everything is configured: targets, AUTH and its key, build options, identity (call
signs), radio frequencies and timing, the digipeater and store and forward, the ST-Link probes. Every change is saved to
<repo>/.s2s2.config and regenerates <repo>/generated/{s2s2_config.h, s2s2.mk, secrets.h}, which both firmware Makefiles use;
secrets.h (random AUTH key) is created automatically the first time. The menu itself is described in s2s2_core.MENUS.

Keys (shown at the bottom):
  Up / Down   move          Enter  open a submenu / change the value      Esc or Left  one menu back
  Space  toggle [*] / [ ]    y / n  set a bool                            ?  help of the item      s  save + generate
  b build  f flash  a flash all  p rescan probes  m memory  o output  c menu  q quit

  python3 tools/config/s2s2_tui.py                    interactive menuconfig       (make / make menuconfig)
  python3 tools/config/s2s2_tui.py --configure        generate the files, no UI    (make configure)
  python3 tools/config/s2s2_tui.py --defconfig        reset the options to the defaults (make defconfig)
  python3 tools/config/s2s2_tui.py --newkey           new random AUTH key          (make newkey)
  python3 tools/config/s2s2_tui.py --memstat          print memory statistics      (make memstat)
  python3 tools/config/s2s2_tui.py --probes           list ST-Link probes          (make probes)
  python3 tools/config/s2s2_tui.py --batch build|flash|flash-all|clean             (make build ...)
        [--only "sat gs"] [--cubus T|F]   restrict to one image / override the CUBUS option for this run (make flash sat CUBUS=T)
  python3 tools/config/s2s2_tui.py --dump             print config + stats as text

If the interactive mode cannot start (no Textual, terminal too small, an error) the reason is printed and written to
<repo>/.s2s2_tui_error.log, and the batch modes above keep working.
"""
import asyncio
import os
import re
import shutil
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s2s2_core as C  # noqa: E402

MIN_COLS, MIN_ROWS = 60, 18


def build_app():
    from rich.text import Text
    from textual.app import App, ComposeResult
    from textual.binding import Binding
    from textual.containers import Horizontal, Vertical, VerticalScroll
    from textual.screen import ModalScreen
    from textual.widgets import (Button, Footer, Header, Input, Label, OptionList, ProgressBar, RichLog, Static,
                                 TabbedContent, TabPane)
    from textual.widgets.option_list import Option

    # ------------------------------------------------------------------ modal dialogs
    class ValueScreen(ModalScreen):
        """Edit one number or text."""
        BINDINGS = [Binding("escape", "cancel", "Cancel")]
        CSS = """
        ValueScreen { align: center middle; }
        #box { width: 70; height: auto; border: round $primary; padding: 1 2; background: $surface; }
        #err { color: $error; height: 1; }
        #hint { color: $text-muted; }
        """

        def __init__(self, title, value, hint, check):
            super().__init__()
            self._title, self._value, self._hint, self._check = title, value, hint, check

        def compose(self):
            with Vertical(id="box"):
                yield Label(self._title)
                yield Input(value=self._value, id="v")
                yield Static("", id="err")
                yield Static(self._hint, id="hint")
                yield Static("Enter = ok   Esc = cancel", id="keys")

        def on_mount(self):
            inp = self.query_one("#v", Input)
            inp.focus()
            inp.select_all()                     # typing replaces the old value

        def on_input_changed(self, ev):
            self.query_one("#err", Static).update(self._check(ev.value) or "")

        def on_input_submitted(self, ev):
            err = self._check(ev.value)
            if err:
                self.query_one("#err", Static).update(err)
            else:
                self.dismiss(ev.value)

        def action_cancel(self):
            self.dismiss(None)

    class ChoiceScreen(ModalScreen):
        """Pick one of a list."""
        BINDINGS = [Binding("escape", "cancel", "Cancel")]
        CSS = """
        ChoiceScreen { align: center middle; }
        #box { width: 70; height: auto; max-height: 20; border: round $primary; padding: 1 2; background: $surface; }
        """

        def __init__(self, title, options, current):
            super().__init__()
            self._title, self._options, self._current = title, options, current

        def compose(self):
            with Vertical(id="box"):
                yield Label(self._title)
                yield OptionList(*[Option(("(*) " if v == self._current else "( ) ") + lab, id=str(i))
                                   for i, (lab, v) in enumerate(self._options)], id="ol")
                yield Static("Enter = select   Esc = cancel")

        def on_mount(self):
            ol = self.query_one("#ol", OptionList)
            ol.focus()
            for i, (_, v) in enumerate(self._options):
                if v == self._current:
                    ol.highlighted = i

        def on_option_list_option_selected(self, ev):
            self.dismiss(self._options[int(ev.option.id)][1])

        def action_cancel(self):
            self.dismiss(None)

    class ConfirmScreen(ModalScreen):
        BINDINGS = [Binding("escape", "no", "No"), Binding("n", "no", "No"), Binding("y", "yes", "Yes")]
        CSS = """
        ConfirmScreen { align: center middle; }
        #box { width: 70; height: auto; border: round $warning; padding: 1 2; background: $surface; }
        """

        def __init__(self, text):
            super().__init__()
            self._text = text

        def compose(self):
            with Vertical(id="box"):
                yield Static(self._text)
                yield Static("\ny = yes    n / Esc = no")

        def action_yes(self):
            self.dismiss(True)

        def action_no(self):
            self.dismiss(False)

    class HelpScreen(ModalScreen):
        BINDINGS = [Binding("escape", "close", "Close"), Binding("enter", "close", "Close"), Binding("question_mark", "close", "Close")]
        CSS = """
        HelpScreen { align: center middle; }
        #box { width: 78; height: auto; max-height: 24; border: round $primary; padding: 1 2; background: $surface; }
        """

        def __init__(self, title, text):
            super().__init__()
            self._title, self._text = title, text

        def compose(self):
            with Vertical(id="box"):
                yield Label(self._title)
                yield Static("\n" + self._text + "\n\nEsc / Enter = close")

        def action_close(self):
            self.dismiss(None)

    # ------------------------------------------------------------------ the app
    class S2S2App(App):
        TITLE = "S2S2 configuration"
        SUB_TITLE = "satellite + ground station"
        CSS = """
        #crumb { height: 1; padding: 0 1; text-style: bold; color: $accent; }
        #menu { height: 1fr; border: round $primary; }
        #help { height: 5; border: round $primary; padding: 0 1; color: $text-muted; }
        #cfgstatus { height: 1; padding: 0 1; color: $text-muted; }
        .mem { border: round $primary; padding: 0 1; margin-bottom: 1; height: auto; }
        .mem .title { text-style: bold; color: $accent; }
        .memline { margin-top: 1; }
        .crit Bar > .bar--bar { color: $error; }
        .warn Bar > .bar--bar { color: $warning; }
        #log { height: 1fr; border: round $primary; }
        #status { height: 1; color: $text-muted; }
        """
        BINDINGS = [
            Binding("escape,left,backspace", "back", "Back", show=False),
            Binding("space", "toggle", "Toggle", show=False),
            Binding("y", "set_bool(True)", "Yes", show=False),
            Binding("n", "set_bool(False)", "No", show=False),
            Binding("question_mark", "item_help", "Help"),
            Binding("s", "save", "Save"),
            Binding("b", "build", "Build"),
            Binding("f", "flash", "Flash"),
            Binding("a", "flash_all", "Flash all"),
            Binding("p", "rescan", "Probes"),
            Binding("c", "tab('tab_cfg')", "Menu"),
            Binding("m", "tab('tab_mem')", "Memory"),
            Binding("o", "tab('tab_out')", "Output"),
            Binding("r", "refresh_stats", "Refresh", show=False),
            Binding("q", "quit_app", "Quit"),
        ]

        def __init__(self):
            super().__init__()
            self.cfg = C.load_config()
            self.probes = []
            self.busy = False
            self.stack = ["main"]
            self.sel = {}                      # menu id -> highlighted index
            self.items = []                    # the visible items of the current menu
            self.status_text = ""

        # ------------------------------------------------------------ layout
        def compose(self) -> ComposeResult:
            yield Header()
            with TabbedContent(initial="tab_cfg", id="tabs"):
                with TabPane("Menu", id="tab_cfg"):
                    yield Static("", id="crumb")
                    yield OptionList(id="menu")
                    yield Static("", id="help")
                    yield Static("", id="cfgstatus")
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
            self.regenerate()                      # creates generated/secrets.h the first time
            self.render_menu()
            self.query_one("#menu", OptionList).focus()
            await self.rescan_probes()
            self.render_menu()
            await self.refresh_memory()

        # ------------------------------------------------------------ helpers
        def tabs(self) -> TabbedContent:
            return self.query_one("#tabs", TabbedContent)

        def log_widget(self) -> RichLog:
            return self.query_one("#log", RichLog)

        def set_status(self, text):
            self.query_one("#status", Static).update(text)

        def say(self, text):
            self.status_text = text
            self.query_one("#cfgstatus", Static).update(text)

        def persist(self):
            C.save_config(self.cfg)
            changed = self.regenerate()
            self.say("saved .s2s2.config" + ("; generated " + ", ".join(os.path.basename(x) for x in changed) if changed else ""))

        def regenerate(self, new_key=False):
            """Write generated/{secrets.h, s2s2_config.h, s2s2.mk}; returns the files that changed ([] on a problem)."""
            try:
                return C.generate(self.cfg, new_key=new_key)
            except ValueError as e:
                self.say("NOT generated: " + str(e))
                return []

        def key_text(self):
            k = C.read_key()
            return "key %s (generated/secrets.h, not in git; both boards need the same one)" % (C.key_fingerprint(k) if k else "?")

        # ------------------------------------------------------------ the menu
        def current(self):
            return self.stack[-1]

        def render_menu(self):
            title, items = C.MENUS[self.current()]
            self.items = [it for it in items if C.item_visible(it, self.cfg)]
            ol = self.query_one("#menu", OptionList)
            ol.clear_options()
            opts = []
            for i, it in enumerate(self.items):
                opts.append(Option(self.line(it), id=str(i)))
            ol.add_options(opts)
            idx = min(self.sel.get(self.current(), 0), max(0, len(self.items) - 1))
            if self.items:
                ol.highlighted = idx
            crumb = " > ".join(C.MENUS[m][0] for m in self.stack)
            self.query_one("#crumb", Static).update(crumb)
            self.update_help()

        def line(self, it):
            k, key, lab = it["kind"], it["key"], it["label"]
            v = self.cfg.get(key)
            if k == "menu":
                return Text("    %s  --->" % lab)
            if k == "bool":
                return Text("[%s] %s" % ("*" if v else " ", lab))
            if k in ("int", "str", "choice"):
                return Text("(%s) %s" % (v if v != "" else "-", lab) + ("  --->" if k == "choice" else ""))
            if k == "probe":
                return Text("(%s) %s  --->" % (v or "none", lab))
            if k == "info":
                return Text("    " + (self.key_text() if key == "_key" else lab))
            return Text("    " + lab)                # action

        def selected(self):
            ol = self.query_one("#menu", OptionList)
            i = ol.highlighted
            return (i, self.items[i]) if i is not None and 0 <= i < len(self.items) else (None, None)

        def update_help(self):
            _, it = self.selected()
            self.query_one("#help", Static).update(it["help"] if it and it["help"] else "")

        def on_option_list_option_highlighted(self, ev):
            if ev.option_list.id == "menu":
                if ev.option_index is not None:
                    self.sel[self.current()] = ev.option_index
                self.update_help()

        def on_option_list_option_selected(self, ev):
            if ev.option_list.id == "menu":
                self.run_worker(self.activate(), exclusive=True, group="dialog")      # dialogs wait for an answer: must run in a worker

        async def activate(self):
            i, it = self.selected()
            if it is None:
                return
            k, key = it["kind"], it["key"]
            if k == "menu":
                self.stack.append(key)
                self.render_menu()
            elif k == "bool":
                self.set_value(key, not self.cfg[key])
            elif k in ("int", "str"):
                await self.edit_value(it)
            elif k == "choice":
                lab_opts = [(o, o) for o in it["options"]]
                v = await self.push_screen_wait(ChoiceScreen(it["label"], lab_opts, self.cfg[key]))
                if v is not None:
                    self.set_value(key, v)
            elif k == "probe":
                opts = [("none", "")] + [("%s   %s   %s" % (p["serial"], p["version"], p["chip"]), p["serial"]) for p in self.probes]
                if self.cfg[key] and all(self.cfg[key] != v for _, v in opts):
                    opts.append(("%s   (not connected)" % self.cfg[key], self.cfg[key]))
                v = await self.push_screen_wait(ChoiceScreen(it["label"], opts, self.cfg[key]))
                if v is not None:
                    self.set_value(key, v)
            elif k == "action":
                await self.do_item_action(key)

        def set_value(self, key, value):
            trial = dict(self.cfg, **{key: value})
            errs = C.validate_config(trial)
            if errs:
                self.say("not changed: " + "; ".join(errs))
                self.notify("; ".join(errs), severity="error")
                return False
            self.cfg = trial
            self.persist()
            self.render_menu()
            return True

        async def edit_value(self, it):
            key, kind = it["key"], it["kind"]

            def check(text):
                try:
                    val = int(text) if kind == "int" else (text.strip().upper() if it.get("upper") else text.strip())
                except ValueError:
                    return "must be a whole number"
                if kind == "int" and not (it["lo"] <= val <= it["hi"]):
                    return "must be %d to %d" % (it["lo"], it["hi"])
                errs = C.validate_config(dict(self.cfg, **{key: val}))
                return errs[0] if errs else ""

            raw = await self.push_screen_wait(ValueScreen(it["label"], str(self.cfg[key]), it["help"], check))
            if raw is not None:
                self.set_value(key, int(raw) if kind == "int" else (raw.strip().upper() if it.get("upper") else raw.strip()))

        async def do_item_action(self, key):
            if key == "newkey":
                if await self.push_screen_wait(ConfirmScreen("Replace the AUTH key?\n\nBoth boards must be rebuilt and reflashed afterwards,\nolder firmware can no longer talk to the new one.")):
                    self.regenerate(new_key=True)
                    self.say("new AUTH key " + self.key_text())
                    self.render_menu()
            elif key == "rescan":
                await self.action_rescan()
            elif key == "build":
                self.action_build()
            elif key == "flash":
                self.action_flash()
            elif key == "flash_all":
                self.action_flash_all()
            elif key == "clean":
                self.action_clean()
            elif key == "memory":
                self.show_tab("tab_mem")
            elif key == "gen":
                changed = self.regenerate()
                self.say("generated: " + (", ".join(changed) if changed else "up to date"))
            elif key == "defconfig":
                if await self.push_screen_wait(ConfirmScreen("Reset all options to the defaults?\n(the ST-Link choice is kept)")):
                    keep = {k: self.cfg[k] for k in ("sat_serial", "gs_serial")}
                    self.cfg = dict(C.DEFAULT_CONFIG, **keep)
                    self.persist()
                    self.render_menu()

        # ------------------------------------------------------------ keys
        def action_back(self):
            if self.tabs().active != "tab_cfg":
                self.show_tab("tab_cfg")
            elif len(self.stack) > 1:
                self.stack.pop()
                self.render_menu()

        def action_toggle(self):
            _, it = self.selected()
            if it and it["kind"] == "bool" and self.tabs().active == "tab_cfg":
                self.set_value(it["key"], not self.cfg[it["key"]])

        def action_set_bool(self, value: bool):
            _, it = self.selected()
            if it and it["kind"] == "bool" and self.tabs().active == "tab_cfg":
                self.set_value(it["key"], value)

        def action_item_help(self):
            _, it = self.selected()
            if it:
                self.run_worker(self.push_screen_wait(HelpScreen(it["label"], it["help"] or "(no help text)")), exclusive=True, group="dialog")

        def action_save(self):
            self.persist()
            self.notify("saved " + os.path.basename(C.CONFIG_PATH) + " and generated/")

        def show_tab(self, name: str):
            self.set_focus(None)
            self.tabs().active = name
            if name == "tab_cfg":
                self.query_one("#menu", OptionList).focus()

        def action_tab(self, name: str):
            self.show_tab(name)

        async def action_rescan(self):
            await self.rescan_probes()
            self.render_menu()
            self.notify("%d ST-Link probe(s) found" % len(self.probes))

        async def action_refresh_stats(self):
            await self.refresh_memory()
            self.notify("Memory statistics refreshed")

        def action_quit_app(self):
            self.persist()
            self.exit()

        # ------------------------------------------------------------ probes
        async def rescan_probes(self):
            self.probes = await asyncio.to_thread(C.scan_probes)
            self.say("%d ST-Link probe(s) connected" % len(self.probes))

        # ------------------------------------------------------------ memory tab
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

        # ------------------------------------------------------------ build / flash
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
                self.notify("; ".join(problems) + "  (Menu > Programmer)", severity="error")
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
                self.notify("Nothing to do: no target selected (Menu > Targets)", severity="warning")
                return
            self.busy = True
            self.run_worker(self.run_steps(title, steps), exclusive=False)

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
            self.busy = False
            self.notify(("Done: " if ok else "Failed: ") + title, severity="information" if ok else "error")

    return S2S2App


# ---------------------------------------------------------------------------------------------------- batch modes
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


def run_interactive():
    """Start the menuconfig; on any problem say why, write the details to .s2s2_tui_error.log and return 2."""
    log = os.path.join(C.REPO, ".s2s2_tui_error.log")
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("menuconfig needs a terminal. Without one use: make configure | build | flash | memstat | probes")
        return 2
    cols, rows = shutil.get_terminal_size((80, 24))
    if cols < MIN_COLS or rows < MIN_ROWS:
        print("The terminal is too small for the menu (%dx%d, need at least %dx%d): make the window bigger and run `make` again." %
              (cols, rows, MIN_COLS, MIN_ROWS))
        return 2
    try:
        app = build_app()()
    except ImportError as e:
        print("Textual is not available (%s).\nRun: python3 -m venv tools/.venv && tools/.venv/bin/pip install -r tools/requirements.txt\n"
              "or use the batch targets: make configure | build | flash | memstat" % e)
        return 2
    try:
        app.run()
    except Exception:
        with open(log, "w") as f:
            traceback.print_exc(file=f)
        print("The menu stopped with an error. Details: %s\nYou can still use: make configure | defconfig | build | flash | memstat" % log)
        print(traceback.format_exc().strip().splitlines()[-1])
        return 1
    return 0


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
        # make flash sat CUBUS=T  ->  --batch flash --only sat --cubus T   (the Makefile passes these through)
        def opt(name):
            return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else None
        only = opt("--only").split() if opt("--only") else None
        try:
            cubus = C.parse_bool(opt("--cubus")) if opt("--cubus") else None
        except ValueError as e:
            print("--cubus: %s" % e)
            return 2
        return C.batch(argv[argv.index("--batch") + 1], only=only, cubus=cubus)
    return run_interactive()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
