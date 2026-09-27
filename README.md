<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/banner-dark.svg">
    <img src="assets/banner-light.svg" alt="silicord" width="640">
  </picture>
</p>

A native Discord client for Windows, written in C and x64 assembly. No embedded browser, no C runtime: just Win32, WinHTTP and a few kilobytes.

> **Status: early.** Silicord logs in (QR code or token) and lists your servers. Messages come next. See the [roadmap](docs/ROADMAP.md).

## Why

The official client ships a full Chromium: several processes, hundreds of MB of RAM and CPU usage even when idle. Silicord aims for the opposite:

- a single `.exe` of a few dozen KB, nothing to install
- ~0% CPU when idle (the process sleeps until the next network event)
- a few MB of RAM

Measured on the login screen: 54 KB executable, 0.01% CPU and 3.3 MB of private memory at idle.
- a codebase small enough to read end to end

## ⚠️ Disclaimer

Silicord logs in with a **user account**. Discord's [Terms of Service](https://discord.com/terms) forbid third-party clients, and using one may get your account suspended. Use it at your own risk, try it on a secondary account first, and do not automate your account.

Your token grants full access to your account. Silicord stores it in the Windows Credential Manager (encrypted by Windows), never in a plain-text file. Never paste it anywhere else or share it.

Silicord is not affiliated with or endorsed by Discord Inc.

## Usage

Run `silicord.exe`, then either:

- **scan the QR code** with the Discord mobile app (Settings › Scan QR Code) and confirm on your phone. Passkeys, two-factor codes and SMS checks all happen on the phone, so every account type works;
- or choose **Use a token instead** and paste a token.

The token is checked with Discord, then stored in the Windows Credential Manager. **Log out** removes it.

`silicord --debug` also opens a console with a connection log.

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
