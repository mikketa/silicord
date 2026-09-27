<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/banner-dark.svg">
    <img src="assets/banner-light.svg" alt="silicord" width="640">
  </picture>
</p>

A native Discord client for Windows, written in C and x64 assembly. No embedded browser, no C runtime: just Win32, WinHTTP, DirectWrite and a few kilobytes.

> **Status: early.** Silicord logs in with a QR code and covers everyday text chat: servers, channels and direct messages; messages with formatting, custom emoji, images, files, link previews, stickers and reactions; replies, edits and deletes; file uploads; the emoji picker; typing indicators; the member list with statuses and role colors; and user profiles with their banner, badges, avatar decoration and display name style. See the [roadmap](docs/ROADMAP.md).

## Why

The official client ships a full Chromium: several processes, hundreds of MB of RAM and CPU usage even when idle. Silicord aims for the opposite:

- a single `.exe` of a few dozen KB, nothing to install
- ~0% CPU when idle (the process sleeps until the next network event)
- a few MB of RAM

Measured while connected to an account with 20 servers: 195 KB executable, 0% CPU over 30 idle seconds, about 8 MB of memory at startup (Task Manager) and 9 MB after browsing eight servers. Silicord's own data, images included, stays around 3 MB.
- a codebase small enough to read end to end

## ⚠️ Disclaimer

Silicord logs in with a **user account**. Discord's [Terms of Service](https://discord.com/terms) forbid third-party clients, and using one may get your account suspended. Use it at your own risk, try it on a secondary account first, and do not automate your account.

Your token grants full access to your account. Silicord stores it in the Windows Credential Manager (encrypted by Windows), never in a plain-text file. Never paste it anywhere else or share it.

Silicord is not affiliated with or endorsed by Discord Inc.

## Usage

Run `silicord.exe` and scan the QR code with the Discord mobile app (Settings › Scan QR Code), then confirm on your phone. Passkeys, two-factor codes and SMS checks all happen on the phone, so every account type works.

The token Discord sends back is stored in the Windows Credential Manager. The power button next to your name logs out and removes it.

Images from Discord's CDN are kept in `%LOCALAPPDATA%\Silicord\images` (at most 64 MB, least recently used first out), so avatars and icons seen before show up in the same frame instead of being downloaded again.

Clicking an avatar or a name opens that user's profile. Display names with a custom font use free fonts (SIL Open Font License) that Silicord downloads once from the [Google Fonts repository](https://github.com/google/fonts), at a pinned commit, and keeps in `%LOCALAPPDATA%\Silicord\fonts`.

`silicord --debug` also opens a console with a connection log, mirrored to `%TEMP%\silicord-debug.log`. It includes a `[mem]` line each time a channel opens: memory allocated by Silicord itself (and its images) against the whole process.

## Building

Requirements:

- [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) with the "Desktop development with C++" workload
- [NASM](https://www.nasm.us/) on the `PATH`
- [CMake](https://cmake.org/) 3.20+ and Ninja (shipped with the Build Tools)

From an "x64 Native Tools Command Prompt":

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

CI builds every push, runs the tests and publishes `silicord.exe` as an artifact.

If Smart App Control is enabled, Windows blocks unsigned executables you build yourself. Use the CI artifact instead.

## Layout

```
src/     C sources (Win32, no CRT)
asm/     x64 assembly routines (NASM, Win64 ABI)
tests/   unit tests (same no-CRT setup)
assets/  logo and banners
docs/    roadmap and architecture
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commits follow [Conventional Commits](https://www.conventionalcommits.org).

## License

[MIT](LICENSE)
