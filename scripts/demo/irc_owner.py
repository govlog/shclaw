#!/usr/bin/env python3
"""Play the owner on #shclaw and record the channel.

Usage: irc_owner.py <scenario.json> <log.jsonl>
scenario: [{"say": "...", "quiet": 12, "max": 180, "until": "optional text"}]
Each step sends one line, then waits until the bot has been quiet for
`quiet` seconds (or until a line containing `until` arrives), at most `max`.
"""
import json, socket, sys, time

HOST, PORT, NICK, CHAN, KEY = "127.0.0.1", 6667, "govlog", "#shclaw", "crabs"

steps = json.load(open(sys.argv[1]))
log = open(sys.argv[2], "a", buffering=1)
s = socket.create_connection((HOST, PORT))
s.settimeout(1.0)
buf = b""
last_bot = time.time()


def send(line):
    s.sendall(line.encode() + b"\r\n")


def record(kind, nick, text):
    log.write(json.dumps({"t": time.strftime("%H:%M"), "kind": kind,
                          "nick": nick, "text": text}) + "\n")


def pump():
    """Read what arrived; return the channel lines seen."""
    global buf, last_bot
    seen = []
    try:
        data = s.recv(65536)
    except socket.timeout:
        return seen
    if not data:
        raise SystemExit("disconnected")
    buf += data
    while b"\r\n" in buf:
        raw, buf = buf.split(b"\r\n", 1)
        line = raw.decode("utf-8", "replace")
        if line.startswith("PING"):
            send("PONG" + line[4:])
            continue
        parts = line.split(" ", 3)
        if len(parts) >= 4 and parts[1] == "PRIVMSG" and parts[2] == CHAN:
            nick = parts[0][1:].split("!")[0]
            text = parts[3][1:]
            kind = "msg"
            if text.startswith("\x01ACTION ") and text.endswith("\x01"):
                kind, text = "action", text[8:-1]
            record(kind, nick, text)
            last_bot = time.time()
            seen.append(text)
        elif len(parts) >= 3 and parts[1] == "JOIN":
            nick = parts[0][1:].split("!")[0]
            if nick != NICK:
                record("join", nick, CHAN)
    return seen


send(f"NICK {NICK}")
send(f"USER {NICK} 0 * :owner")
while b" 001 " not in buf:
    try:
        buf += s.recv(65536)
    except socket.timeout:
        pass
    for l in buf.split(b"\r\n"):
        if l.startswith(b"PING"):
            send("PONG" + l[4:].decode())
buf = b""
send(f"JOIN {CHAN} {KEY}")
t0 = time.time()
while time.time() - t0 < 25:       # let the bot join
    pump()

for step in steps:
    if "say" in step:          # a step without "say" only waits
        record("msg", NICK, step["say"])
        send(f"PRIVMSG {CHAN} :{step['say']}")
    start = last_bot = time.time()
    while True:
        lines = pump()
        now = time.time()
        if step.get("until") and any(step["until"].lower() in l.lower() for l in lines):
            break
        if not step.get("until") and now - last_bot > step.get("quiet", 12) and now - start > 5:
            break
        if now - start > step.get("max", 180):
            break
    t1 = time.time()
    while time.time() - t1 < 3:
        pump()
send("QUIT :bye")
