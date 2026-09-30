# Internals

Technical details for contributors and the curious.

## Event loop and sessions

The daemon runs one `poll()` loop with a one-second tick. It watches the IRC socket, the Unix socket (CLI and TUI) and the attached TUI clients.

- **IRC is optional.** When the link drops, the loop keeps serving the socket and reconnects in the background: 5 s after a loss, then doubling up to 5 min until the server accepts the registration.
- **One session per trigger, one at a time per agent.** Each session runs in its own thread, with a 5-second gap between two sessions of the same agent. Owner messages (IRC, TUI, CLI) wait in a queue; inter-agent mail and due tasks stay on disk until the agent is free, so nothing is lost or truncated while it works.
- **Plugins** are rescanned every 5 seconds.

A session sends two things to the model: a system prompt that only changes with the agent's configuration (personality, rules, objectives, peer list, and the plugin template for the builder), then a first user turn with the session context (communication rules, facts, schedule, recent memories, what the other agents said to the owner, time) and the trigger. In an open conversation with the owner (see below), a session that answers the owner starts from the agent's previous history instead, plus one turn with the trigger, the time and what the others said since. It loops until the model answers without calling a tool (default limit: 100 turns).

## Harness for small models

Small models loop, invent tools and send broken arguments. The session loop answers each of these with an error the model can act on:

| Model mistake | Harness answer |
|---------------|----------------|
| Arguments that are not a JSON object, or that do not match the schema (missing parameter, wrong type, value outside an enum) | The tool's usage line. Numbers and booleans sent as strings are accepted |
| Unknown tool | The list of available tools |
| Same call (tool and arguments) a third time | Refused; a session that keeps repeating itself is stopped |
| Empty reply | One nudge |
| Text answer to another agent's request, without `send_message` | Sent back to that agent, marked as an answer so it cannot start a reply loop |
| Rescheduling a due reminder instead of delivering it | Due tasks read "do it now"; one-shot tasks in the past are refused |
| Wrong absolute dates | `schedule_task` also takes `in_minutes` |
| A JSON object where a string is expected (`test_input: {...}`) | Taken as its JSON text |
| Plugin schema written as raw JSON inside the C string | Its quotes are escaped when the text is valid JSON |

Providers:

- **Anthropic:** assistant turns are replayed exactly as received (thinking blocks keep their signatures), and the history is append-only.
- **OpenAI-compatible:** `history_budget` shortens old tool outputs when a session outgrows a small context window.
- **Both** retry connection errors, 408, 409, 429 and 5xx twice, honouring `Retry-After`.

## Prompt caching

Providers reuse the work already done on a prompt prefix they have seen. Anthropic and OpenAI bill those tokens at a fraction of the price (often a tenth); a local server (Ollama, llama.cpp) skips computing them again, which matters most on a CPU. Each request keeps that prefix as long as possible:

- **Tools first,** in a fixed order: built-in tools, then plugins sorted by name. The system prompt follows and holds nothing that changes between sessions.
- **First user turn, from stable to volatile:** trigger type and communication rules, facts, schedule, recent memories, the lines of the open conversation, time, then the message. Before GPT-5.6, OpenAI reuses its cache only when few tokens follow the first difference with an earlier request (on gpt-4.1-nano: a hit with 50 tokens after it, a miss with 100). The time and the message therefore come last.
- **Append-only history,** except with `history_budget`: it shortens old tool outputs to half the budget in one go, so the prefix changes rarely.
- **Conversation with the owner.** IRC and TUI messages, due tasks and answers to the agent's own requests talk to the owner. While the conversation is open (a line less than `conversation_timeout` minutes ago), such a session appends one turn to the agent's previous history, so the whole prefix stays cached. The daemon keeps the last 20 lines of the channel (owner messages and replies) with the agent each concerns; the new turn gives the lines of the other agents since the last one. At the end of a completed session the history is kept, cut to 64K characters (or `history_budget`) by dropping the oldest exchanges after the first; if the first and the last do not fit together, or the session failed, the next message starts afresh. Requests between agents stay one-shot. Everything is in memory: a restart closes the conversation.
- **Anthropic:** a cache breakpoint closes the system prompt, so the next sessions reuse tools and system prompt; automatic caching covers the conversation.
- **OpenAI:** `prompt_cache_key` is `shclaw-<agent>`. All agents start with the same tools, so without a key they share one routing bucket (about 15 requests per minute) and overflow to servers without their cache.

Each model call logs its usage, e.g. `[oracle] Tokens: 1430 in (1280 from cache, 0 to cache), 8 out`. A prefix under the provider minimum is never cached: 1024 tokens for OpenAI, 512 to 4096 for Claude depending on the model.

## Plugin compilation

