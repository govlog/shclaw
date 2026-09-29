# Internals

Technical details for contributors and the curious.

## How the event loop works

The daemon runs a single `poll()` loop that watches the IRC socket, the Unix domain socket (CLI and TUI) and the attached TUI clients, with a one-second tick. IRC is optional; when its link drops, the loop keeps serving the socket and reconnects in the background: 5 s after a loss, then doubling up to 5 min as long as the server does not accept the registration.

Each trigger starts a session in its own thread. An agent runs one session at a time, with a 5-second gap between sessions:

- owner messages (IRC, TUI, CLI) wait in a queue until their agent is free;
- inter-agent mail and due tasks stay on disk until the agent is free, so nothing is lost or truncated while it works;
- plugin sources are rescanned every 5 seconds.

In a session, the agent sends a system prompt that only changes with its configuration (personality, rules, objectives, peer list, plugin template for the builder) and a first user turn with the session context (communication rules, facts, schedule, recent memories, time) followed by the trigger. It then loops with the LLM provider until it answers without calling a tool (default limit: 100 turns).

## How the harness helps small models

Small models loop, invent tools and send broken arguments. The session loop catches these cases and answers with an error the model can act on:

- arguments that are not a JSON object, or that do not match the tool schema (missing parameter, wrong type, value outside an enum), get the tool's usage line; numbers and booleans sent as strings are accepted;
- an unknown tool gets the list of available tools;
- the third identical call (same tool, same arguments) is refused, and a session that keeps repeating itself is stopped;
- an empty reply gets one nudge;
- a text answer to another agent's request is sent back to that agent automatically, marked as an answer, so it never starts a reply loop;
- a due task is presented as "do it now", and a one-shot task in the past is refused (models used to reschedule a due reminder instead of delivering it);
- `schedule_task` takes `in_minutes`, which small models get right far more often than absolute dates.

With Anthropic, the assistant turns are replayed exactly as received (thinking blocks keep their signatures) and the history is append-only. With OpenAI-compatible providers, `history_budget` shortens old tool outputs when a session outgrows a small context window. Both providers retry connection errors, 408, 409, 429 and 5xx twice, honouring `Retry-After`.

## How prompts stay cacheable

A provider reuses the work already done on a prompt prefix it has seen: Anthropic and OpenAI bill those tokens at a fraction of the price (often a tenth), and a local server (Ollama, llama.cpp) does not compute them again, which matters most on a CPU. Each request keeps that prefix as long as possible:

- the tools come first: built-in tools in a fixed order, then plugins sorted by name. The system prompt follows and holds nothing that changes between sessions;
- the first user turn goes from the most stable part to the most volatile: trigger type and communication rules, facts, schedule, recent memories, time, then the message. Before GPT-5.6, OpenAI reuses its cache only when few tokens follow the first difference with an earlier request (on gpt-4.1-nano: a hit with 50 tokens after it, a miss with 100), so the time and the message come last;
- the history only grows, except with `history_budget`: it shortens old tool outputs down to half the budget in one go, so the prefix changes rarely;
- Anthropic: a cache breakpoint closes the system prompt, so the next sessions reuse tools and system prompt; automatic caching covers the conversation. OpenAI: `prompt_cache_key` is `shclaw-<agent>`. Agents share the same tools at the head of the prompt, so without the key they share one routing bucket (about 15 requests per minute) and overflow to servers without their cache.

Each model call logs its usage, e.g. `[oracle] Tokens: 1430 in (1280 from cache, 0 to cache), 8 out`. A prefix under the provider minimum is never cached: 1024 tokens for OpenAI, 512 to 4096 for Claude depending on the model.

## How plugins get compiled

shclaw embeds [TinyCC](https://bellard.org/tcc/) (libtcc) as a library. When an agent calls `create_plugin`, the daemon:

1. Decodes the source once more if it arrives on one line with literal `\n` (small models sometimes escape the code twice)
2. Compiles the source once in a forked child, so a compiler crash on bad input, or a bad symbol, cannot take the daemon down
3. Compiles it in memory with `tcc_compile_string()` and relocates it with `tcc_relocate()`
4. Resolves `TC_PLUGIN_NAME`, `tc_execute`, the description and the optional schema with `tcc_get_symbol()`
5. Writes the `.c` source to `plugins/` only then: a failed attempt never replaces a working plugin
6. Runs it once with `test_input`, if given, and reports the output and the HTTP calls, with a note when a response did not fit the plugin's buffer

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
