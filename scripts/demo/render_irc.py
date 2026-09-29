#!/usr/bin/env python3
"""Render a recorded #shclaw log (irc_owner.py output) as a terminal IRC window.

Usage: render_irc.py <log.jsonl> <title> <out.html>
"""
import html, json, re, sys

AGENT_COLORS = {"jarvis": "#61afef", "oracle": "#c678dd", "builder": "#e5c07b"}
NICK_COLORS = {"govlog": "#56b6c2", "shclaw": "#ff6b4a"}


def esc(s):
    return html.escape(s, quote=False)


def agent_prefix(text):
    """'oracle: hi' -> ('oracle', 'hi'); 'jarvis => oracle: x' -> arrow form"""
    m = re.match(r"^(\w+) => (\w+): (.*)$", text)
    if m and m.group(1) in AGENT_COLORS:
        a, b, rest = m.groups()
        return (f'<span style="color:{AGENT_COLORS[a]}">{a}</span>'
                f'<span class="dim"> → </span>'
                f'<span style="color:{AGENT_COLORS.get(b, "#abb2bf")}">{b}</span>'
                f'<span class="dim">:</span> {esc(rest)}')
    m = re.match(r"^(\w+): (.*)$", text)
    if m and m.group(1) in AGENT_COLORS:
        a, rest = m.groups()
        return f'<span style="color:{AGENT_COLORS[a]}">{a}</span><span class="dim">:</span> {esc(rest)}'
    return esc(text)


def action(text):
    """'jarvis calls weather(city=Paris)' with the tool name highlighted"""
    m = re.match(r"^(\w+) calls (\w+)\((.*)\)$", text)
    if not m:
        return f'<span class="act">{esc(text)}</span>'
    who, tool, args = m.groups()
    return (f'<span style="color:{AGENT_COLORS.get(who, "#abb2bf")}">{who}</span>'
            f'<span class="act"> calls </span><span class="tool">{tool}</span>'
            f'<span class="act">({esc(args)})</span>')


rows = []
last_t = None
for line in open(sys.argv[1]):
    d = json.loads(line)
    t = d["t"] if d["t"] != last_t else ""
    last_t = d["t"]
    if d["kind"] == "action":
        nick, body = '<span class="dim">*</span>', action(d["text"])
    else:
        c = NICK_COLORS.get(d["nick"], "#abb2bf")
        nick = f'<span style="color:{c}">{esc(d["nick"])}</span>'
        body = agent_prefix(d["text"]) if d["nick"] == "shclaw" else esc(d["text"])
    rows.append(f'<div class="t">{t}</div><div class="n">{nick}</div>'
                f'<div class="s">│</div><div class="m">{body}</div>')

title = esc(sys.argv[2])
page = f"""<!doctype html><html><head><meta charset="utf-8"><style>
body {{ margin:0; padding:10px; background:#ffffff; zoom:2; }}
.win {{ width:960px; border-radius:12px; overflow:hidden; background:#0f1320;
        box-shadow:0 0 0 1px #2a3142;
        font:14.5px/1.5 'DejaVu Sans Mono', monospace; color:#d8dee9; }}
.bar {{ height:34px; background:#1a2030; display:flex; align-items:center; padding:0 14px;
        color:#8b949e; font-size:13px; }}
.dot {{ width:12px; height:12px; border-radius:50%; margin-right:8px; }}
.bar .title {{ flex:1; text-align:center; margin-right:60px; }}
.log {{ display:grid; grid-template-columns:auto auto auto 1fr; column-gap:8px;
        padding:14px 16px 8px; }}
.t {{ color:#5c6370; }} .n {{ text-align:right; font-weight:bold; }}
.s {{ color:#3b4252; }} .m {{ white-space:pre-wrap; word-break:break-word; }}
.dim {{ color:#6b7385; }} .act {{ color:#8b93a6; font-style:italic; }}
.tool {{ color:#98c379; font-weight:bold; }}
.status {{ background:#1a2030; color:#8b949e; padding:4px 16px; font-size:13px; }}
.status b {{ color:#ff6b4a; font-weight:normal; }}
.input {{ padding:6px 16px 10px; color:#6b7385; }}
.cur {{ background:#8ef58a; color:#8ef58a; }}
</style></head><body><div class="win">
<div class="bar"><span class="dot" style="background:#ff5f57"></span><span class="dot" style="background:#febc2e"></span><span class="dot" style="background:#28c840"></span><span class="title">{title}</span></div>
<div class="log">{''.join(rows)}</div>
<div class="status">[{last_t}] [govlog] 2:local/<b>#shclaw</b>(+k) {{shclaw: jarvis · oracle · builder}}</div>
<div class="input">[#shclaw] <span class="cur">_</span></div>
</div></body></html>"""
open(sys.argv[3], "w").write(page)
