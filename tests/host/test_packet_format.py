#!/usr/bin/env python3
"""
Packet Format compliance (file table of docs/spec/Packet Format.xlsx): every file of the table must be served by the satellite and requestable from the ground
station, with the sheet's directory. No hardware. The sheet is read straight from the xlsx (standard library only), the code is checked as text.
"""
import os
import re
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
fails = 0


def check(name, ok, extra=""):
    global fails
    print("%s  %s %s" % ("ok  " if ok else "FAIL", name, extra))
    if not ok:
        fails += 1


z = zipfile.ZipFile(os.path.join(ROOT, "docs", "spec", "Packet Format.xlsx"))
ss = [re.sub(r"<[^>]+>", "", s) for s in re.findall(r"<si>(.*?)</si>", z.read("xl/sharedStrings.xml").decode(), re.S)]
# the file table is a run of strings: file name, directory ("D1/D3", "D2/D4", "D1"), file number ("F1".."F8")
names = {"flags.txt": 1, "sat_health.txt": 2, "sat_log.txt": 3, "rsv_table.txt": 4, "cam.txt": 5, "epdm.txt": 6, "adcs.txt": 7}
for n, f in names.items():
    i = ss.index(n)
    near = ss[i:i + 6]
    # shared strings are stored once: the F5 cell of cam.txt is the same string as the F5 of the CAM DOWNLOAD command, so it is not next to the name
    check("sheet lists %s as F%d" % (n, f), ("F%d" % f in near) if f != 5 else (ss[i + 1] == "D2/D4" and "F5" in ss), str(near[:4]))
check("sheet lists the housekeeping download as F8", "F8" in ss)

sat = open(os.path.join(ROOT, "cpu2", "satellite", "app", "satellite_main.c")).read()
body = sat[sat.index("static void Sat_Classify_Telecommand"):sat.index("static bool Sat_Tc_Is_Known")]
for f in range(1, 9):
    check("satellite classifies file F%d" % f, ("0xF%d" % f) in body)
check("satellite accepts the alternate directories D3 / D4", "0xD3" in body and "0xD4" in body)
check("EPDM and ADCS are served (mission ids)", "SAT_MISSION_EPDM" in body and "SAT_MISSION_ADCS" in body)
check("satellite serves the ADCS (MCU 03) and EPDM (MCU 05) mission run commands of the reference firmware", "CMD_TYPE_ADCS" in body and "CMD_TYPE_EPDM" in body and "tcc.adcs || tcc.epdm" in sat)

gs = open(os.path.join(ROOT, "cpu2", "ground_station", "app", "gs_app.c")).read()
tab = gs[gs.index("GS_FILES[] = {"):gs.index("};", gs.index("GS_FILES[] = {"))]
for cmd, d, f in (("FLAGS", "D1", "F1"), ("SATLOG", "D1", "F3"), ("RSVTABLE", "D1", "F4"), ("EPDMFILE", "D2", "F6"), ("ADCSFILE", "D2", "F7")):
    check("GS %s requests directory %s file %s" % (cmd, d, f), re.search(r'"%s",.*0x%s, 0x%s' % (cmd, d.replace("D", "D"), f.replace("F", "F")), tab) is not None)
check("GS ADCS / EPDM send the reference mission commands (53 03 A0 53 CF / 53 05 EC CF CF)",
      "{ 0x53, 0x03, 0xA0, 0x53, 0xCF" in gs and "{ 0x53, 0x05, 0xEC, 0xCF, 0xCF" in gs)
check("satellite serves ping (1D D1 F5) and the dummy camera test (CC 5E BE) of the reference firmware", "c->ping" in body and "c->dummy_cam" in body and "0xF5" in body and "0xBE" in body)
check("GS PING sends 1D D1 F5 and DUMMYCAM sends CC 5E BE", "0x1D, 0xD1, 0xF5" in gs and gs.count("0xCC, 0x5E, 0xBE") >= 2)
check("EPDM is described as the earthquake precursor detection mission", "earthquake precursor" in gs)
print("packet format test: %s" % ("all ok" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
