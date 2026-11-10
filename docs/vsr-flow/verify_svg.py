#!/usr/bin/env python3
"""Independent structural checks of vsr-flow.svg and the SVG inside vsr-flow.html."""
import re, sys, os, xml.etree.ElementTree as ET
D = os.path.dirname(os.path.abspath(__file__))
NS = "{http://www.w3.org/2000/svg}"

def check(svg_text, name):
    root = ET.fromstring(svg_text.encode("utf-8"))
    assert root.tag == NS + "svg" and root.get("viewBox").split()[2] == "1456", "viewBox width"
    dur = None; n = 0
    for e in root.iter():
        tag = e.tag.replace(NS, "")
        assert tag not in ("script", "image", "foreignObject", "use", "a"), "forbidden element " + tag
        for a, v in e.attrib.items():
            assert a not in ("href", "{http://www.w3.org/1999/xlink}href"), "external reference"
            assert "http" not in v or a == "xmlns", "url in attribute %s=%s" % (a, v)
            assert not (a in ("fill", "stroke", "stop-color") and v.startswith("#")), "hard colour in markup"
        if tag in ("animate", "animateTransform", "animateMotion", "set"):
            n += 1
            assert tag != "set", "set not used"
            for bad in ("begin", "end", "fill", "onbegin", "repeatDur", "min", "max"):
                assert bad not in e.attrib, "%s on animation" % bad
            assert e.get("repeatCount") == "indefinite"
            d = e.get("dur"); dur = dur or d; assert d == dur, "dur differs"
            kt = [float(x) for x in e.get("keyTimes").split(";")]
            vals = e.get("values").split(";")
            assert len(kt) == len(vals), "keyTimes/values count"
            assert kt[0] == 0 and kt[-1] == 1 and all(a < b for a, b in zip(kt, kt[1:])), "keyTimes order"
            if e.get("calcMode") == "discrete":
                assert tag == "animate"
    css = "".join(s.text for s in root.iter(NS + "style"))
    for m in re.finditer(r"font-size:(\d+)px", css):
        assert int(m.group(1)) >= 19, "font-size below 19"
    assert "font-size" not in re.sub(r"<style.*?</style>", "", svg_text, flags=re.S), "inline font-size"
    # colours only in the style block (hex) and declared once
    assert len(re.findall(r"--bg:#", css)) == 2, "dark + light variants"
    body = re.sub(r"<style.*?</style>", "", svg_text, flags=re.S)
    assert not re.search(r"#[0-9a-fA-F]{6}", body), "hex colour outside style"
    assert "@media (prefers-color-scheme: light)" in css
    print("%s: %d animation elements, dur %s, all keyTimes ok" % (name, n, dur))

svg = open(os.path.join(D, "vsr-flow.svg"), encoding="utf-8").read()
html = open(os.path.join(D, "vsr-flow.html"), encoding="utf-8").read()
check(svg.split("?>", 1)[1], "vsr-flow.svg")
check(re.search(r"<svg .*?</svg>", html, re.S).group(0), "vsr-flow.html (inline svg)")
for f, lim in (("vsr-flow.svg", 380), ("vsr-flow.html", 480)):
    kb = os.path.getsize(os.path.join(D, f)) / 1024
    assert kb <= lim, f; print("%s %.1f KB (limit %d KB)" % (f, kb, lim))
assert "<script" not in svg
print("OK")
