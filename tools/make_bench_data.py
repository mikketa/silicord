"""Writes the inputs of the bench: a synthetic READY shaped like Discord's, a
batch of messages, and a gateway session compressed as zlib-stream
(gateway.z: each websocket message as a 4-byte little-endian length and its bytes).

    python tools/make_bench_data.py OUT_DIR

The data is random but deterministic (fixed seed). Sizes are those of an account
with about 25 servers: READY around 3 MB, as Discord sends it.
"""
import json
import os
import random
import struct
import sys
import zlib

R = random.Random(1234)
WORDS = ("the of and to in is you that it he was for on are as with his they at be this have from or one had by "
         "word but not what all were we when your can said there use an each which she do how their if will up "
         "other about out many then them these so some her would make like him into time has look two more write "
         "go see number no way could people my than first water been call who oil its now find long down day did "
         "get come made may part discord server voice channel stream game emoji ping role").split()
EMOJI = ["\U0001F600", "\U0001F525", "❤️", "\U0001F44D", "\U0001F602", "\U0001F914"]
_next_id = [1000000000000000000]


def snowflake():
    _next_id[0] += R.randrange(1, 1 << 22)
    return str(_next_id[0])


def hexhash():
    return "%032x" % R.getrandbits(128)


def words(lo, hi):
    return " ".join(R.choice(WORDS) for _ in range(R.randint(lo, hi)))


def user(uid=None):
    return {"id": uid or snowflake(), "username": R.choice(WORDS) + str(R.randint(1, 9999)),
            "global_name": words(1, 2).title() if R.random() < 0.7 else None,
            "avatar": hexhash() if R.random() < 0.8 else None, "discriminator": "0",
            "public_flags": R.choice([0, 64, 128, 256]), "avatar_decoration_data": None,
            "primary_guild": None, "clan": None, "collectibles": None, "display_name_styles": None}


def overwrites(roles):
    return [{"id": R.choice(roles)["id"], "type": 0, "allow": str(R.choice([0, 1024, 3072])),
             "deny": str(R.choice([0, 1024, 2048]))} for _ in range(R.randint(0, 4))]


