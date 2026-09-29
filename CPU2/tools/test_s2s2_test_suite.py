#!/usr/bin/env python3
"""
test_s2s2_test_suite.py -- unit tests for s2s2_test_suite.py, the unified
web bench tool (CPU2/tools/s2s2_test_suite.py). Covers the pieces that
are meaningfully unit-testable without real hardware: the activity-
monitor log-line regexes, SerialLink's line-handling/counter logic,
TestBattery's step sequencing, the FastAPI endpoints' validation, the
real static memory-profile reader, and -- the one with actual teeth --
a regression test pinning gmsk_demod.py's stale BAUD=9600/FDEV_HZ=2400
module defaults against the firmware's real 4800bps/1200Hz operating
point (see config.h's RADIO_BIT_RATE_BPS/RADIO_FDEV_HZ), so nobody
reintroduces the "demod baud doesn't match the firmware" bug this
session found and worked around by always passing baud= explicitly.

Run: python3 -m pytest test_s2s2_test_suite.py -v
     (or: python3 -m pytest test_s2s2_test_suite.py -v -k "not slow"
      to skip the two frame-modulation regression tests, each of which
      compiles a small C program and runs a GMSK modulate/demod pass)
"""
import os
import sys
import time

import pytest
from fastapi.testclient import TestClient

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import s2s2_test_suite as sts
import gmsk_demod as gd
import s2s2_decoder as dec
import test_demod  # reuses build_reference_frame()'s real-firmware C harness


# =====================================================================
# Activity-monitor regex patterns -- exact log-line text the firmware
# prints (ipcc_m0plus.c: Send_Cw_Beacon/Send_Beacon1/Send_Beacon2/
# Send_Beacon_If_Due/Try_Digipeat/Cycle_Tick).
# =====================================================================

def test_cycle_phase_regex_matches_all_four_new_names():
    for phase in ("CW1", "MISSION1", "CW2", "MISSION2"):
        m = sts._RE_CYCLE_PHASE.search(f"CYCLE PHASE: {phase}")
        assert m is not None and m.group(1) == phase


def test_cycle_phase_regex_rejects_old_pre_rename_names():
    # This session renamed the cycle from CW/Beacon1/Mission/Beacon2 to
    # CW1/Mission1/CW2/Mission2 -- the regex must NOT still match the
    # old names (that was the actual bug fixed in mission_control_api.py
    # and satellite_gui.py this session).
    for stale in ("BEACON1", "BEACON2", "MISSION", "CW"):
        assert sts._RE_CYCLE_PHASE.search(f"CYCLE PHASE: {stale}") is None


def test_cw_tx_regex():
    assert sts._RE_CW_TX.search('CW BEACON TX: "DE NEPSAT 00000"')
    assert not sts._RE_CW_TX.search("BEACON1 TX (cycle): day=0 hour=0")


def test_beacon1_tx_regex():
    assert sts._RE_BEACON1_TX.search("BEACON1 TX (cycle): day=0 hour=0")
    assert not sts._RE_BEACON1_TX.search("BEACON2 TX (cycle)")


def test_beacon2_tx_regex():
    assert sts._RE_BEACON2_TX.search("BEACON2 TX (cycle)")
    assert not sts._RE_BEACON2_TX.search("BEACON1 TX (cycle): day=0 hour=0")


def test_text_ping_regex():
    assert sts._RE_TEXT_PING_TX.search("BEACON TX (5s tick)")
    assert not sts._RE_TEXT_PING_TX.search("BEACON1 TX (cycle)")


def test_digi_relayed_regex_extracts_groups():
    line = "DIGI: relayed via hop 2 (GROUND-3), 42 info byte(s), rs_corrected=5"
    m = sts._RE_DIGI_RELAYED.search(line)
    assert m is not None
    assert m.groups() == ("2", "GROUND-3", "42", "5")


def test_digi_duplicate_regex():
    assert sts._RE_DIGI_DUP.search("DIGI: duplicate, not re-relaying")


def test_digi_failed_regex():
    assert sts._RE_DIGI_FAILED.search("DIGI: relay TX failed")


# =====================================================================
# SerialLink -- line handling and activity counters. _handle_line()
# touches no serial hardware, so it can be exercised directly.
# =====================================================================

@pytest.fixture
def link_with_events():
    events = []
    link = sts.SerialLink("satellite", events.append)
    link.connected_at = time.time()
    return link, events


