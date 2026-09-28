# Contributing

## Setup

```sh
git config core.hooksPath .githooks
```

This enables the `commit-msg` hook that checks every commit message.

## Commit messages

Silicord follows [Conventional Commits](https://www.conventionalcommits.org):

```
<type>(<scope>): <summary>

<body>
```

- **type**: `feat`, `fix`, `perf`, `refactor`, `docs`, `style`, `test`, `build`, `ci`, `chore`, `revert`
- **scope** (optional): the module touched, e.g. `gateway`, `http`, `json`, `asm`, `ui`
- **summary**: imperative mood, lowercase, no trailing period, 72 characters max for the whole line
- **breaking change**: add `!` after the type/scope and a `BREAKING CHANGE:` footer

Examples:

```
feat(gateway): send heartbeat at the interval given by hello
perf(asm): vectorize sc_strlen with sse2
fix(http): retry once on connection reset
```

## Code

- C11, no C runtime, Windows x64 only for now.
- One module per `src/<module>.c` + `.h`, functions prefixed by module (`con_`, `http_`, `gw_`, `json_`).
- Assembly in `asm/`, NASM syntax, Win64 ABI, symbols prefixed `sc_` and declared in `src/sc_asm.h`.
- Everything in the repository is written in English.
- Anything fed by Discord (JSON, markdown, images) must survive any input: add a unit test, and run the host tests and a fuzzer run (`tests/host`, see the README) before sending a change to the core.
- Formatting into fixed buffers (`wsprintfA`, `lstrcpynA`) needs a precision or a size that fits the worst case: names are up to 32 or 100 characters of up to 4 bytes each.

## Assets

`assets/silicord.ico` is generated: after changing `assets/logo.svg`, run `python tools/make_icon.py` and commit both.