def guild(me):
    gid = snowflake()
    roles = [{"id": gid, "name": "@everyone", "permissions": "1071698660929", "position": 0, "color": 0,
              "hoist": False, "managed": False, "mentionable": False, "icon": None, "unicode_emoji": None,
              "flags": 0, "description": None, "tags": {}, "colors": {"primary_color": 0, "secondary_color": None,
                                                                     "tertiary_color": None}}]
    for k in range(R.randint(10, 60)):
        roles.append({"id": snowflake(), "name": words(1, 3), "permissions": str(R.getrandbits(40)),
                      "position": k + 1, "color": R.getrandbits(24), "hoist": R.random() < 0.3,
                      "managed": False, "mentionable": R.random() < 0.5, "icon": None, "unicode_emoji": None,
                      "flags": 0, "description": None, "tags": {},
                      "colors": {"primary_color": R.getrandbits(24), "secondary_color": None,
                                 "tertiary_color": None}})
    channels = []
    for c in range(R.randint(3, 8)):
        cat = {"id": snowflake(), "type": 4, "name": words(1, 2).upper(), "position": c, "flags": 0,
               "permission_overwrites": overwrites(roles)}
        channels.append(cat)
        for k in range(R.randint(3, 12)):
            t = R.choice([0, 0, 0, 0, 2, 5, 15])
            ch = {"id": snowflake(), "type": t, "name": "-".join(words(1, 3).split()), "position": k,
                  "parent_id": cat["id"], "flags": 0, "permission_overwrites": overwrites(roles),
                  "last_message_id": snowflake(), "rate_limit_per_user": 0, "nsfw": False,
                  "topic": words(3, 25) if R.random() < 0.5 else None, "version": R.getrandbits(40)}
            if t == 2:
                ch.update({"bitrate": 64000, "user_limit": 0, "rtc_region": None})
            if t == 15:
                ch.update({"available_tags": [{"id": snowflake(), "name": words(1, 1), "moderated": False,
                                               "emoji_id": None, "emoji_name": R.choice(EMOJI)}
                                              for _ in range(R.randint(0, 6))], "default_reaction_emoji": None})
            channels.append(ch)
    emojis = [{"id": snowflake(), "name": R.choice(WORDS) + str(k), "roles": [], "require_colons": True,
               "managed": False, "animated": R.random() < 0.2, "available": True}
              for k in range(R.randint(0, 120))]
    stickers = [{"id": snowflake(), "name": words(1, 2), "tags": R.choice(WORDS), "type": 2, "format_type": 1,
                 "description": words(2, 8), "asset": "", "available": True, "guild_id": gid}
                for _ in range(R.randint(0, 20))]
    threads = [{"id": snowflake(), "type": 11, "name": words(2, 6), "parent_id": R.choice(channels)["id"],
                "owner_id": snowflake(), "last_message_id": snowflake(), "message_count": R.randint(1, 500),
                "member_count": R.randint(1, 50), "thread_metadata": {"archived": False,
                                                                       "auto_archive_duration": 4320,
                                                                       "archive_timestamp": "2026-10-01T10:00:00+00:00",
                                                                       "locked": False}, "flags": 0}
               for _ in range(R.randint(0, 10))]
    return {"id": gid, "properties": {"id": gid, "name": words(1, 4).title(), "icon": hexhash(),
                                      "owner_id": snowflake(), "description": words(5, 20),
                                      "features": ["COMMUNITY", "NEWS", "ANIMATED_ICON", "INVITE_SPLASH"],
                                      "banner": hexhash(), "splash": None, "premium_tier": R.randint(0, 3),
                                      "preferred_locale": "en-US", "verification_level": 1,
                                      "system_channel_id": channels[1]["id"], "afk_timeout": 300},
            "name": None, "member_count": R.randint(50, 200000), "joined_at": "2024-01-01T00:00:00+00:00",
            "lazy": True, "large": True, "roles": roles, "channels": channels, "emojis": emojis,
            "stickers": stickers, "threads": threads, "stage_instances": [], "guild_scheduled_events": [],
            "data_mode": "full", "version": R.getrandbits(40), "premium_subscription_count": R.randint(0, 30),
            "application_command_counts": {"1": R.randint(0, 40)},
            "members": [{"user": {"id": me}, "roles": [R.choice(roles)["id"] for _ in range(R.randint(0, 4))],
                         "nick": None, "joined_at": "2024-01-01T00:00:00+00:00", "flags": 0}]}


