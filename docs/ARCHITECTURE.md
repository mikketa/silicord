# Architecture

## Principles

1. **No C runtime.** The entry point is `entry` (`/ENTRY:entry /NODEFAULTLIB`). Only Windows libraries are linked (`kernel32`, `user32`, `winhttp`, `d2d1`, `dwrite`, `windowscodecs`, ...). What the compiler itself needs (`memset`, `memcpy`) lives in `src/rt.c`.
2. **Windows does the heavy lifting.** TLS, HTTP and WebSocket go through WinHTTP. No OpenSSL, no libcurl. Drawing goes through Direct2D (software rasterizer, so no GPU driver is loaded) and DirectWrite; images are decoded by WIC.
3. **Event-driven.** No busy loop: the thread blocks (`WaitForMultipleObjects`, `GetMessage`) until the next event.
4. **Assembly where it measurably helps.** C first, then hot functions (JSON parsing, inflate, text) are rewritten in NASM only when a benchmark shows a gain.

## Data flow

```
             REST (HTTPS)
  UI  <--->  client  <------------->  discord.com/api/v10
  Win32      state /    Gateway (WSS)
             cache   <------------->  gateway.discord.gg
                        Images (HTTPS)
                     <------------->  cdn.discordapp.com
                        Fonts (HTTPS, once)
                     <------------->  raw.githubusercontent.com
```

Images are decoded at the size they are shown and held once, on the render target. The UI keeps up to 16 MB of them in memory, dropping the least recently drawn ones first; the files themselves stay in a disk cache, so an image seen before is decoded in the same frame (up to 8 ms per paint, the rest in the background).

Display name fonts are the only files fetched outside Discord: they come from a pinned commit of the Google Fonts repository and are cached in `%LOCALAPPDATA%\Silicord\fonts`.

## Conventions

- C11, one module per `src/<module>.c` + `src/<module>.h`, functions prefixed by module (`con_`, `http_`, `gw_`, `json_`).
- Assembly: NASM, Win64 ABI (arguments in `rcx`, `rdx`, `r8`, `r9`, result in `rax`), `sc_` prefix, declarations in `src/sc_asm.h`.
- UTF-8 strings everywhere; conversion to UTF-16 only at Win32 `W` calls.
- `src/render.cpp` is the only C++ file: the Direct2D and DirectWrite headers are C++ only. It is built without exceptions, RTTI or static constructors, and exposes a C API (`render.h`).
