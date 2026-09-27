# Roadmap

Every milestone ships a working `.exe`. Binary size, RAM and idle CPU are measured at each step.

## 0. Foundations ✅

- [x] CMake + MSVC + NASM build, no C runtime
- [x] First assembly routine (`sc_strlen`)
- [x] Console banner
- [x] GitHub Actions CI

## 1. Connection

- [ ] Token storage in the Windows Credential Manager (`CredWriteW` / `CredReadW`)
- [ ] HTTPS REST requests with WinHTTP (`GET /users/@me`)
- [ ] Allocation-free JSON parser (tokenizer pointing into the source buffer)
- [ ] WebSocket gateway (`WinHttpWebSocket*`): Hello, Identify, Heartbeat, Ready
- [ ] Automatic resume and reconnect

## 2. Text client (console)

- [ ] List guilds, channels and DMs
- [ ] Live messages in a channel (`MESSAGE_CREATE`)
- [ ] Send a message
- [ ] Load history (`GET /channels/{id}/messages`)

## 3. Efficiency

- [ ] Gateway `zlib-stream` compression (custom inflate, assembly candidate)
- [ ] Local cache for channels and users
- [ ] Metrics: binary size, RAM, idle CPU, bytes received per hour

## 4. Win32 UI

- [ ] Native window: guild list, channel list, message view
- [ ] Text rendering with GDI / DirectWrite
- [ ] Mentions, replies, edit and delete
- [ ] Windows notifications
- [ ] Executable icon from `assets/logo.svg`

## 5. Later

- [ ] Images and emojis (lazy loading)
- [ ] Attachments
- [ ] Themes
- [ ] Voice (Opus + encryption), the biggest piece
