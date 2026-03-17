# Internals

Technical details for contributors and the curious.

## How the event loop works

The daemon runs a single `poll()` call watching two file descriptors: the IRC socket and the Unix domain socket (for TUI/CLI). A 5-second timer handles inbox delivery, task scheduling, and plugin directory scanning.

Each agent runs in its own pthread. A trigger (IRC mention, scheduled task, inter-agent message, CLI command) creates a session thread. The agent builds a system prompt from its personality, memory, schedule, and peer list, then enters a conversation loop with the LLM provider. Sessions have a configurable turn limit (default 100) and a 5-second cooldown between sessions.

## How plugins get compiled

shclaw embeds [TinyCC](https://bellard.org/tcc/) (libtcc) as a library. When an agent calls `create_plugin`, the daemon:

1. Writes the `.c` source to `plugins/`
2. Feeds it to `tcc_compile_string()`
3. Relocates it in-memory with `tcc_relocate()`
4. Resolves the entry point with `tcc_get_symbol()`

No `.so` is ever written to disk. The compiled code lives in the process address space and is callable immediately. On restart, `plugin_scan()` recompiles all `.c` files. Changed files are detected by mtime and recompiled every 5 seconds.

## Cross-platform binary (Cosmopolitan)

The musl build produces a standard Linux static binary (~528K, hardened with static-PIE, RELRO, NX, stack protector, FORTIFY_SOURCE). Works on x86_64, aarch64, and armv7l.

The [Cosmopolitan](https://justine.lol/cosmopolitan/) build produces a single ~968K ELF that runs on Linux, NetBSD, FreeBSD, and OpenBSD. We compile with `x86_64-unknown-cosmo-cc` (TCC generates x86_64 ELF relocations, so we need the x86_64-specific toolchain). The `assimilate` step converts the APE format to native ELF.

### TCC patches for Cosmopolitan

Three patches are applied automatically (`patches/tcc-cosmo.patch`):

1. **NULL guard in `tcc_split_path()`** -- Cosmopolitan doesn't set `tcc_lib_path` by default; dereferencing it crashes.
2. **`strcpy` to `memcpy` for PLT names** -- Cosmopolitan's `strcpy` uses SSE instructions that crash on certain small stack-aligned buffers.
3. **Skip empty section names** -- `tcc_add_linker_symbols()` generates duplicate symbols for sections with empty names after dot-stripping.

## What's inside

| Component | Project | Role |
|-----------|---------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) by Thomas Pornin | HTTPS to LLM APIs, IRC over TLS |
| C compiler | [TinyCC](https://bellard.org/tcc/) by Fabrice Bellard | In-memory plugin compilation |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble | LLM API payload parsing |
| C library | [musl](https://musl.libc.org/) or [Cosmopolitan](https://justine.lol/cosmopolitan/) | Static linking |

Everything else (HTTP client, IRC client, INI parser, TUI, scheduler, memory, plugin scanner) is written from scratch -- about 6000 lines of C across 20 source files.
