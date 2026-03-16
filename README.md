<div align="center">

# shclaw

**Self-contained multi-agent AI orchestrator in C.**
**Single static binary. No dependencies. No runtime.**

<br>

![C](https://img.shields.io/badge/C11-00599C?style=flat-square&logo=c&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)
![musl](https://img.shields.io/badge/musl-518K-blue?style=flat-square)
![cosmo](https://img.shields.io/badge/cosmo-956K-blue?style=flat-square)
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

I got tired of bloatware. Every "AI agent framework" I looked at (with a few notable exceptions) pulled in half the npm registry or needed a Python virtualenv the size of a small country. I wanted something that would just *run*. No runtime, no interpreter, no package manager, no containers required. Just a binary and a kernel.

So I wrote shclaw: a multi-agent AI orchestrator in C that compiles down to a single static binary under 1MB. It talks to LLMs (Claude, GPT, Ollama), lives on IRC, schedules its own tasks, remembers things across sessions, and -- the part I find genuinely cool -- it embeds a C compiler so agents can write and compile their own plugins at runtime. The whole thing runs natively on Linux, FreeBSD, NetBSD, and OpenBSD from the same ELF binary.

This is a side project. A toy. Something I hack on because the problem space is interesting and because there's something satisfying about fitting TLS, HTTP, IRC, JSON parsing, an agentic loop, and a C compiler into 956 kilobytes -- or just 518K if you only need Linux.

It's also largely vibe-coded. I had the building blocks, the architecture in my head, and enough C and systems knowledge to steer things -- but the bulk of the code was written through a combination of Claude, Codex, and me yelling at both of them until the binary linked.

> **Fair warning.** Shclaw gives autonomous AI agents access to shell commands, file I/O, and network calls. One of them can write and compile C at runtime. Run it somewhere you don't care about -- a VM, a container, a Raspberry Pi on a VLAN.

```
~6000 lines of C · ~518K static (musl) · ~956K cross-platform (cosmo) · 4 operating systems
```

---

## How it works

```
    agents          hub · research · builder (1 pthread each)
      │                     │
      ▼                     ▼
  event loop ◄──── poll() on IRC fd + unix socket fd
      │               5s timer: scheduler, inbox, plugin scan
      │
      ├── IRC (TLS 6697)        ← owner talks to agents
      ├── unix socket           ← TUI / CLI
      ├── scheduler + inbox     ← timed tasks, inter-agent messages
      └── plugin scanner        ← recompile changed .c files via TCC
```

Each agent runs in its own pthread. A trigger (IRC mention, scheduled task, inter-agent message, CLI command) creates a session thread. The agent builds a system prompt from its personality, memory, schedule, and peer list, then enters a conversation loop with the LLM provider. When the LLM returns tool calls, it executes them and feeds results back. When it returns text, the response goes to IRC or the TUI socket. Sessions have a configurable turn limit (default 100) and a 5-second cooldown between sessions for the same agent.

The event loop is a single `poll()` call watching the IRC socket and the Unix domain socket, with a 5-second timer for inbox delivery, task scheduling, and plugin directory scanning.

### Agent roles

Each agent is defined by an INI file in `etc/agents/`. Two flags control special behavior:

- **`hub = true`** -- This agent is the default recipient. Any IRC message that doesn't start with an `@mention` gets routed to the hub. There should be exactly one hub agent -- it's the "front desk" that handles general conversation and decides when to delegate to specialists via `send_message`. The hub doesn't need a big model -- something like `qwen3.5:9b` via Ollama or `gpt-4.1-nano` works well for routing and casual conversation.
- **`builder = true`** -- This agent gets access to the `create_plugin` tool. It can write C source code and have the daemon compile it into a live tool via TCC. You don't want every agent to have this -- it's powerful and dangerous, so you give it to one dedicated agent with a carefully crafted system prompt that knows the plugin API constraints (`-nostdlib`, `tc_plugin.h` only, no libc). **This requires a capable model** (Claude Sonnet/Opus, GPT-4.1) -- smaller models will hallucinate libc headers or produce code that doesn't compile. Use the `builder.ini.example` template, it contains enough instructions in `system_prompt_extra` to keep the model on track.

A typical setup: a hub agent (general-purpose, cheap/local model), a research agent (analysis, standard model), and a builder agent (plugin creation, capable model). They coordinate via `send_message` -- file-based inboxes in `data/messages/<agent>/`, polled every 5 seconds by the daemon.

Right now, the only communication channels are IRC and the Unix socket (TUI/CLI). I'm planning to add more channels soon.

### Two-tier memory

Each agent has two independent persistence mechanisms:

**Memory** (`data/<agent>/memory/memory.jsonl`) -- An append-only JSONL log. When an agent calls `remember`, a JSON object is appended with the content, a category (`general`, `project`, `person`, `event`, `error`), an importance score (1-10), tags, and a timestamp. When the agent calls `recall`, it searches by keyword and tag matching. The last N memories are injected into the system prompt at the start of each session, so the agent has context about past interactions.

**Facts** (`data/<agent>/memory/facts.json`) -- A flat key-value JSON object. When an agent calls `set_fact`, it stores a permanent association (`timezone` = `Europe/Paris`, `owner_name` = `Alice`). Facts are always included in the system prompt -- they're the agent's "hard knowledge" that never gets pruned. Unlike memories which accumulate and may eventually need trimming, facts are meant to be few, precise, and permanent.

The idea: memories are for episodic recall ("last Tuesday the server had a disk issue"), facts are for identity and configuration ("the owner speaks French", "the prod server is 10.0.1.5").

### The TCC trick

The single most unusual piece of shclaw is that it embeds [TinyCC](https://bellard.org/tcc/) (libtcc) -- Fabrice Bellard's C compiler -- as a library. When an agent wants to create a new tool, it writes C source code. The daemon feeds it to `tcc_compile_string()`, relocates it in-memory with `tcc_relocate()`, and resolves the entry point with `tcc_get_symbol()`. No `.so` file is ever written to disk. The compiled code lives in the process's address space and is callable immediately.

Plugins run in `-nostdlib` mode: no libc, no system headers. The daemon injects a curated set of functions (`tc_malloc`, `tc_http_get`, `tc_json_parse`, etc.) via `tcc_add_symbol()` before compilation. This is both a sandbox (plugins can't call arbitrary libc functions) and a convenience (plugins get TLS-enabled HTTP for free without linking anything).

On daemon restart, `plugin_scan()` recompiles all `.c` files in `plugins/`. Changed files are detected by mtime and recompiled on the fly every 5 seconds.

### How the ELF runs on four kernels

The musl build produces a standard Linux static binary (~518K, hardened with static-PIE, RELRO, NX, stack protector, FORTIFY_SOURCE). It works on x86_64, aarch64, and armv7l -- architecture detection is automatic. I've tested it on a Raspberry Pi 3B+ running 32-bit Raspbian and it works fine (static-pie is disabled on armv7l due to musl/kernel quirks, falls back to plain `-static`).

The Cosmopolitan build is more interesting. [Cosmopolitan Libc](https://justine.lol/cosmopolitan/) by Justine Tunney provides a C library that targets multiple operating systems from the same binary. We compile with `x86_64-unknown-cosmo-cc` (not the fat `cosmocc` -- TinyCC generates x86_64 ELF relocations, so we need the x86_64-specific toolchain). The `assimilate` step converts the APE (Actually Portable Executable) format to a native ELF that the kernel loads directly -- no shell trampoline, no interpreter.

The result: one 956K ELF binary that runs unmodified on Linux, NetBSD, FreeBSD, and OpenBSD. Same binary, four kernels. I've only tested Linux and NetBSD so far -- FreeBSD and OpenBSD are on the list, I just haven't gotten around to it yet.

Getting TCC to work under Cosmopolitan required three patches (`patches/tcc-cosmo.patch`):

1. **NULL guard in `tcc_split_path()`** -- Cosmopolitan doesn't set `tcc_lib_path` by default; dereferencing it crashes. Fix: fallback to `"."`.
2. **`strcpy` to `memcpy` for PLT names** -- Cosmopolitan's `strcpy` uses SSE instructions (`memrchr16_sse`) that crash on certain stack-aligned small buffers. This one was fun to track down.
3. **Skip empty section names** -- `tcc_add_linker_symbols()` generates duplicate symbols for sections with empty names after dot-stripping.

### What's inside

| Component | Project | Role |
|-----------|---------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) by Thomas Pornin | HTTPS to LLM APIs, IRC over TLS. No dynamic allocation, no dependencies. |
| C compiler | [TinyCC](https://bellard.org/tcc/) by Fabrice Bellard | In-memory plugin compilation. libtcc embedded as a library. |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble | Parse/generate all LLM API payloads. Single file, ~1700 lines. |
| C library | [musl](https://musl.libc.org/) or [Cosmopolitan](https://justine.lol/cosmopolitan/) | Static linking. musl for Linux, Cosmopolitan for cross-platform. |

Everything else (HTTP/1.1 client, IRC client, INI parser, ANSI TUI, scheduler, memory system, plugin scanner) is written from scratch -- about 6000 lines of C across 20 source files.

---

## Getting started

### Dependencies

Debian/Ubuntu:
```bash
sudo apt install build-essential musl-tools git
```

That's it. The Makefile auto-fetches vendored sources ([BearSSL](https://bearssl.org/), [TinyCC](https://bellard.org/tcc/), [cJSON](https://github.com/DaveGamble/cJSON)) on first build via `vendor.sh`.

For the Cosmopolitan build, the toolchain is also auto-fetched (~40MB download, cached in `vendor/cosmo/`).

### Build

```bash
git clone https://github.com/govlog/shclaw.git && cd shclaw

# Linux only (musl-gcc, static, hardened)
make musl          # => ./shclaw (~518K)

# Cross-platform (Cosmopolitan, x86_64)
make cosmo         # => ./shclaw.com (~956K, runs on Linux/NetBSD/FreeBSD/OpenBSD)

make clean         # clean everything
```

### Configure

Shclaw uses flat INI files. No YAML, no JSON config, no env vars.

```bash
mkdir -p etc/agents
```

**`etc/config.ini`** -- global settings (providers, tiers, IRC, paths):

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

# Optional -- remove this section to run without IRC
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

See `etc/config.ini.example` and `etc/agents/*.ini.example` for the full reference.

### Run

#### Bare metal

```bash
./shclaw                  # foreground
./shclaw -d               # daemonize (fork + setsid)
./shclaw tui              # terminal chat UI (connects to running daemon)
./shclaw tui oracle       # talk to a specific agent
./shclaw msg jarvis "hi"  # fire-and-forget message
./shclaw status           # agent status (JSON)
./shclaw stop             # graceful shutdown
```

The daemon looks for `etc/config.ini` in the current directory. Override with `--workdir=/path/to/instance`.

#### Docker

```bash
# Build image
make docker-image     # => shclaw:latest (~11MB Alpine)

# Prepare an instance directory
mkdir -p my-instance/{etc/agents,data,logs,plugins}
# copy and edit config.ini + agent .ini files into my-instance/etc/

# Run
docker run -v ./my-instance:/app/instance shclaw
```

#### smolBSD (NetBSD microVM)

[smolBSD](https://github.com/NetBSDfr/smolBSD) boots a minimal NetBSD VM in ~60ms via QEMU. Shclaw's Cosmopolitan binary runs natively on it.

```bash
# Fetch smolBSD (first time)
./vendor.sh smolbsd

# Build the microVM image (requires the cosmo binary)
make smolbsd AGENT_DIR=/path/to/instance

# Boot it
cd vendor/smolbsd
./startnb.sh -k kernels/netbsd-SMOL -i images/shclaw-amd64.img -w /path/to/instance
```

The instance directory is mounted via 9P virtfs at `/mnt`. The VM boots, mounts the host directory, and starts shclaw in about a second.

#### System install

```bash
make install PREFIX=~/.local/shclaw

# The prefix is baked into the binary -- it always looks there for config
~/.local/shclaw/bin/shclaw
```

Creates `$PREFIX/{bin,etc,etc/agents,plugins,include,data,logs}`.

---

## Agent tools

Agents have 15 built-in tools:

| Tool | What it does |
|------|-------------|
| `exec` | Run a shell command (fork+exec, pipe capture, timeout, pgid kill) |
| `read_file` | Read file contents (with offset and limit) |
| `write_file` | Write/append file (atomic: tmp + rename) |
| `schedule_task` | One-shot task at an ISO 8601 time |
| `schedule_recurring` | Recurring task (30min / hourly / 6h / 12h / daily / weekly) |
| `list_tasks` | List scheduled tasks |
| `update_task` | Modify an existing task |
| `cancel_task` | Cancel a task by ID |
| `remember` | Save a memory (tagged, categorized, importance-scored) |
| `recall` | Search memories by keyword |
| `set_fact` | Store a permanent key-value fact |
| `get_fact` | Retrieve a fact |
| `send_message` | Message another agent, the owner (via IRC), or broadcast |
| `list_agents` | List running agents |
| `create_plugin` | Write C source + compile via TCC (builder agents only) |

Plugins written by agents become tools available to all agents immediately.

---

## Plugin API

Plugins are single `.c` files. They `#include "tc_plugin.h"` and export three symbols:

```c
#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "weather";
const char *TC_PLUGIN_DESC = "Get current weather for a city";

const char *tc_execute(const char *input_json) {
    void *json = tc_json_parse(input_json);
    const char *city = tc_json_string(tc_json_get(json, "city"));

    char url[256];
    tc_snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);

    static char result[512];
    int status = tc_http_get(url, result, sizeof(result));
    tc_json_free(json);

    return (status == 200) ? result : "error";
}
```

Available functions (injected by the daemon at compile time):

| Category | Functions |
|----------|-----------|
| Memory | `tc_malloc`, `tc_free` |
| Strings | `tc_strlen`, `tc_strcmp`, `tc_strncmp`, `tc_strcpy`, `tc_strncpy`, `tc_snprintf`, `tc_memcpy`, `tc_memset` |
| Files | `tc_read_file`, `tc_write_file` |
| HTTP | `tc_http_get`, `tc_http_post`, `tc_http_post_json`, `tc_http_header` |
| JSON | `tc_json_parse`, `tc_json_free`, `tc_json_print`, `tc_json_get`, `tc_json_index`, `tc_json_array_size`, `tc_json_string` |
| System | `tc_gethostname` |
| Logging | `tc_log` |

HTTP calls use the daemon's BearSSL stack -- plugins get HTTPS for free.

---

## IRC

Single connection, single nick, one channel. Agents are multiplexed on the same connection.

```
you>    hey, check the server load              => routes to hub agent
you>    @oracle analyze this log file           => routes to oracle
you>    @oracle X @jarvis Y                     => fan-out to both
you>    @all status                             => broadcast

bot>    jarvis: CPU is at 12%, all good.
bot>    oracle: I see 3 anomalies in the log...
```

TLS is automatic on port 6697 (BearSSL). If no channel/key is configured, shclaw generates random ones -- retrieve them with `shclaw irc-info`.

---

## Documentation status

This is one person's side project. The code is the documentation for now. If you're interested in running this or contributing, feel free to open an issue -- I'm happy to write better docs for the parts people actually want to use.

Areas where help would be welcome:
- ARM64 testing on NetBSD/FreeBSD
- More plugin examples
- Documentation in general

---

## Credits

This project wouldn't exist without the work of:

- **[Justine Tunney](https://justine.lol/)** -- [Cosmopolitan Libc](https://justine.lol/cosmopolitan/) and the APE format. The idea that a single binary can run on four operating systems, for real, without emulation.
- **[Fabrice Bellard](https://bellard.org/)** -- [TinyCC](https://bellard.org/tcc/). A complete C compiler small enough to embed as a library. This is what makes runtime plugin compilation possible.
- **[Thomas Pornin](https://www.bearssl.org/)** -- [BearSSL](https://bearssl.org/). A TLS implementation designed for embedded systems: small, no-allocation, constant-time crypto. It's why a sub-1MB binary can talk HTTPS.
- **[Dave Gamble](https://github.com/DaveGamble/cJSON)** -- [cJSON](https://github.com/DaveGamble/cJSON). Single-file JSON parser. Simple, reliable, handles everything the LLM APIs throw at it.
- **[iMil](https://x.com/iMilnb)** -- [smolBSD](https://github.com/NetBSDfr/smolBSD). Minimal NetBSD microVM framework. Boot a full BSD in 20ms.

## License

MIT. See [LICENSE](LICENSE).

Vendored libraries carry their own licenses:

| Library | License |
|---------|---------|
| BearSSL | MIT |
| cJSON | MIT |
| TinyCC | LGPL-2.1 |
| musl libc | MIT |
| Cosmopolitan Libc | ISC |

See [NOTICE](NOTICE) for full details.
