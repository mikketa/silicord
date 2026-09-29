/* Member list operations: SYNC, INSERT, UPDATE, DELETE, INVALIDATE. */
#include <windows.h>
#include "test.h"
#include "memberlist.h"

static int apply(ml_t *l, const char *json)
{
    json_t d;

    return json_parse(json, sc_strlen(json), &d) && ml_apply(l, d);
}

static int member_is(const ml_t *l, int i, const char *id, const char *name)
{
    return i < l->n && l->items[i].valid && !l->items[i].group && lstrcmpA(l->items[i].id, id) == 0 &&
           str_eq(&l->items[i].name, name);
}

void entry(void)
{
    ml_t l = {0};

    check(apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"member_count\":5,\"online_count\":3,\"groups\":[],\"ops\":["
                    "{\"op\":\"SYNC\",\"range\":[0,99],\"items\":["
                    "{\"group\":{\"id\":\"50\",\"count\":1}},"
                    "{\"member\":{\"user\":{\"id\":\"7\",\"username\":\"mod\",\"global_name\":\"Mod\",\"avatar\":\"a7\"},"
                    "\"nick\":\"Boss\",\"roles\":[\"50\",\"51\"],\"presence\":{\"status\":\"dnd\",\"activities\":["
                    "{\"type\":0,\"name\":\"Chess\"},{\"type\":4,\"name\":\"Custom Status\",\"state\":\"busy\"}]}}},"
                    "{\"group\":{\"id\":\"online\",\"count\":2}},"
                    "{\"member\":{\"user\":{\"id\":\"8\",\"username\":\"ann\",\"global_name\":null,\"bot\":true},"
                    "\"roles\":[],\"presence\":{\"status\":\"idle\",\"activities\":[{\"type\":2,\"name\":\"Spotify\"}]}}},"
                    "{\"member\":{\"user\":{\"id\":\"9\",\"username\":\"bob\"},\"roles\":[],\"presence\":{\"status\":\"online\"}}}"
                    "]}]}"),
          "sync applies");
    check(lstrcmpA(l.guild, "1") == 0, "guild");
    apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"groups\":[{\"id\":\"50\",\"count\":1},{\"id\":\"online\",\"count\":7}],\"ops\":[]}");
    check(ml_group_count(&l, "online") == 7 && ml_group_count(&l, "50") == 1 && ml_group_count(&l, "x") == 0 && l.n == 100,
          "group sizes from the groups field");
    check(l.n == 100 && l.items[0].valid && l.items[0].group && lstrcmpA(l.items[0].id, "50") == 0 && l.items[0].count == 1,
          "role group header");
    check(member_is(&l, 1, "7", "Boss") && l.items[1].status == ML_DND && str_eq(&l.items[1].activity, "busy") &&
              str_eq(&l.items[1].roles, "50,51") && lstrcmpA(l.items[1].avatar, "a7") == 0,
          "member: nickname, status, custom status wins, roles");
    check(member_is(&l, 3, "8", "ann") && l.items[3].status == ML_IDLE && l.items[3].bot &&
              str_eq(&l.items[3].activity, "Listening to Spotify"),
          "username without display name, bot, activity");
    check(!l.items[5].valid, "slots not sent yet are empty");

    check(apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"ops\":[{\"op\":\"INSERT\",\"index\":3,\"item\":"
                    "{\"member\":{\"user\":{\"id\":\"10\",\"username\":\"new\"},\"roles\":[],\"presence\":{\"status\":\"online\"}}}}]}"),
          "insert applies");
    check(member_is(&l, 3, "10", "new") && member_is(&l, 4, "8", "ann") && l.n == 101, "insert shifts the rest down");

    apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"ops\":[{\"op\":\"UPDATE\",\"index\":4,\"item\":"
              "{\"member\":{\"user\":{\"id\":\"8\",\"username\":\"ann\"},\"roles\":[],\"presence\":{\"status\":\"offline\"}}}}]}");
    check(member_is(&l, 4, "8", "ann") && l.items[4].status == ML_OFFLINE && !l.items[4].activity.len, "update replaces");

    apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"ops\":[{\"op\":\"DELETE\",\"index\":3}]}");
    check(member_is(&l, 3, "8", "ann") && l.n == 100, "delete shifts the rest up");

    apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"ops\":[{\"op\":\"INVALIDATE\",\"range\":[0,2]}]}");
    check(!l.items[0].valid && !l.items[2].valid && l.items[3].valid, "invalidate empties a range");

    apply(&l, "{\"guild_id\":\"1\",\"id\":\"everyone\",\"ops\":[{\"op\":\"SYNC\",\"range\":[3,5],\"items\":["
              "{\"member\":{\"user\":{\"id\":\"11\",\"username\":\"solo\",\"avatar\":\"own\"},\"avatar\":\"srv\","
              "\"roles\":[]}}]}]}");
    check(member_is(&l, 3, "11", "solo") && !l.items[4].valid && !l.items[5].valid, "a sync replaces its whole range");
    check(lstrcmpA(l.items[3].avatar, "own") == 0 && lstrcmpA(l.items[3].member_avatar, "srv") == 0,
          "the server avatar is kept apart from the user's own");

    check(!apply(&l, "{\"guild_id\":\"1\",\"id\":\"123\",\"ops\":[{\"op\":\"UPDATE\",\"index\":1,\"item\":"
                     "{\"member\":{\"user\":{\"id\":\"12\",\"username\":\"other\"},\"roles\":[]}}}]}") &&
              member_is(&l, 3, "11", "solo") && l.n == 100 && lstrcmpA(l.list_id, "everyone") == 0,
          "updates to another list leave ours alone");
    check(apply(&l, "{\"guild_id\":\"2\",\"id\":\"everyone\",\"ops\":[{\"op\":\"SYNC\",\"range\":[0,99],\"items\":[]}]}") &&
              l.n == 100 && !l.items[3].valid && lstrcmpA(l.guild, "2") == 0,
          "another server starts over with its sync");
    ml_free(&l);
    finish();
}
