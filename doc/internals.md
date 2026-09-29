# Internals

Technical details for contributors and the curious.

## How the event loop works

The daemon runs a single `poll()` loop that watches the IRC socket, the Unix domain socket (CLI and TUI) and the attached TUI clients, with a one-second tick. IRC is optional; when its link drops, the loop keeps serving the socket and reconnects in the background: 5 s after a loss, then doubling up to 5 min as long as the server does not accept the registration.

Each trigger starts a session in its own thread. An agent runs one session at a time, with a 5-second gap between sessions:

- owner messages (IRC, TUI, CLI) wait in a queue until their agent is free;
- inter-agent mail and due tasks stay on disk until the agent is free, so nothing is lost or truncated while it works;
- plugin sources are rescanned every 5 seconds.

In a session, the agent builds a system prompt from its personality, facts, recent memories, schedule and peer list, then loops with the LLM provider until it answers without calling a tool (default limit: 100 turns).

## How the harness helps small models

Small models loop, invent tools and send broken arguments. The session loop catches these cases and answers with an error the model can act on:

- arguments that are not a JSON object, or that do not match the tool schema (missing parameter, wrong type, value outside an enum), get the tool's usage line; numbers and booleans sent as strings are accepted;
- an unknown tool gets the list of available tools;
- the third identical call (same tool, same arguments) is refused, and a session that keeps repeating itself is stopped;
- an empty reply gets one nudge;
- a text answer to another agent's request is sent back to that agent automatically, marked as an answer, so it never starts a reply loop;
- a due task is presented as "do it now", and a one-shot task in the past is refused (models used to reschedule a due reminder instead of delivering it);
- `schedule_task` takes `in_minutes`, which small models get right far more often than absolute dates.

With Anthropic, the assistant turns are replayed exactly as received (thinking blocks keep their signatures) and the history is append-only. The request enables automatic prompt caching, so each tool round only pays full price for what it adds. With OpenAI-compatible providers, `history_budget` shortens old tool outputs when a session outgrows a small context window. Both providers retry connection errors, 408, 409, 429 and 5xx twice, honouring `Retry-After`.

## How plugins get compiled

shclaw embeds [TinyCC](https://bellard.org/tcc/) (libtcc) as a library. When an agent calls `create_plugin`, the daemon:

1. Compiles the source once in a forked child, so a compiler crash on bad input, or a bad symbol, cannot take the daemon down
2. Compiles it in memory with `tcc_compile_string()` and relocates it with `tcc_relocate()`
3. Resolves `TC_PLUGIN_NAME`, `tc_execute`, the description and the optional schema with `tcc_get_symbol()`
4. Writes the `.c` source to `plugins/` only then: a failed attempt never replaces a working plugin
5. Runs it once with `test_input`, if given, and reports the output and the HTTP calls

No `.so` is ever written to disk. On restart, `plugin_scan()` recompiles all `.c` files. Changed files are detected by mtime, and deleted ones are unloaded, every 5 seconds.

Every plugin call runs in a forked child with a 180-second timeout.

## Cross-platform binary (Cosmopolitan)

The musl build produces a standard Linux static binary (~530K, hardened with static-PIE, RELRO, NX, stack protector). Works on x86_64, aarch64, and armv7l.

The [Cosmopolitan](https://justine.lol/cosmopolitan/) build produces a single ~970K ELF that runs on Linux, NetBSD, FreeBSD, and OpenBSD. We compile with `x86_64-unknown-cosmo-cc` (TCC generates x86_64 ELF relocations, so we need the x86_64-specific toolchain). The `assimilate` step converts the APE format to native ELF.

Two Cosmopolitan pitfalls shape the code:

- Some string functions (`strstr`, `strcpy`...) are IFUNCs: taking their address breaks them for the whole program. Plugins therefore receive small wrappers, never libc addresses.
- `sscanf` does not support scansets such as `%*1[T ]`: the date parser is written by hand.

`make check-cosmo` runs the checks with the Cosmopolitan build.

### TCC patches for Cosmopolitan

Three patches are applied automatically (`patches/tcc-cosmo.patch`):

1. **NULL guard in `tcc_split_path()`** -- Cosmopolitan doesn't set `tcc_lib_path` by default; dereferencing it crashes.
2. **`strcpy` to `memcpy` for PLT names** -- Cosmopolitan's `strcpy` uses SSE instructions that crash on certain small stack-aligned buffers.
3. **Skip empty section names** -- `tcc_add_linker_symbols()` generates duplicate symbols for sections with empty names after dot-stripping.

## Vendored sources

`vendor.sh` fetches pinned revisions: a commit for BearSSL, TinyCC and smolBSD, a commit plus SHA-256 for cJSON, a version plus SHA-256 for the cosmocc toolchain. TinyCC's `mob` branch accepts pushes from anyone, which is why nothing is built from an unpinned head. To upgrade one, change its pin, delete `vendor/<name>` and rebuild.

## What's inside

| Component | Project | Role |
|-----------|---------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) by Thomas Pornin | HTTPS to LLM APIs, IRC over TLS |
| C compiler | [TinyCC](https://bellard.org/tcc/) by Fabrice Bellard | In-memory plugin compilation |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble | LLM API payload parsing |
| C library | [musl](https://musl.libc.org/) or [Cosmopolitan](https://justine.lol/cosmopolitan/) | Static linking |

Everything else (HTTP client, IRC client, INI parser, TUI, scheduler, memory, plugin scanner) is written from scratch -- about 7000 lines of C across 20 source files.