shclaw embeds [TinyCC](https://bellard.org/tcc/) (libtcc) as a library. When an agent calls `create_plugin`, the daemon:

1. Decodes the source once more if it arrives on one line with literal `\n` (small models sometimes escape the code twice).
2. Compiles it once in a forked child, so a compiler crash or a bad symbol cannot take the daemon down.
3. Compiles it in memory with `tcc_compile_string()` and relocates it with `tcc_relocate()`.
4. Resolves `TC_PLUGIN_NAME`, `tc_execute`, the description and the optional schema with `tcc_get_symbol()`.
5. Only then writes the `.c` source to `plugins/`: a failed attempt never replaces a working plugin.
6. Runs it once with `test_input`, if given, and reports the output and the HTTP calls, with a note when a response did not fit the plugin's buffer.

TinyCC is built with `CONFIG_RUNMEM_RO`: plugin code is mapped read and execute, data read and write, never both at once (OpenBSD enforces this W^X rule).

Plugins are built with `-nostdlib`. Besides the `tc_*` functions, the daemon provides the helpers that TinyCC calls on its own: `memset` for zeroed arrays, `memmove` for struct copies and, on 32-bit ARM, the EABI helpers for division and 64-bit conversions (from libgcc and musl).

No `.so` is ever written to disk. At startup, `plugin_scan()` compiles every `.c` file in `plugins/`; every 5 seconds it recompiles changed files (by mtime) and unloads deleted ones. Each plugin call runs in a forked child with a 180-second timeout.

## Builds

**musl:** a static Linux binary, about 430K. With `LIBC=vendor`, it links musl 1.2.6 built from pinned sources (`./vendor.sh musl`, with the fixes Alpine applies) instead of the system's `musl-gcc`: about 390K. The Makefile supports x86_64, i686, aarch64, 32-bit ARM and riscv64. On i686, the code is built with `-mstackrealign`: TinyCC keeps the stack 4-byte aligned, while gcc code expects 16 bytes for SSE, and HTTPS calls from plugins crashed.

**Size or hardening:** by default every binary is as small as possible. This agent is already a security hole by design: remote code execution is a feature (the builder compiles and runs what a model writes, and a plugin can open a remote shell), so exploit mitigations would guard little ;D The default build:

- `-Oz` (`-Os` for Cosmopolitan and NetBSD's gcc 10), no debug info, no unwind tables, one section per function so that the linker drops unused code, link-time optimization (musl, FreeBSD, OpenBSD), symbols and section headers stripped;
- TinyCC without its backtrace and bounds checker, which only `tcc -run`, `-bt` and `-b` use;
- no stack protector, stack clash probes, FORTIFY or CET, a plain static binary (no PIE), no RELRO, code and read-only data in one segment, and the unwind tables of the BSD libcs dropped at link time (160K on FreeBSD);
- Cosmopolitan's `-mtiny` runtime, 217K smaller, which drops `--strace`, `--ftrace`, malloc's cookies and some checks of API misuse.

`SECURE=1` builds the mitigations back: `-fstack-protector-strong`, `-fstack-clash-protection`, `_FORTIFY_SOURCE=2`, `-fcf-protection` on x86, static-PIE (with packed relative relocations on x86_64, i686 and aarch64: DT_RELR, musl 1.2.4 or later), full RELRO, separate code pages and Cosmopolitan's full runtime. Run `make clean` when switching: the vendored builds do not track it.

On x86_64, with `LIBC=vendor`: 393K small, 422K with `SECURE=1`; Cosmopolitan 707K and 919K.

**Native (FreeBSD, NetBSD, OpenBSD):** `gmake native` runs the same build with the system compiler; `gmake check-native` runs the checks. Their static libcs weigh more than musl: FreeBSD's jemalloc alone is 73K.

**Cosmopolitan:** a single ELF of about 710K that runs on Linux, FreeBSD and NetBSD without emulation: the libc handles the syscall differences. It is built with `x86_64-unknown-cosmo-cc`, because TinyCC generates x86_64 code. The `assimilate -b` step turns the APE output into a plain ELF and keeps the FreeBSD brand, without which FreeBSD refuses it. OpenBSD 7.5 and later only accept system calls from the system libc (pinsyscalls), which no Cosmopolitan binary can run under. `make check-cosmo` runs the checks with this toolchain.

Two Cosmopolitan pitfalls shape the code:

- Some string functions (`strstr`, `strcpy`...) are IFUNCs: taking their address breaks them for the whole program. Plugins get small wrappers, never libc addresses.
- `sscanf` does not support scansets such as `%*1[T ]`: the date parser is written by hand.

`patches/tcc-cosmo.patch` makes three changes to TinyCC for Cosmopolitan:

1. **NULL guard in `tcc_split_path()`:** Cosmopolitan does not set `tcc_lib_path` by default, and dereferencing it crashes.
2. **`memcpy` instead of `strcpy` for PLT names:** Cosmopolitan's `strcpy` uses SSE instructions that crash on some small stack-aligned buffers.
3. **Skip empty section names:** after dot-stripping, `tcc_add_linker_symbols()` generates duplicate symbols for them.

## Vendored sources

`vendor.sh` fetches pinned revisions: a commit for BearSSL, TinyCC and smolBSD, a commit plus SHA-256 for cJSON, a version plus SHA-256 for the cosmocc toolchain. Anyone can push to TinyCC's `mob` branch, so nothing is built from an unpinned head. To upgrade a library, change its pin, delete `vendor/<name>` and rebuild.

| Component | Project | Role |
|-----------|---------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) by Thomas Pornin | HTTPS to LLM APIs, IRC over TLS |
| C compiler | [TinyCC](https://bellard.org/tcc/) by Fabrice Bellard | In-memory plugin compilation |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble | LLM API payloads |
| C library | [musl](https://musl.libc.org/) or [Cosmopolitan](https://justine.lol/cosmopolitan/) | Static linking |

Everything else (HTTP client, IRC client, INI parser, TUI, scheduler, memory, plugin scanner) is written from scratch: about 7,500 lines of C.
