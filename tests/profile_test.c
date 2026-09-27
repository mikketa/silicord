/* Profile parsing: global fields, server overrides, styles, badges and mutuals. */
#include <windows.h>
#include "test.h"
#include "profile.h"

static const char k_full[] =
    "{\"user\":{\"id\":\"42\",\"username\":\"niok\",\"global_name\":\"Niok\",\"avatar\":\"a1\","
    "\"banner\":\"a_b1\",\"accent_color\":16711680,\"bio\":\"old\","
    "\"avatar_decoration_data\":{\"asset\":\"a_deco\",\"sku_id\":\"1\",\"expires_at\":null},"
    "\"display_name_styles\":{\"font_id\":8,\"effect_id\":2,\"colors\":[16738740,11141290]},"
    "\"primary_guild\":{\"identity_enabled\":true,\"identity_guild_id\":\"77\",\"tag\":\"CAT\",\"badge\":\"tb\"}},"
    "\"user_profile\":{\"bio\":\"sys/net admin\\nGNU/linux\",\"pronouns\":\"\",\"theme_colors\":[1,2],"
    "\"accent_color\":null},"
    "\"badges\":[{\"id\":\"hypesquad_house_1\",\"description\":\"HypeSquad Bravery\",\"icon\":\"8a88\"},"
    "{\"id\":\"no_icon\",\"description\":\"x\"},"
    "{\"id\":\"quest\",\"description\":\"Completed a Quest\",\"icon\":\"7d9a\",\"link\":\"https://discord.com/discovery/quests\"}],"
    "\"mutual_guilds\":[{\"id\":\"1\",\"nick\":null},{\"id\":\"2\"},{\"id\":\"3\"}],"
    "\"mutual_friends\":[{\"id\":\"5\",\"avatar\":\"f5\"},{\"id\":\"6\",\"avatar\":null}],"
    "\"mutual_friends_count\":2,"
    "\"guild_member\":{\"nick\":\"Server Niok\",\"avatar\":\"ga\",\"roles\":[]},"
    "\"guild_member_profile\":{\"bio\":\"\",\"pronouns\":\"they/them\",\"banner\":null}}";

static const char k_error[] = "{\"message\":\"Unknown User\",\"code\":10013}";

static const char k_bare[] =
    "{\"user\":{\"id\":\"9\",\"username\":\"plain\",\"global_name\":null,\"avatar\":null,\"banner\":null,"
    "\"primary_guild\":{\"identity_enabled\":false,\"tag\":\"NO\"}},\"badges\":[]}";

void entry(void)
{
    profile_t p;
    json_t root;

    check(json_parse(k_full, sizeof k_full - 1, &root) && profile_parse(root, NULL, &p), "full profile parses");
    check(lstrcmpA(p.id, "42") == 0, "id");
    check(str_eq(&p.username, "niok") && str_eq(&p.name, "Niok"), "username and global name");
    check(str_eq(&p.bio, "sys/net admin\nGNU/linux"), "user_profile bio wins over the user bio");
    check(p.pronouns.len == 0, "empty pronouns stay empty");
    check(lstrcmpA(p.avatar, "a1") == 0 && lstrcmpA(p.banner, "a_b1") == 0, "avatar and banner hashes");
    check(!p.member_avatar && !p.member_banner && !p.guild_id[0], "no server profile without a guild");
    check(p.has_accent && p.accent == 0xFF0000, "a null accent does not clear the user accent");
    check(p.ntheme == 2 && p.theme[0] == 1 && p.theme[1] == 2, "theme colors");
    check(lstrcmpA(p.decoration, "a_deco") == 0, "avatar decoration");
    check(p.font_id == 8 && p.effect_id == NAME_GRADIENT && p.ncolors == 2 && p.colors[0] == 0xFF69B4,
          "display name style");
    check(lstrcmpA(p.tag, "CAT") == 0 && lstrcmpA(p.tag_badge, "tb") == 0 && lstrcmpA(p.tag_guild, "77") == 0,
          "server tag");
    check(p.nbadges == 2, "badges without an icon are skipped");
    check(p.nbadges == 2 && lstrcmpA(p.badges[0].icon, "8a88") == 0 && str_eq(&p.badges[0].description, "HypeSquad Bravery") &&
              p.badges[0].link.len == 0,
          "first badge");
    check(p.nbadges == 2 && str_eq(&p.badges[1].link, "https://discord.com/discovery/quests"), "badge link");
    check(p.mutual_guilds == 3 && p.mutual_friends == 2, "mutual counts");
    check(p.nfriends == 2 && lstrcmpA(p.friends[0].avatar, "f5") == 0 && !p.friends[1].avatar[0], "mutual friends");
    profile_free(&p);

    check(profile_parse(root, "77", &p), "server profile parses");
    check(str_eq(&p.name, "Server Niok"), "nickname replaces the display name");
    check(lstrcmpA(p.avatar, "ga") == 0 && p.member_avatar, "server avatar");
    check(lstrcmpA(p.banner, "a_b1") == 0 && !p.member_banner, "null server banner keeps the global one");
    check(str_eq(&p.bio, "sys/net admin\nGNU/linux"), "empty server bio keeps the global one");
    check(str_eq(&p.pronouns, "they/them"), "server pronouns");
    check(lstrcmpA(p.guild_id, "77") == 0, "guild id kept");
    profile_free(&p);

    check(json_parse(k_bare, sizeof k_bare - 1, &root) && profile_parse(root, NULL, &p), "bare profile parses");
    check(str_eq(&p.name, "plain"), "username when there is no global name");
    check(!p.avatar[0] && !p.banner[0] && !p.decoration[0] && !p.has_accent && !p.ntheme, "no images or colors");
    check(!p.tag[0], "disabled server tag is hidden");
    check(p.nbadges == 0 && p.mutual_guilds == -1 && p.mutual_friends == -1, "no badges, unknown mutuals");
    check(p.font_id == 0 && p.ncolors == 0, "no name style");
    profile_free(&p);

    check(json_parse(k_error, sizeof k_error - 1, &root) && !profile_parse(root, NULL, &p), "error body is rejected");
    profile_free(&p);
    finish();
}