def test_handle_line_ignores_blank_lines(link_with_events):
    link, events = link_with_events
    link._handle_line("   \r\n")
    assert events == []
    assert link.lines_total == 0


def test_handle_line_cw_tx_increments_counter_and_emits_activity(link_with_events):
    link, events = link_with_events
    link._handle_line('CW BEACON TX: "DE NEPSAT 00000"')
    assert link.cw_tx_count == 1
    kinds = [e["kind"] for e in events]
    assert "RAW" in kinds and "ACTIVITY" in kinds
    activity = next(e for e in events if e["kind"] == "ACTIVITY")
    assert activity["cw_tx_count"] == 1


def test_handle_line_beacon1_and_beacon2_counters_independent(link_with_events):
    link, _ = link_with_events
    link._handle_line("BEACON1 TX (cycle): day=0 hour=0")
    link._handle_line("BEACON1 TX (cycle): day=0 hour=1")
    link._handle_line("BEACON2 TX (cycle)")
    assert link.beacon1_tx_count == 2
    assert link.beacon2_tx_count == 1


def test_handle_line_cycle_phase_updates_link_state(link_with_events):
    link, events = link_with_events
    link._handle_line("CYCLE PHASE: MISSION1")
    assert link.cycle_phase == "MISSION1"
    phase_events = [e for e in events if e["kind"] == "CYCLE_PHASE"]
    assert len(phase_events) == 1 and phase_events[0]["phase"] == "MISSION1"


def test_handle_line_digi_relayed_increments_counter(link_with_events):
    link, _ = link_with_events
    link._handle_line("DIGI: relayed via hop 1 (SAT-1), 10 info byte(s), rs_corrected=0")
    assert link.digi_relayed == 1
    assert link.digi_duplicate == 0
    assert link.digi_failed == 0


def test_handle_line_unrelated_text_is_raw_only_no_activity_event(link_with_events):
    link, events = link_with_events
    link._handle_line("some unrelated debug print")
    kinds = [e["kind"] for e in events]
    assert kinds == ["RAW"]  # no counter changed -> no ACTIVITY event emitted


def test_lines_total_and_last_line_ts_track_every_line(link_with_events):
    link, _ = link_with_events
    link._handle_line("line one")
    link._handle_line("line two")
    assert link.lines_total == 2
    assert link.last_line_ts is not None


def test_activity_snapshot_shape_and_uptime(link_with_events):
    link, _ = link_with_events
    link._handle_line("CW BEACON TX: test")
    snap = link.activity_snapshot()
    for key in ("connected_at", "uptime_s", "lines_total", "cycle_phase",
                "cw_tx_count", "beacon1_tx_count", "beacon2_tx_count",
                "digi_relayed", "digi_duplicate", "digi_failed"):
        assert key in snap
    assert snap["uptime_s"] >= 0
    assert snap["cw_tx_count"] == 1


def test_ground_role_link_feeds_gs_parser_not_activity_counters(link_with_events):
    link = sts.SerialLink("ground", lambda e: None)
    # Ground role has no cw/beacon counters wired to firmware text that
    # only the satellite prints -- CW_TX regex simply won't match
    # anything a ground station's CLI would print, counters stay zero.
    link._handle_line("some ground CLI response line")
    assert link.cw_tx_count == 0
    assert link._gs_parser is not None


# =====================================================================
# TestBattery -- continuous step sequencing against a fake link.
# =====================================================================

class FakeLink:
    def __init__(self, fail_on_step=None):
        self.sent = []
        self.connected = True
        self.fail_on_step = fail_on_step

    def send_line(self, text):
        if self.fail_on_step is not None and len(self.sent) == self.fail_on_step:
            raise RuntimeError("simulated serial write failure")
        self.sent.append(text)


def test_battery_runs_steps_in_declared_order():
    link = FakeLink()
    battery = sts.TestBattery(link, lambda e: None)
    battery.STEPS = [("TESTCW", 0.01), ("TESTBEACON1", 0.01)]
    battery.start()
    for _ in range(200):
        if not battery.running and link.sent:
            break
        time.sleep(0.01)
    battery.stop()
    battery._thread.join(timeout=2.0)
    assert link.sent[:2] == ["TESTCW", "TESTBEACON1"]


