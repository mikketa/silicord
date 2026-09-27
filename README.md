<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/banner-dark.svg">
    <img src="assets/banner-light.svg" alt="silicord" width="640">
  </picture>
</p>

A native Discord client for Windows, written in C and x64 assembly. No embedded browser, no C runtime: just Win32, WinHTTP and a few kilobytes.

> **Status: very early.** The executable prints its banner and exits. See the [roadmap](docs/ROADMAP.md).

## Why

The official client ships a full Chromium: several processes, hundreds of MB of RAM and CPU usage even when idle. Silicord aims for the opposite:

- a single `.exe` of a few dozen KB, nothing to install
- ~0% CPU when idle (the process sleeps until the next network event)
- a few MB of RAM
- a codebase small enough to read end to end

## ⚠️ Disclaimer

Silicord logs in with a **user account**. Discord's [Terms of Service](https://discord.com/terms) forbid third-party clients, and using one may get your account suspended. Use it at your own risk, try it on a secondary account first, and do not automate your account.

Your token grants full access to your account. Silicord stores it in the Windows Credential Manager (encrypted by Windows), never in a plain-text file. Never paste it anywhere else or share it.

Silicord is not affiliated with or endorsed by Discord Inc.

## Building

Requirements:

- [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) with the "Desktop development with C++" workload
- [NASM](https://www.nasm.us/) on the `PATH`
- [CMake](https://cmake.org/) 3.20+ and Ninja (shipped with the Build Tools)

From an "x64 Native Tools Command Prompt":

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\silicord.exe
```

CI builds every push and publishes `silicord.exe` as an artifact.

## Layout

```
src/     C sources (Win32, no CRT)
asm/     x64 assembly routines (NASM, Win64 ABI)
assets/  logo and banners
docs/    roadmap and architecture
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Commits follow [Conventional Commits](https://www.conventionalcommits.org).

## License

[MIT](LICENSE)
