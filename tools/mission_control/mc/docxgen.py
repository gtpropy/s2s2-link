"""mc.docxgen -- minimal .docx writer (standard library only) for the operational report template.

blocks: ("title", text) | ("subtitle", text) | ("h", text) | ("p", text) | ("kv", [(key, value), ...]) | ("table", header_row, rows)
Empty values become empty cells that the operator fills in by hand.
"""
import zipfile
from xml.sax.saxutils import escape


def _run(text, bold=False, size=None, color=None):
    pr = ""
    if bold:
        pr += "<w:b/>"
    if color:
        pr += f'<w:color w:val="{color}"/>'
    if size:
        pr += f'<w:sz w:val="{size}"/>'
    return f'<w:r><w:rPr>{pr}</w:rPr><w:t xml:space="preserve">{escape(str(text))}</w:t></w:r>'


def _p(text, bold=False, size=None, align=None, color=None, space_after=120):
    al = f'<w:jc w:val="{align}"/>' if align else ""
    return f'<w:p><w:pPr><w:spacing w:after="{space_after}"/>{al}</w:pPr>{_run(text, bold, size, color)}</w:p>'


def _cell(text, w, bold=False, shade=None):
    sh = f'<w:shd w:val="clear" w:color="auto" w:fill="{shade}"/>' if shade else ""
    return f'<w:tc><w:tcPr><w:tcW w:w="{w}" w:type="dxa"/>{sh}</w:tcPr>{_p(text, bold=bold, space_after=40)}</w:tc>'


def _table(rows, widths, header=True):
    b = "".join(f'<w:{s} w:val="single" w:sz="4" w:space="0" w:color="8C96AA"/>' for s in ("top", "left", "bottom", "right", "insideH", "insideV"))
    x = [f'<w:tbl><w:tblPr><w:tblW w:w="{sum(widths)}" w:type="dxa"/><w:tblBorders>{b}</w:tblBorders><w:tblLayout w:type="fixed"/></w:tblPr>']
    x.append("<w:tblGrid>" + "".join(f'<w:gridCol w:w="{w}"/>' for w in widths) + "</w:tblGrid>")
    for i, r in enumerate(rows):
        head = header and i == 0
        x.append("<w:tr>" + "".join(_cell(v, widths[j], bold=head, shade="DCE6F4" if head else None) for j, v in enumerate(r)) + "</w:tr>")
    x.append("</w:tbl>")
    return "".join(x) + _p("", space_after=80)


def write_docx(path, blocks):
    body = []
    for b in blocks:
        k = b[0]
        if k == "title":
            body.append(_p(b[1], bold=True, size=40, align="center", space_after=60))
        elif k == "subtitle":
            body.append(_p(b[1], size=24, align="center", color="555555", space_after=60))
        elif k == "h":
            body.append(_p(b[1], bold=True, size=28, color="1F3A6E", space_after=100))
        elif k == "p":
            body.append(_p(b[1]))
        elif k == "kv":
            body.append(_table([[kk, vv] for kk, vv in b[1]], [3600, 5760], header=False))
        elif k == "table":
            ncol = len(b[1])
            w = [max(700, 9360 // ncol)] * ncol
            body.append(_table([b[1]] + [[("" if v is None else v) for v in r] for r in b[2]], w))
    doc = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>'
           + "".join(body) + '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1000" w:right="1200" w:bottom="1000" w:left="1200" w:header="708" w:footer="708" w:gutter="0"/></w:sectPr></w:body></w:document>')
    ct = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/>'
          '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", ct)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", doc)


def docx_text(path):
    """Plain text of a .docx (used by the report viewer and the tests)."""
    import re
    z = zipfile.ZipFile(path)
    x = z.read("word/document.xml").decode("utf-8", "replace")
    x = re.sub(r"</w:p>", "\n", x)
    x = re.sub(r"</w:tc>", " | ", x)
    x = re.sub(r"<[^>]+>", "", x)
    return re.sub(r"\n{3,}", "\n\n", x).replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
