"""mc.xlsxio -- minimal .xlsx writer and reader (standard library only, no openpyxl).

write_xlsx(path, [(sheet name, rows), ...], widths=None)    rows = list of lists of str / int / float / bool / None; the first row is written bold
read_xlsx(path, max_rows=1000)                              -> [{"name": ..., "rows": [[...], ...], "truncated": bool}]

Cells are written as inline strings and numbers, so there is no shared-strings table to keep in step. The reader understands both inline and shared strings, which
makes it good enough for the report viewer and for the spreadsheets the operations team already produces.
"""
import re
import zipfile
from xml.sax.saxutils import escape

_BAD = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f]")


def _col(n):
    s = ""
    n += 1
    while n:
        n, r = divmod(n - 1, 26)
        s = chr(65 + r) + s
    return s


def _cell(ref, v, style=0):
    st = f' s="{style}"' if style else ""
    if v is None or v == "":
        return f'<c r="{ref}"{st}/>' if style else ""
    if isinstance(v, bool):
        return f'<c r="{ref}"{st} t="b"><v>{int(v)}</v></c>'
    if isinstance(v, (int, float)):
        return f'<c r="{ref}"{st}><v>{v!r}</v></c>'
    t = _BAD.sub("", str(v))[:32000]
    return f'<c r="{ref}"{st} t="inlineStr"><is><t xml:space="preserve">{escape(t)}</t></is></c>'


def _sheet_xml(rows, widths):
    out = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">']
    out.append('<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/></sheetView></sheetViews>')
    ncol = max((len(r) for r in rows), default=1)
    w = list(widths or [])
    out.append("<cols>" + "".join(f'<col min="{i + 1}" max="{i + 1}" width="{(w[i] if i < len(w) and w[i] else 18)}" customWidth="1"/>' for i in range(ncol)) + "</cols>")
    out.append("<sheetData>")
    for i, r in enumerate(rows):
        cells = "".join(_cell(f"{_col(j)}{i + 1}", v, 1 if i == 0 else 0) for j, v in enumerate(r))
        out.append(f'<row r="{i + 1}">{cells}</row>')
    out.append("</sheetData></worksheet>")
    return "".join(out)


def write_xlsx(path, sheets, widths=None):
    names = []
    for n, _ in sheets:
        n = re.sub(r"[\[\]:*?/\\]", "_", n)[:31] or "Sheet"
        while n in names:
            n = (n[:28] + "_" + str(len(names)))[:31]
        names.append(n)
    ct = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
          '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'
          + "".join(f'<Override PartName="/xl/worksheets/sheet{i + 1}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>' for i in range(len(sheets)))
          + "</Types>")
    rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>')
    wb = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
          'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>'
          + "".join(f'<sheet name="{escape(n, {chr(34): "&quot;"})}" sheetId="{i + 1}" r:id="rId{i + 1}"/>' for i, n in enumerate(names)) + "</sheets></workbook>")
    wbrels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
              + "".join(f'<Relationship Id="rId{i + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet{i + 1}.xml"/>' for i in range(len(sheets)))
              + f'<Relationship Id="rId{len(sheets) + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>')
    styles = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
              '<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>'
              '<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>'
              '<fill><patternFill patternType="solid"><fgColor rgb="FFDCE6F4"/><bgColor indexed="64"/></patternFill></fill></fills>'
              '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders><cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
              '<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1"/></cellXfs></styleSheet>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rels)
        z.writestr("xl/workbook.xml", wb)
        z.writestr("xl/_rels/workbook.xml.rels", wbrels)
        z.writestr("xl/styles.xml", styles)
        for i, (_, rows) in enumerate(sheets):
            z.writestr(f"xl/worksheets/sheet{i + 1}.xml", _sheet_xml(rows, (widths or {}).get(names[i]) if isinstance(widths, dict) else widths))


def _colidx(ref):
    n = 0
    for ch in re.match(r"[A-Z]+", ref).group(0):
        n = n * 26 + ord(ch) - 64
    return n - 1


def read_xlsx(path, max_rows=1000):
    z = zipfile.ZipFile(path)
    ss = []
    if "xl/sharedStrings.xml" in z.namelist():
        for si in re.findall(r"<si>(.*?)</si>", z.read("xl/sharedStrings.xml").decode("utf-8", "replace"), re.S):
            ss.append(_unesc("".join(re.findall(r"<t[^>]*>(.*?)</t>", si, re.S))))
    wb = z.read("xl/workbook.xml").decode("utf-8", "replace")
    rels = z.read("xl/_rels/workbook.xml.rels").decode("utf-8", "replace")
    rmap = {m.group(1): m.group(2) for m in re.finditer(r'<Relationship [^>]*?Id="([^"]+)"[^>]*?Target="([^"]+)"', rels)}
    rmap.update({m.group(2): m.group(1) for m in re.finditer(r'<Relationship [^>]*?Target="([^"]+)"[^>]*?Id="([^"]+)"', rels)})
    out = []
    for m in re.finditer(r"<sheet ([^>]*?)/>", wb):
        a = m.group(1)
        name = _unesc(re.search(r'name="([^"]*)"', a).group(1))
        rid = re.search(r'r:id="([^"]+)"', a).group(1)
        tgt = rmap.get(rid, "")
        part = tgt if tgt.startswith("xl/") else "xl/" + tgt.lstrip("/")
        if part not in z.namelist():
            continue
        x = z.read(part).decode("utf-8", "replace")
        rows, trunc = [], False
        for rm in re.finditer(r"<row [^>]*?(?:/>|>(.*?)</row>)", x, re.S):
            if len(rows) >= max_rows:
                trunc = True
                break
            cells = {}
            for cm in re.finditer(r'<c r="([A-Z]+\d+)"([^>]*?)(?:/>|>(.*?)</c>)', rm.group(1) or "", re.S):
                ref, attrs, inner = cm.groups()
                inner = inner or ""
                t = re.search(r't="(\w+)"', attrs)
                t = t.group(1) if t else "n"
                if t == "inlineStr":
                    v = _unesc("".join(re.findall(r"<t[^>]*>(.*?)</t>", inner, re.S)))
                else:
                    vm = re.search(r"<v>(.*?)</v>", inner, re.S)
                    if not vm:
                        continue
                    v = vm.group(1)
                    if t == "s":
                        v = ss[int(v)] if int(v) < len(ss) else ""
                    elif t == "str":
                        v = _unesc(v)
                    elif t == "b":
                        v = bool(int(v))
                    else:
                        try:
                            f = float(v)
                            v = int(f) if f.is_integer() and "." not in v and "E" not in v.upper() else f
                        except ValueError:
                            pass
                cells[_colidx(ref)] = v
            if cells:
                row = [""] * (max(cells) + 1)
                for k, v in cells.items():
                    row[k] = v
                rows.append(row)
            else:
                rows.append([])
        while rows and not rows[-1]:
            rows.pop()
        out.append({"name": name, "rows": rows, "truncated": trunc})
    return out


def _unesc(s):
    return s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"').replace("&apos;", "'").replace("&amp;", "&")