def test_battery_stop_halts_it():
    link = FakeLink()
    battery = sts.TestBattery(link, lambda e: None)
    battery.STEPS = [("TESTCW", 0.05)] * 50  # long enough to still be running
    battery.start()
    time.sleep(0.05)
    assert battery.running
    battery.stop()
    battery._thread.join(timeout=2.0)
    assert not battery.running


def test_battery_counts_cycles_across_multiple_passes():
    link = FakeLink()
    battery = sts.TestBattery(link, lambda e: None)
    battery.STEPS = [("TESTCW", 0.0)]
    battery.start()
    for _ in range(200):
        if battery.cycle_count >= 3:
            break
        time.sleep(0.01)
    battery.stop()
    battery._thread.join(timeout=2.0)
    assert battery.cycle_count >= 3


def test_battery_stops_itself_on_send_failure():
    link = FakeLink(fail_on_step=0)
    events = []
    battery = sts.TestBattery(link, events.append)
    battery.STEPS = [("TESTCW", 0.01)]
    battery.start()
    battery._thread.join(timeout=2.0)
    assert not battery.running
    step_events = [e for e in events if e["kind"] == "BATTERY_STEP"]
    assert step_events and step_events[0]["ok"] is False


def test_battery_double_start_is_a_noop():
    link = FakeLink()
    battery = sts.TestBattery(link, lambda e: None)
    battery.STEPS = [("TESTCW", 0.05)] * 20
    battery.start()
    first_thread = battery._thread
    battery.start()  # already running -- must not spawn a second thread
    assert battery._thread is first_thread
    battery.stop()
    battery._thread.join(timeout=2.0)


# =====================================================================
# FastAPI endpoints -- validation logic, no hardware required.
# =====================================================================

@pytest.fixture
def client():
    with TestClient(sts.app) as c:
        yield c


def test_role_endpoint_sets_satellite_role(client):
    r = client.post("/api/pane/a/role", json={"role": "satellite"})
    assert r.status_code == 200
    body = r.json()
    assert body["ok"] is True
    assert body["status"]["role"] == "satellite"
    assert body["status"]["connected"] is False


def test_role_endpoint_rejects_unknown_role(client):
    r = client.post("/api/pane/a/role", json={"role": "not-a-real-role"})
    assert r.json()["ok"] is False


def test_role_none_tears_down_pane(client):
    client.post("/api/pane/a/role", json={"role": "sdr"})
    r = client.post("/api/pane/a/role", json={"role": "none"})
    assert r.json()["status"]["role"] is None


