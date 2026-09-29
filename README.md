<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/banner-dark.svg">
    <img src="assets/banner-light.svg" alt="silicord" width="640">
  </picture>
</p>

A native Discord client for Windows, written in C and x64 assembly. No embedded browser, no C runtime: just Win32, WinHTTP, DirectWrite and a few kilobytes.

> **Status: early.** Silicord logs in with a QR code and covers everyday text chat:
>
> - servers (with their folders), channels, threads, forums and direct messages, the friends list
> - messages with formatting, custom and animated emoji, GIFs, images, files, link previews, stickers, polls, reactions, and bots' buttons and menus
> - replies, edits and deletes, uploads (plus button, drag and drop, pasting), the emoji, GIF and sticker picker, `@` `#` `:` and slash command suggestions
> - pins, search, jumping to a message, the quick switcher (Ctrl+K) and Discord's keyboard shortcuts
> - typing indicators, unread markers, the mentions inbox, notifications with Discord's per-server and per-channel settings, the member list with statuses and role colors, and user profiles with their (animated) banner, badges, roles, avatar decoration and display name style
> - a settings screen: status and custom status, developer mode, notification preferences
>
> Voice is being built: joining channels with Discord's end-to-end encryption (DAVE, over MLS) and an Opus codec written from scratch are in, and so are cameras and screen sharing (Go Live) with a VP8 codec of our own, but not yet tested against Discord's servers. See the [roadmap](docs/ROADMAP.md).

## Why

The official client ships a full Chromium: several processes, hundreds of MB of RAM and CPU usage even when idle. Silicord aims for the opposite:

- a single `.exe` of a few hundred KB, nothing to install
- ~0% CPU when idle (the process sleeps until the next network event)
- a few MB of RAM
- a codebase small enough to read end to end

Measured while connected to an account with 20 servers: 480 KB executable (the emoji table and its search names included), 0% CPU over 30 idle seconds, about 8 MB of memory at startup (Task Manager) and 9 MB after browsing eight servers. Silicord's own data, images included, stays around 3 MB.

## ⚠️ Disclaimer

Silicord logs in with a **user account**. Discord's [Terms of Service](https://discord.com/terms) forbid third-party clients, and using one may get your account suspended. Use it at your own risk, try it on a secondary account first, and do not automate your account.

Your token grants full access to your account. Silicord stores it in the Windows Credential Manager (encrypted by Windows), never in a plain-text file. Never paste it anywhere else or share it.

Silicord is not affiliated with or endorsed by Discord Inc.

## Usage

Run `silicord.exe` and scan the QR code with the Discord mobile app (Settings › Scan QR Code), then confirm on your phone. Passkeys, two-factor codes and SMS checks all happen on the phone, so every account type works.

The token Discord sends back is stored in the Windows Credential Manager. The power button next to your name logs out and removes it.

Images from Discord's CDN are kept in `%LOCALAPPDATA%\Silicord\images` (at most 64 MB, least recently used first out), so avatars and icons seen before show up in the same frame instead of being downloaded again.

Clicking an avatar or a name opens that user's profile. Display names with a custom font use free fonts (SIL Open Font License) that Silicord downloads once from the [Google Fonts repository](https://github.com/google/fonts), at a pinned commit, and keeps in `%LOCALAPPDATA%\Silicord\fonts`.

`silicord --debug` also opens a console with a connection log, mirrored to `%TEMP%\silicord-debug.log`. It includes `[mem]`, `[net]` and `[cpu]` lines each time a channel opens: memory allocated by Silicord itself (and its images) against the whole process, bytes received from the gateway, the API and the CDN (with the hourly rate), and CPU time since start. The About screen in the settings shows the same.

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

The platform-independent code (JSON, inflate, markdown, messages, the model, slash commands, APNG...) also builds on Linux, where the same tests and a fuzzer run under AddressSanitizer and UBSan. CI runs them on every push too:

```sh
cmake -S tests/host -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
build-host/fuzz 100000 json 42   # a longer run: iterations, target (all, json, command, text, inflate, apng), seed
```

If Smart App Control is enabled, Windows blocks unsigned executables you build yourself. Use the CI artifact instead.

## Layout

```
src/     C sources (Win32, no CRT)
asm/     x64 assembly routines (NASM, Win64 ABI)
tests/   unit tests (same no-CRT setup); tests/host: Linux build with sanitizers and a fuzzer
assets/  logo, icon and banners
tools/   generators: the icon (make_icon.py), Discord's emoji names (make_emoji_aliases.py)
docs/    roadmap and architecture
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commits follow [Conventional Commits](https://www.conventionalcommits.org).

## License

[MIT](LICENSE)
