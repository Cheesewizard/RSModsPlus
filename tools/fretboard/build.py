#!/usr/bin/env python3
"""Assemble the fretboard artifact: inject guitar_data.js into the template to produce
fretboard.html (a self-contained page). Run after prep_fretboard_data.py, or on its own
if guitar_data.js is already present (it is committed)."""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
tpl = open(os.path.join(HERE, "fretboard_template.html"), encoding="utf-8").read()
data = open(os.path.join(HERE, "guitar_data.js"), encoding="utf-8").read()
out = tpl.replace("/*__GUITAR_DATA__*/", data)
dst = os.path.join(HERE, "fretboard.html")
open(dst, "w", encoding="utf-8").write(out)
print(f"wrote {dst}  ({len(out)/1e6:.2f} MB)")
