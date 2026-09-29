# Architecture

## Principles

1. **No C runtime.** The entry point is `entry` (`/ENTRY:entry /NODEFAULTLIB`). Only Windows libraries are linked (`kernel32`, `user32`, `winhttp`, `dwrite`, `windowscodecs`, ...). What the compiler itself needs (`memset`, `memcpy`) lives in `src/rt.c`.
2. **Windows does the heavy lifting.** TLS, HTTP and WebSocket go through WinHTTP. No OpenSSL, no libcurl. DirectWrite lays out and draws the text, color emoji included, into a bitmap Silicord owns; images are decoded by WIC. Neither Direct2D nor the GPU is used.
3. **Event-driven.** No busy loop: the thread blocks (`WaitForMultipleObjects`, `GetMessage`) until the next event.
4. **Assembly where it measurably helps.** C first, then hot functions (JSON parsing, inflate, text) are rewritten in NASM only when a benchmark shows a gain.
5. **SIMD in the codecs.** The VP8 decoder and encoder run their hot loops (subpixel filters, loop filter, inverse transform, quantization) with SSE2 intrinsics, which every x64 CPU has. Each one gives the same bits as the scalar code it replaced: checked on random input against it, on the test vectors, and against ffmpeg.

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
- `src/render.cpp` is the only C++ file: the DirectWrite headers are C++ only. It is built without exceptions, RTTI or static constructors, and exposes a C API (`render.h`).

## Rendering

`render.cpp` is a small software renderer. DirectWrite draws glyph runs into the bitmap of an `IDWriteBitmapRenderTarget` (color emoji are split into their colored layers with `TranslateColorGlyphRun`); rounded rectangles, circles, gradients and scaled images are rasterized by hand with analytic anti-aliasing.

A frame is drawn in bands of 256 rows through one bitmap as wide as the window: the UI repeats its paint for each band and anything outside it is skipped before any work is done. The bitmap stays around 2 MB instead of a full-window back buffer, and there is no Direct2D device or WARP rasterizer, whose caches used to take 20 to 30 MB.

A Go Live stream travels over a voice connection of its own, next to the call's (`voice.c` keeps three: the call, the screen we share, the stream we watch). The gateway creates it (op 18) or joins it (op 20) and answers with `STREAM_CREATE` and `STREAM_SERVER_UPDATE`; the connection identifies with the call's voice session, the stream's server and token, and a `screen` stream, and its DAVE group is the stream's server id minus one. Sharing captures the monitor showing the window with DXGI Desktop Duplication (`screen.c`, the pointer drawn in), scales it to 720p and converts it to I420 (`picture.c`), then encodes it with our VP8 encoder at 15 frames a second.

Video from a call is converted from YUV at the size its tile shows it (`picture.c`: boxes of 2 or 4 pixels averaged in YUV first, then area or bilinear resampling in fixed point), on the thread that decodes it, so the UI draws each picture by copying it.

DirectWrite does not clip to a rectangle, so the pixels a text layout can touch outside the visible area are saved before drawing it and put back after; the same copy gives translucent text. Styled display names are drawn as a coverage mask (white glyphs, grayscale anti-aliasing) and composited with their gradient or effect.

## Testing

The unit tests in `tests/` are built like the client (no C runtime, `entry` as the entry point) and run by CTest on Windows. Most of them cover code that does not depend on Windows: the JSON reader, the inflater, markdown, messages, the model, slash commands, APNG, the member list. `tests/host` builds that code on Linux, with a small shim for the handful of Win32 calls it makes (`HeapAlloc`, `MultiByteToWideChar`, `lstrcpynA`...), and runs the same tests under AddressSanitizer and UBSan.

It also builds `fuzz`, a deterministic fuzzer for everything that parses data from Discord. JSON documents are either generated from the keys the code looks up (taken from the sources) or made by replacing values in the JSON of the unit tests, then given to every consumer: messages and batches, READY and the gateway events applied to the model, profiles, member list updates, slash commands. Markdown, search filters and emoji expansion get random text; the inflater's output is compared with zlib's; APNGs are corrupted. A sanitizer report or a broken invariant (a markdown span outside the text, invalid JSON from `cmd_build`) stops it.
