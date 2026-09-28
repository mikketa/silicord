# Roadmap

Every milestone ships a working `.exe`. Binary size, RAM and idle CPU are measured at each step.

## 0. Foundations ✅

- [x] CMake + MSVC + NASM build, no C runtime
- [x] First assembly routine (`sc_strlen`)
- [x] Console banner
- [x] GitHub Actions CI

## 1. Connection ✅

- [x] Token storage in the Windows Credential Manager (`CredWriteW` / `CredReadW`)
- [x] HTTPS REST requests with WinHTTP (`GET /users/@me`)
- [x] Allocation-free JSON reader (values are slices of the source buffer)
- [x] QR code login through the mobile app (passkeys, 2FA and SMS are handled on the phone)
- [x] WebSocket gateway (`WinHttpWebSocket*`): Hello, Identify, Heartbeat, Ready
- [x] Automatic reconnect with backoff, session resume, fresh identify when the session is gone

## 2. Text client (console)

- [x] List servers and channels, hiding channels you cannot see
- [x] Direct messages and group DMs on the home screen, most recent first
- [x] Live changes: new conversations, channels and servers created, renamed, moved or removed
- [x] Live messages in the open channel (`MESSAGE_CREATE`, `MESSAGE_UPDATE`, `MESSAGE_DELETE`)
- [x] Send a message
- [x] Load history, and older messages while scrolling up (`GET /channels/{id}/messages`)

## 3. Efficiency

- [x] Gateway `zlib-stream` compression (own inflater)
- [ ] Local cache for channels and users
- [x] Disk cache for CDN images, memory budget for decoded images
- [x] Memory counters in `--debug` (Silicord's allocations against the whole process)
- [x] Metrics: idle CPU, bytes received per hour (`--debug` and the About screen)

## 4. Win32 UI 🚧

- [x] Native, custom-drawn window with dark title bar and per-monitor DPI
- [x] Login screen with the QR code
- [x] Server rail with round icons, channel list with collapsible categories, user panel
- [x] Message view (grouping, dates, replies, mentions) and composer
- [x] Software renderer on DirectWrite (no Direct2D, no GPU), color emoji everywhere, drawn in bands
- [x] Discord markdown: bold, italic, underline, strike, code, code blocks, quotes, headings, lists, spoilers, links
- [x] Replies, edit (also with the up arrow) and delete with confirmation, hover toolbar
- [x] Images, files, link previews and bot embeds, stickers, custom emoji, jumbo emoji, "(edited)"
- [x] Reactions: counts, adding and removing, live updates
- [x] Emoji picker with search, categories and the server's emoji; `:name:` becomes the emoji on send
- [x] File uploads: plus button and drag and drop
- [x] Typing indicator, both ways
- [x] Member list with role groups, statuses, activities and bot tags; role colors on names
- [x] Statuses in direct messages, and picking our own (online, idle, do not disturb, invisible)
- [x] Mention, channel and emoji autocomplete in the composer
- [x] Threads, forums, pins, search, jumping to a message
- [x] Friends list: online, all, pending, blocked, adding friends
- [x] Server folders, right-click menus, quick switcher (Ctrl+K), keyboard shortcuts
- [x] Polls: results and voting
- [x] GIF picker (Discord's GIF search) and sticker picker
- [x] Pasting pictures and files into the composer
- [x] Slash commands: suggestions, subcommands and options, answers shown as in Discord
- [x] Bots' buttons and select menus, components v2 text
- [x] Mentions inbox, role names and `<t:...>` timestamps in messages
- [x] Unread markers, mention badges, read sync with other devices
- [x] Windows notifications for DMs, mentions and role mentions
- [x] Profile popouts: banner, avatar decoration, display name fonts and effects, server tag, badges, mutual friends and servers, bio, message box
- [x] Profile popouts: roles
- [x] Profile popouts: animated avatars, banners and decorations (own APNG player)
- [x] Executable icon from `assets/logo.svg`, sharp at every scale

## 5. Later

- [x] Animated GIFs and emoji
- [x] Settings screen: status, custom status, developer mode, notifications, about
- [x] Notification settings per server, category and channel, timed mutes
- [ ] Themes
- [ ] Voice (Opus + encryption), the biggest piece
