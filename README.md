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

## Download

Get `silicord.exe` from the [latest release](https://github.com/mikketa/silicord/releases/latest). It is a single portable executable: no installer, no dependencies. Release builds are made by GitHub Actions from the tagged commit; see the [Code Signing Policy](#code-signing-policy).

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

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

If Smart App Control is enabled, Windows blocks unsigned executables, including the ones you build yourself and the CI artifacts. Use a signed [release](https://github.com/mikketa/silicord/releases), or test local builds on a machine without Smart App Control.

## Layout

```
src/     C sources (Win32, no CRT)
asm/     x64 assembly routines (NASM, Win64 ABI)
tests/   unit tests (same no-CRT setup); tests/host: Linux build with sanitizers and a fuzzer
assets/  logo, icon and banners
tools/   generators: the icon (make_icon.py), Discord's emoji names (make_emoji_aliases.py)
docs/    roadmap and architecture
```

## Privacy

Silicord has no telemetry, analytics or crash reporting, and no server of its own. It only talks to the services needed to show your account:

- Discord: `discord.com` (API), `gateway.discord.gg` (events), `remote-auth-gateway.discord.gg` (QR login), `cdn.discordapp.com` and `*.discordapp.net` (images and files), and the voice servers Discord assigns when you join a voice channel or a screen share, which receive your microphone while you are in the channel and not muted, your camera or screen only while you share them, and the sound other programs play only while you share your screen with "Share sound" on (Silicord's own sound is left out)
- `media.tenor.com` and `static.klipy.com`: GIFs shown in messages and in the GIF picker
- `raw.githubusercontent.com`: the [Google Fonts repository](https://github.com/google/fonts), at a pinned commit, when a profile uses a display name font not downloaded yet

Every request carries a `Silicord/<version>` user agent with a link to this repository. Links in messages open in your default browser only when you click them.

On your computer, Silicord keeps:

- your token, in the Windows Credential Manager (`silicord/token`)
- its settings, image cache and fonts, in `%LOCALAPPDATA%\Silicord`
- pasted pictures waiting to be sent, in `%TEMP%\Silicord-paste`, and with `--debug` the log `%TEMP%\silicord-debug.log`

It writes nothing to the registry, adds no startup entry, file association or shell extension, and installs no service. The tray icon only exists while Silicord runs.

## Uninstalling

1. Log out with the power button next to your name, which removes the token from the Credential Manager. Without logging in again, `cmdkey /delete:silicord/token` does the same.
2. Close Silicord, including from the tray icon.
3. Delete `silicord.exe`, the `%LOCALAPPDATA%\Silicord` folder, and if present `%TEMP%\Silicord-paste` and `%TEMP%\silicord-debug.log`.

## Code Signing Policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

Only `silicord.exe` is signed, as built by the [release workflow](.github/workflows/release.yml) on GitHub Actions from a tagged commit of this repository. It contains no third-party binaries.

Team roles:

- Committers and reviewers: [mikketa](https://github.com/mikketa)
- Approvers: [mikketa](https://github.com/mikketa)

Changes from anyone else come as pull requests and are reviewed by a committer before being merged. Every signing request is approved by hand.

Privacy: see [Privacy](#privacy). Silicord sends nothing to other networked systems beyond the services listed there, which it needs to show your account.

Third-party components, included as data in the source:

- emoji short names from [gemoji](https://github.com/github/gemoji) (MIT License, Copyright (c) 2019 GitHub, Inc.), in `src/emoji_data.c`
- Discord's emoji names from [discord-emoji](https://github.com/xCykrix/discord_emoji) (MIT License, Copyright (c) 2020 Samuel Voeller, Copyright (c) 2016-2020 Marek Kulik), in `src/emoji_alias.c`

Display name fonts are not shipped: they are downloaded at run time from the Google Fonts repository and are under the SIL Open Font License.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commits follow [Conventional Commits](https://www.conventionalcommits.org).

## License

[MIT](LICENSE)