def ready():
    me = snowflake()
    guilds = [guild(me) for _ in range(25)]
    users = [user() for _ in range(400)]
    private = []
    for _ in range(150):
        rec = R.sample(users, 1 if R.random() < 0.85 else R.randint(2, 8))
        private.append({"id": snowflake(), "type": 1 if len(rec) == 1 else 3, "flags": 0,
                        "last_message_id": snowflake(), "recipient_ids": [u["id"] for u in rec],
                        "recipients": rec, "name": None if len(rec) == 1 else words(1, 3),
                        "icon": None, "is_spam": False})
    all_channels = [c for g in guilds for c in g["channels"]] + private
    read_state = [{"id": c["id"], "last_message_id": c.get("last_message_id", snowflake()),
                   "mention_count": R.choice([0, 0, 0, 1, 3]), "last_pin_timestamp": "1970-01-01T00:00:00+00:00",
                   "flags": 0, "last_viewed": R.randint(1000, 4000)}
                  for c in R.sample(all_channels, len(all_channels) * 3 // 4)]
    ugs = [{"guild_id": g["id"], "muted": R.random() < 0.2, "message_notifications": R.choice([0, 1, 2]),
            "suppress_everyone": R.random() < 0.3, "suppress_roles": False, "mobile_push": True, "flags": 0,
            "hide_muted_channels": False, "mute_config": None, "notify_highlights": 0, "version": 1,
            "channel_overrides": [{"channel_id": c["id"], "muted": R.random() < 0.5, "message_notifications": 3,
                                   "collapsed": False, "mute_config": None, "flags": 4096}
                                  for c in R.sample(g["channels"], min(len(g["channels"]), R.randint(0, 8)))]}
           for g in guilds]
    folders = [{"id": R.randint(1, 1 << 30), "name": words(1, 2), "color": R.getrandbits(24),
                "guild_ids": [g["id"] for g in guilds[k:k + 3]]} for k in range(0, 9, 3)]
    folders += [{"id": None, "guild_ids": [g["id"]]} for g in guilds[9:]]
    return {"v": 9, "user": dict(user(me), email=None, verified=True, premium_type=2, mfa_enabled=True,
                                 bio=words(5, 30), phone=None, nsfw_allowed=True),
            "users": users, "guilds": guilds, "private_channels": private,
            "relationships": [{"id": u["id"], "type": R.choice([1, 1, 1, 2, 3]), "nickname": None,
                               "user_id": u["id"], "since": "2023-05-05T00:00:00+00:00"} for u in users[:200]],
            "read_state": {"entries": read_state, "partial": False, "version": 1234},
            "user_guild_settings": {"entries": ugs, "partial": False, "version": 99},
            "user_settings": {"status": "online", "developer_mode": True, "theme": "dark", "locale": "en-US",
                              "custom_status": {"text": words(2, 5), "emoji_name": EMOJI[1]},
                              "guild_folders": folders, "guild_positions": [g["id"] for g in guilds]},
            "sessions": [{"session_id": hexhash(), "status": "online", "activities": [],
                          "client_info": {"client": "web", "os": "windows", "version": 0}}],
            "session_id": hexhash(), "session_type": "normal", "resume_gateway_url": "wss://gateway-us-east1-b.discord.gg",
            "geo_ordered_rtc_regions": ["us-east", "us-central", "atlanta", "newark", "us-south"],
            "experiments": [[R.getrandbits(32), 0, R.randint(0, 3), -1, 0, 0, 0] for _ in range(300)],
            "guild_experiments": [[R.getrandbits(32), None, R.getrandbits(32),
                                   [[R.randint(0, 3), [[R.randint(0, 9999), R.randint(0, 9999)]]]],
                                   [], [], 0, 0] for _ in range(80)],
            "merged_members": [[{"user_id": me, "roles": [], "nick": None, "flags": 0}] for _ in guilds],
            "presences": [], "connected_accounts": [], "consents": {"personalization": {"consented": True}},
            "auth_session_id_hash": hexhash(), "analytics_token": hexhash() * 4, "country_code": "FR"}


def markdown():
    parts = [words(3, 30)]
    r = R.random()
    if r < 0.15:
        parts.append("**" + words(1, 4) + "**")
    if r < 0.1:
        parts.append("`" + words(1, 3) + "`")
    if R.random() < 0.08:
        parts.append("```c\nint main(void)\n{\n    return " + str(R.randint(0, 9)) + ";\n}\n```")
    if R.random() < 0.1:
        parts.append("<@" + snowflake() + ">")
    if R.random() < 0.1:
        parts.append("https://example.com/" + R.choice(WORDS))
    if R.random() < 0.1:
        parts.append("> " + words(3, 10))
    if R.random() < 0.15:
        parts.append(R.choice(EMOJI) + " <:" + R.choice(WORDS) + ":" + snowflake() + ">")
    if R.random() < 0.05:
        parts.append("||" + words(2, 5) + "||")
    R.shuffle(parts)
    return " ".join(parts)


def message(cid, authors):
    a = R.choice(authors)
    m = {"id": snowflake(), "type": 0, "content": markdown(), "channel_id": cid, "author": a,
         "attachments": [], "embeds": [], "mentions": [], "mention_roles": [], "pinned": False,
         "mention_everyone": False, "tts": False, "timestamp": "2026-10-20T12:34:56.789000+00:00",
         "edited_timestamp": "2026-10-20T12:40:00.000000+00:00" if R.random() < 0.1 else None,
         "flags": 0, "components": [], "nonce": snowflake()}
    if R.random() < 0.15:
        m["attachments"].append({"id": snowflake(), "filename": R.choice(WORDS) + ".png", "size": R.randint(1000, 9000000),
                                 "url": "https://cdn.discordapp.com/attachments/%s/%s/x.png?ex=1&is=2&hm=%s" % (cid, snowflake(), hexhash()),
                                 "proxy_url": "https://media.discordapp.net/attachments/%s/%s/x.png" % (cid, snowflake()),
                                 "width": 1280, "height": 720, "content_type": "image/png", "placeholder": "abc",
                                 "placeholder_version": 1})
    if R.random() < 0.1:
        m["embeds"].append({"type": "rich", "url": "https://example.com/" + R.choice(WORDS), "title": words(2, 8),
                            "description": markdown(), "color": R.getrandbits(24),
                            "fields": [{"name": words(1, 3), "value": words(2, 10), "inline": True}
                                       for _ in range(R.randint(0, 4))],
                            "thumbnail": {"url": "https://example.com/t.png", "proxy_url": "https://images-ext-1.discordapp.net/x",
                                          "width": 80, "height": 80}})
    if R.random() < 0.2:
        m["reactions"] = [{"emoji": {"id": None, "name": R.choice(EMOJI)}, "count": R.randint(1, 30), "me": R.random() < 0.3,
                           "count_details": {"burst": 0, "normal": 1}, "burst_colors": [], "me_burst": False,
                           "burst_me": False, "burst_count": 0} for _ in range(R.randint(1, 4))]
    if R.random() < 0.1:
        m["type"] = 19
        m["message_reference"] = {"type": 0, "channel_id": cid, "message_id": snowflake()}
        m["referenced_message"] = dict(message(cid, authors), referenced_message=None)
    return m


def gateway_events(n):
    authors = [user() for _ in range(60)]
    cid, gid = snowflake(), snowflake()
    seq = 1
    for _ in range(n):
        r = R.random()
        if r < 0.45:
            t, d = "MESSAGE_CREATE", dict(message(cid, authors), guild_id=gid,
                                          member={"roles": [snowflake()], "nick": None, "joined_at": "2024-01-01T00:00:00+00:00"})
        elif r < 0.65:
            t, d = "TYPING_START", {"user_id": R.choice(authors)["id"], "timestamp": 1760000000 + seq,
                                    "channel_id": cid, "guild_id": gid}
        elif r < 0.85:
            t, d = "PRESENCE_UPDATE", {"user": {"id": R.choice(authors)["id"]}, "status": R.choice(["online", "idle", "dnd"]),
                                       "client_status": {"desktop": "online"}, "guild_id": gid,
                                       "activities": [{"type": 0, "name": words(1, 3), "id": hexhash()[:16],
                                                       "created_at": 1760000000000, "timestamps": {"start": 1760000000000}}]}
        elif r < 0.95:
            t, d = "MESSAGE_REACTION_ADD", {"user_id": R.choice(authors)["id"], "channel_id": cid, "message_id": snowflake(),
                                            "guild_id": gid, "emoji": {"id": None, "name": R.choice(EMOJI)}, "burst": False,
                                            "type": 0}
        else:
            t, d = "MESSAGE_UPDATE", dict(message(cid, authors), guild_id=gid)
        yield {"t": t, "s": seq, "op": 0, "d": d}
        seq += 1


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    z = zlib.compressobj()
    gw = bytearray()
    rd = json.dumps({"t": "READY", "s": 1, "op": 0, "d": ready()}, separators=(",", ":"), ensure_ascii=False).encode()
    with open(os.path.join(out, "ready.json"), "wb") as f:
        f.write(rd)
    # Each websocket message: its length (4 bytes, little endian), then its compressed bytes.
    for doc in [rd] + [json.dumps(ev, separators=(",", ":"), ensure_ascii=False).encode()
                       for ev in gateway_events(2000)]:
        part = z.compress(doc) + z.flush(zlib.Z_SYNC_FLUSH)
        gw += struct.pack("<I", len(part)) + part
    with open(os.path.join(out, "gateway.z"), "wb") as f:
        f.write(gw)
    authors = [user() for _ in range(30)]
    cid = snowflake()
    with open(os.path.join(out, "messages.json"), "wb") as f:
        f.write(json.dumps([message(cid, authors) for _ in range(50)], separators=(",", ":"),
                           ensure_ascii=False).encode())
    print("ready.json %d bytes, gateway.z %d bytes" % (len(rd), len(gw)))


if __name__ == "__main__":
    main()
