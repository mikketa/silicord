# Architecture

## Principles

1. **No C runtime.** The entry point is `entry` (`/ENTRY:entry /NODEFAULTLIB`). Only `kernel32`, `winhttp`, `user32`, `gdi32` and `advapi32` are called. What the compiler itself needs (`memset`, `memcpy`) lives in `src/rt.c`.
2. **Windows does the heavy lifting.** TLS, HTTP and WebSocket go through WinHTTP. No OpenSSL, no libcurl.
3. **Event-driven.** No busy loop: the thread blocks (`WaitForMultipleObjects`, `GetMessage`) until the next event.
4. **Assembly where it measurably helps.** C first, then hot functions (JSON parsing, inflate, text) are rewritten in NASM only when a benchmark shows a gain.

## Planned data flow

```
             REST (HTTPS)
  UI  <--->  client  <------------->  discord.com/api/v10
  Win32      state /    Gateway (WSS, zlib-stream)
             cache   <------------->  gateway.discord.gg
```

## Conventions

- C11, one module per `src/<module>.c` + `src/<module>.h`, functions prefixed by module (`con_`, `http_`, `gw_`, `json_`).
- Assembly: NASM, Win64 ABI (arguments in `rcx`, `rdx`, `r8`, `r9`, result in `rax`), `sc_` prefix, declarations in `src/sc_asm.h`.
- UTF-8 strings everywhere; conversion to UTF-16 only at Win32 `W` calls.