def test_send_message_rejects_odd_length_hex(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    r = client.post("/api/pane/a/send_message", json={"hex": "ABC"})
    assert r.json()["ok"] is False


def test_send_message_rejects_non_hex_characters(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    r = client.post("/api/pane/a/send_message", json={"hex": "ZZZZ"})
    assert r.json()["ok"] is False


def test_send_message_valid_hex_but_not_connected(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    r = client.post("/api/pane/a/send_message", json={"hex": "48656C6C6F"})
    body = r.json()
    # Hex passes validation; fails only because nothing is connected.
    assert body["ok"] is False
    assert "not connected" in body["error"]


def test_tx_power_rejects_out_of_range(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    r = client.post("/api/pane/a/tx_power", json={"dbm": 99})
    assert r.json()["ok"] is False


def test_tx_power_in_range_but_not_connected(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    r = client.post("/api/pane/a/tx_power", json={"dbm": 10})
    body = r.json()
    assert body["ok"] is False
    assert "not connected" in body["error"]


def test_wrong_role_rejects_satellite_only_commands(client):
    client.post("/api/pane/a/role", json={"role": "sdr"})
    r = client.post("/api/pane/a/cw")
    assert r.json()["ok"] is False


def test_gs_commands_endpoint_matches_known_keyword_set(client):
    r = client.get("/api/gs_commands")
    cmds = r.json()
    for kw in ("COMMAND", "HEARTBEAT", "HEALTH", "HOUSEKEEPING", "ABORT", "DIGIPEATER"):
        assert kw in cmds


def test_ports_endpoint_returns_a_list(client):
    r = client.get("/api/ports")
    assert isinstance(r.json(), list)


def test_pane_status_defaults_to_no_role(client):
    r = client.get("/api/pane/b/status")
    assert r.json()["role"] is None


def test_disconnect_when_never_connected_is_safe(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    r = client.post("/api/pane/a/disconnect")
    assert r.json()["ok"] is True


def test_pane_activity_endpoint_on_non_serial_role(client):
    client.post("/api/pane/a/role", json={"role": "sdr"})
    r = client.get("/api/pane/a/activity")
    assert r.json()["ok"] is False


def test_two_panes_are_independent(client):
    client.post("/api/pane/a/role", json={"role": "satellite"})
    client.post("/api/pane/b/role", json={"role": "sdr"})
    a = client.get("/api/pane/a/status").json()
    b = client.get("/api/pane/b/status").json()
    assert a["role"] == "satellite"
    assert b["role"] == "sdr"


# =====================================================================
# Memory profile -- real numbers off the built .elf.
# =====================================================================

def test_memory_profile_missing_elf_reports_error(monkeypatch):
    monkeypatch.setattr(sts, "ELF_PATH", "/nonexistent/path/does_not_exist.elf")
    result = sts.get_memory_profile()
    assert result["ok"] is False
    assert "not found" in result["error"]


@pytest.mark.skipif(not os.path.isfile(sts.ELF_PATH), reason="ipcc_m0plus.elf not built yet (make sat)")
def test_memory_profile_real_elf_has_plausible_numbers():
    result = sts.get_memory_profile()
    assert result["ok"] is True
    assert 0 < result["flash_used"] < result["flash_total"]
    assert 0 < result["ram_used"] < result["ram_total"]
    # No symbol should ever exceed total addressable flash+RAM -- guards
    # against the _estack wraparound-size bug this same test file's
    # get_memory_profile() filters out.
    for sym in result["top_symbols"]:
        assert sym["size"] <= result["flash_total"] + result["ram_total"]


@pytest.mark.skipif(not os.path.isfile(sts.ELF_PATH), reason="ipcc_m0plus.elf not built yet (make sat)")
def test_memory_profile_endpoint_matches_direct_call(client):
    r = client.get("/api/memory_profile")
    body = r.json()
    assert body["ok"] is True
    assert body["flash_total"] == sts.FLASH_TOTAL_BYTES
    assert body["ram_total"] == sts.RAM_TOTAL_BYTES


# =====================================================================
# gmsk_demod baud/fdev regression -- the actual bug this session found:
# gmsk_demod.py's module-level BAUD=9600/FDEV_HZ=2400 defaults are stale
# against the firmware's real 4800bps/1200Hz (config.h's
# RADIO_BIT_RATE_BPS/RADIO_FDEV_HZ). Anything that calls
# extract_candidate_frames()/gmsk_modulate() without an explicit baud=
# silently demodulates at the wrong rate. Marked slow: each compiles a
# small C program against the real firmware RS/AX.25/FX.25 sources.
# =====================================================================

@pytest.mark.slow
def test_correct_baud_decodes_real_firmware_4800bps_signal():
    frame = test_demod.build_reference_frame()
    sample_rate = 240000.0
    iq = gd.gmsk_modulate(frame, sample_rate=sample_rate, baud=4800, fdev=1200,
                           order="msb", invert=False)
    candidates = gd.extract_candidate_frames(iq, sample_rate, baud=4800)
    assert any(dec.fx25_decode_frame(c["frame"]) is not None for c in candidates), (
        "extract_candidate_frames(baud=4800) failed to decode a signal modulated "
        "at the firmware's real 4800bps operating point"
    )


@pytest.mark.slow
def test_stale_module_default_baud_fails_on_real_firmware_signal():
    """Pins the actual footgun: calling extract_candidate_frames() with
    no baud= argument silently uses gmsk_demod.BAUD (9600), which does
    NOT match the real firmware's 4800bps. If this test ever starts
    failing because the module defaults were finally corrected to 4800,
    delete it -- that would mean the footgun is gone, not that this
    test is wrong."""
    frame = test_demod.build_reference_frame()
    sample_rate = 240000.0
    iq = gd.gmsk_modulate(frame, sample_rate=sample_rate, baud=4800, fdev=1200,
                           order="msb", invert=False)
    assert gd.BAUD == 9600, "module default BAUD changed -- re-check this test's premise"
    wrong_baud_candidates = gd.extract_candidate_frames(iq, sample_rate)  # no baud= -> stale 9600 default
    assert not any(dec.fx25_decode_frame(c["frame"]) is not None for c in wrong_baud_candidates), (
        "a 4800bps signal decoded successfully at the stale default baud=9600 -- "
        "either this regression test's assumption is wrong, or extract_candidate_frames "
        "got lucky; investigate before trusting baud-less calls elsewhere"
    )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
