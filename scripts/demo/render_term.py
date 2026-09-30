#!/usr/bin/env python3
"""Render a recorded shell session as a terminal window.

The session is plain text: lines starting with "$ " are commands, the others
their output. Usage: render_term.py <session.txt> <title> <out.html>
"""
import html, sys


def esc(s):
    return html.escape(s, quote=False)


def output(line):
    if line.startswith("==> "):
        return f'<span class="built">{esc(line)}</span>'
    if line == "All checks passed.":
        return f'<span class="ok">{esc(line)}</span>'
    if line.split("\t")[0] in ("real", "user", "sys"):
        return f'<span class="dim">{esc(line)}</span>'
    return esc(line)


cwd = "~"
rows = []
for line in open(sys.argv[1]).read().rstrip("\n").split("\n"):
    if line.startswith("$ "):
        cmd = line[2:]
        rows.append(f'<span class="cwd">{cwd}</span> <span class="dim">$</span> '
                    f'<span class="cmd">{esc(cmd)}</span>')
        if cmd.startswith("cd "):
            cwd = f"{cwd}/{cmd[3:]}"
    else:
        rows.append(output(line))

title = esc(sys.argv[2])
page = f"""<!doctype html><html><head><meta charset="utf-8"><style>
body {{ margin:0; padding:10px; background:#ffffff; zoom:2; }}
.win {{ width:960px; border-radius:12px; overflow:hidden; background:#0f1320;
        box-shadow:0 0 0 1px #2a3142;
        font:14.5px/1.55 'DejaVu Sans Mono', monospace; color:#abb2bf; }}
.bar {{ height:34px; background:#1a2030; display:flex; align-items:center; padding:0 14px;
        color:#8b949e; font-size:13px; }}
.dot {{ width:12px; height:12px; border-radius:50%; margin-right:8px; }}
.bar .title {{ flex:1; text-align:center; margin-right:60px; }}
.log {{ padding:14px 16px 16px; white-space:pre-wrap; word-break:break-word; }}
.cwd {{ color:#56b6c2; }} .dim {{ color:#5c6370; }} .cmd {{ color:#e6e6e6; font-weight:bold; }}
.built {{ color:#ff6b4a; font-weight:bold; }} .ok {{ color:#98c379; font-weight:bold; }}
.cur {{ background:#8ef58a; color:#8ef58a; }}
</style></head><body><div class="win">
<div class="bar"><span class="dot" style="background:#ff5f57"></span><span class="dot" style="background:#febc2e"></span><span class="dot" style="background:#28c840"></span><span class="title">{title}</span></div>
<div class="log">{chr(10).join(rows)}
<span class="cwd">{cwd}</span> <span class="dim">$</span> <span class="cur">_</span></div>
</div></body></html>"""
open(sys.argv[3], "w").write(page)
