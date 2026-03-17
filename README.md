<div align="center">

# shclaw

**Self-contained multi-agent AI orchestrator in C.**
**Single static binary. No dependencies. No runtime.**

<br>

![C](https://img.shields.io/badge/C11-00599C?style=flat-square&logo=c&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)
![musl](https://img.shields.io/badge/musl-528K-blue?style=flat-square)
![cosmo](https://img.shields.io/badge/cosmo-968K-blue?style=flat-square)
![Lines](https://img.shields.io/badge/~6000_lines-grey?style=flat-square)
![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat-square&logo=linux&logoColor=black)
![FreeBSD](https://img.shields.io/badge/FreeBSD-AB2B28?style=flat-square&logo=freebsd&logoColor=white)
![NetBSD](https://img.shields.io/badge/NetBSD-FF6600?style=flat-square&logo=netbsd&logoColor=white)
![OpenBSD](https://img.shields.io/badge/OpenBSD-F2CA30?style=flat-square&logo=openbsd&logoColor=black)

<br>

*IRC · Multi-LLM · Runtime C plugins · Scheduling · Inter-agent messaging*

<br>

<img src="screen.png" alt="shclaw TUI" width="700">

</div>

<br>

*[Lire en francais](README-fr.md)*

## What is this

A multi-agent AI orchestrator in C. One static binary under 1MB. It talks to LLMs (Claude, GPT, Ollama), lives on IRC, schedules its own tasks, and remembers things across sessions.

The binary is polyglot: built with [Cosmopolitan Libc](https://justine.lol/cosmopolitan/), it produces a single ELF that runs unmodified on Linux, FreeBSD, NetBSD, and OpenBSD. Same file, four kernels, no emulation -- the libc abstracts away syscall differences at compile time.

It also embeds [TinyCC](https://bellard.org/tcc/) (Fabrice Bellard's C compiler) as a library. When an agent wants a new tool, it writes C source code. The daemon compiles it in-memory with `tcc_compile_string()`, relocates it into the process address space with `tcc_relocate()`, and resolves the entry point with `tcc_get_symbol()`. No `.so` ever touches disk -- the compiled code is live and callable immediately. Plugins are sandboxed: no libc access, only a curated set of functions (HTTP+TLS, JSON, file I/O) injected by the daemon before compilation. This means the binary is simultaneously an AI orchestrator, an IRC client, a TLS stack, and a C compiler -- all in under 1MB.

> **Fair warning.** Shclaw gives AI agents access to shell commands, file I/O, and network calls. One of them can write and compile C at runtime. Run it somewhere you don't care about -- a VM, a container, a Pi on a VLAN.

---

## How it works

The daemon runs an event loop that watches IRC and a Unix socket. Triggers (IRC messages, scheduled tasks, inter-agent messages, CLI commands) create sessions. Each agent has its own thread, a personality, persistent memory, and access to tools.

Agents coordinate via `send_message` -- file-based inboxes polled every 5 seconds.

### Agent types

Each agent is defined by an INI file in `etc/agents/`. Two flags matter:

- **`hub = true`** -- Default recipient for IRC messages without an `@mention`. Handles general conversation, delegates to specialists. Works fine with small models (`qwen3.5:9b`, `gpt-4.1-nano`).
- **`builder = true`** -- Can create C plugins at runtime via `create_plugin`. Only sees 4 tools to keep its context focused. The plugin template is pre-injected into its prompt, and the daemon auto-extracts code if the model outputs it as text. Even small models like `qwen3.5:9b` can create working plugins on the first try.

A typical setup: a hub (cheap/local model), a research agent (standard model), a builder (capable model).

### Memory

Each agent has two persistence layers:

- **Memories** -- Append-only log with categories, importance scores, and tags. Searched via `recall`. Recent memories are included in the system prompt.
- **Facts** -- Permanent key-value pairs (e.g. `timezone = Europe/Paris`). Always in the system prompt. Few, precise, never pruned.

### Runtime plugins

Agents can write C plugins that get compiled in-memory by [TinyCC](https://bellard.org/tcc/). No `.so` hits disk. Plugins run sandboxed (no libc) but get HTTP+TLS, JSON, and file I/O through injected `tc_*` functions.

See [doc/plugin-api.md](doc/plugin-api.md) for the full plugin API.

---

## Getting started

### Build

```bash
sudo apt install build-essential musl-tools git  # Debian/Ubuntu
git clone https://github.com/govlog/shclaw.git && cd shclaw

make musl          # Linux only, ~528K
make cosmo         # Cross-platform (Linux/NetBSD/FreeBSD/OpenBSD), ~968K
```

Vendor libraries are auto-fetched on first build.

### Configure

```bash
mkdir -p etc/agents
```

**`etc/config.ini`** -- providers, IRC, paths:

```ini
[daemon]
data_dir = ./data
log_dir  = ./logs

[provider.anthropic]
type    = anthropic
api_key = sk-ant-api03-YOUR-KEY-HERE

[provider.ollama]
type     = openai
base_url = http://localhost:11434
api_key  =

[tiers]
simple   = anthropic/claude-haiku-4-5-20251001
standard = anthropic/claude-sonnet-4-6
complex  = anthropic/claude-opus-4-6
local    = ollama/llama3

# Optional -- remove to run without IRC
[irc]
server      = irc.libera.chat
port        = 6697
nick        = mybot
channel     = #my-channel
channel_key = mysecretkey
owner       = mynick
```

**`etc/agents/jarvis.ini`** -- one file per agent:

```ini
[agent]
name       = jarvis
model      = simple
hub        = true
max_turns  = 30
personality = You are Jarvis, a helpful and efficient assistant.

[objectives]
1 = Answer questions quickly and efficiently
2 = Keep track of important tasks in memory
```

See `etc/agents/*.ini.example` for more examples.

### Run

```bash
./shclaw                  # foreground
./shclaw -d               # daemonize
./shclaw tui              # terminal chat UI
./shclaw tui oracle       # talk to a specific agent
./shclaw msg jarvis "hi"  # send a message
./shclaw status           # agent status (JSON)
./shclaw stop             # graceful shutdown
```

The daemon looks for `etc/config.ini` in the current directory. Override with `--workdir=/path/to/instance`.

### Docker

```bash
make docker-image
docker run -v ./my-instance:/app/instance shclaw
```

### smolBSD (NetBSD microVM)

The Cosmopolitan binary runs on [smolBSD](https://github.com/NetBSDfr/smolBSD) -- a minimal NetBSD VM that boots in ~60ms.

```bash
make smolbsd AGENT_DIR=/path/to/instance
cd vendor/smolbsd && ./startnb.sh -k kernels/netbsd-SMOL \
  -i images/shclaw-amd64.img -w /path/to/instance
```

---

## Agent tools

16 built-in tools:

| Tool | What it does |
|------|-------------|
| `exec` | Run a shell command |
| `read_file` | Read a file |
| `write_file` | Write/append a file |
| `schedule_task` | One-shot task at a given time |
| `schedule_recurring` | Recurring task |
| `list_tasks` | List scheduled tasks |
| `update_task` | Modify a task |
| `cancel_task` | Cancel a task |
| `remember` | Save a memory |
| `recall` | Search memories |
| `set_fact` | Store a key-value fact |
| `get_fact` | Retrieve a fact |
| `send_message` | Message an agent, the owner, or broadcast |
| `list_agents` | List running agents |
| `create_plugin` | Write + compile a C plugin (builder only) |
| `clear_memory` | Clear memories/facts |

Plugins created by agents become tools available to everyone immediately.

---

## IRC

Single connection, single nick, one channel. Agents share the connection.

```
you>    hey, check the server load              => routed to hub
you>    @oracle analyze this log file           => routed to oracle
you>    @oracle X @jarvis Y                     => both get their part
you>    @all status                             => broadcast

bot>    jarvis: CPU is at 12%, all good.
bot>    oracle: I see 3 anomalies in the log...
```

---

## More docs

- [Plugin API](doc/plugin-api.md) -- writing plugins, available `tc_*` functions
- [Internals](doc/internals.md) -- event loop, TCC compilation, Cosmopolitan build

---

## Credits

- **[Justine Tunney](https://justine.lol/)** -- [Cosmopolitan Libc](https://justine.lol/cosmopolitan/). One binary, four operating systems.
- **[Fabrice Bellard](https://bellard.org/)** -- [TinyCC](https://bellard.org/tcc/). A C compiler small enough to embed as a library.
- **[Thomas Pornin](https://www.bearssl.org/)** -- [BearSSL](https://bearssl.org/). TLS for embedded systems.
- **[Dave Gamble](https://github.com/DaveGamble/cJSON)** -- [cJSON](https://github.com/DaveGamble/cJSON). Single-file JSON parser.
- **[iMil](https://x.com/iMilnb)** -- [smolBSD](https://github.com/NetBSDfr/smolBSD). NetBSD microVMs.

## License

MIT. See [LICENSE](LICENSE).

Vendored libraries: BearSSL (MIT), cJSON (MIT), TinyCC (LGPL-2.1), musl (MIT), Cosmopolitan (ISC). See [NOTICE](NOTICE).
