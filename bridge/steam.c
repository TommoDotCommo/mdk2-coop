/* Steam lobbies and modern relay-capable NetworkingMessages, using the original
 * game's Steam connection and AppID. No Steam DLL replacement, app-id override,
 * private SDK, or SteamAPI_Shutdown (the original game owns the API lifetime).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "bridge.h"
#include "steam_abi.h"

#define MOD_KEY "mdk2_mod"
#define MOD_VALUE "mdk2-coop"
#define PROTOCOL_KEY "mdk2_protocol"
#define PROTOCOL_VALUE "MDKB-MDKF-MDKS-1"
#define STEAM_CHANNEL 27900  /* virtual channel, not a UDP listen port */
#define CALL_TIMEOUT_MS 30000.0

static void *matchmaking, *friends, *utils, *user, *messages;
static void (__cdecl *run_callbacks)(void);
static void (__cdecl *register_callback)(struct steam_callback *, int);
static uint64_t self_id, lobby_id, host_id_steam, members[MAX_PLAYERS];
static uint64_t pending_call, wanted_lobby;
static int pending_kind, initialized, startup_done, member_count;
static double pending_since, next_init, next_members;
static char ini_path[MAX_PATH], status_line[160] = "Steam is starting", lobby_text[24];
static struct steam_callback callbacks[4];

static void status(const char *s)
{
    if (strcmp(status_line, s)) {
        snprintf(status_line, sizeof(status_line), "%s", s);
        logmsg("steam: %s", status_line);
    }
}

const char *steam_status(void) { return status_line; }
const char *steam_lobby_id(void) { return lobby_text; }
int steam_ready(void) { return initialized && lobby_id != 0 && host_id_steam != 0; }

static void identity_set(struct steam_identity *identity, uint64_t id)
{
    memset(identity, 0, sizeof(*identity));
    identity->type = 16;
    identity->size = 8;
    identity->data.id = id;
}

static uint64_t identity_id(const struct steam_identity *identity)
{
    return identity->type == 16 && identity->size == 8 ? identity->data.id : 0;
}

static int parse_lobby(const char *text, uint64_t *id)
{
    uint64_t n = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (!p || !*p)
        return 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || n > (UINT64_MAX - (*p - '0')) / 10)
            return 0;
        n = n * 10 + (*p - '0');
    }
    /* Public-universe chat IDs are lobby IDs; individual SteamIDs are not. */
    if ((n >> 56) != 1 || ((n >> 52) & 15) != 8)
        return 0;
    *id = n;
    return 1;
}

static int parse_connect(const char *text, uint64_t *id)
{
    static const char token[] = "+connect_lobby";
    char digits[24];
    const char *p = strstr(text, token);
    int n = 0;

    if (!p || (p != text && !isspace((unsigned char)p[-1]) && p[-1] != '"'))
        return 0;
    p += sizeof(token) - 1;
    if (!isspace((unsigned char)*p))
        return 0;
    while (isspace((unsigned char)*p)) p++;
    while (*p >= '0' && *p <= '9' && n < (int)sizeof(digits) - 1)
        digits[n++] = *p++;
    if (*p && !isspace((unsigned char)*p) && *p != '"')
        return 0;
    digits[n] = 0;
    return parse_lobby(digits, id);
}

static uint64_t lobby_owner(uint64_t id)
{
    uint64_t result = 0;
    SMETHOD(matchmaking, MM_OWNER, uint64_t *, uint64_t *, uint64_t)(matchmaking, &result, id);
    return result;
}

static uint64_t lobby_member(int index)
{
    uint64_t result = 0;
    SMETHOD(matchmaking, MM_MEMBER, uint64_t *, uint64_t *, uint64_t, int)(matchmaking, &result, lobby_id, index);
    return result;
}

static void close_peer(uint64_t peer)
{
    struct steam_identity identity;
    identity_set(&identity, peer);
    SMETHOD(messages, NM_CLOSE, bool, const struct steam_identity *)(messages, &identity);
    net_steam_left(peer);
}

static void presence(const char *connect)
{
    SMETHOD(friends, SF_PRESENCE, bool, const char *, const char *)(friends, "connect", connect);
}

/* Called only on the game thread, outside DllMain. Process exit also removes the
 * user from Steam's lobby automatically. */
static void leave(void)
{
    int i;
    for (i = 0; i < member_count; i++)
        if (members[i] != self_id)
            close_peer(members[i]);
    member_count = 0;
    if (lobby_id) {
        if (host_id_steam == self_id)
            SMETHOD(matchmaking, MM_JOINABLE, bool, uint64_t, bool)(matchmaking, lobby_id, false);
        SMETHOD(matchmaking, MM_LEAVE, void, uint64_t)(matchmaking, lobby_id);
    }
    lobby_id = host_id_steam = 0;
    lobby_text[0] = 0;
    presence(NULL);
    net_steam_session(role_host, 0);
}

static void refresh_members(void)
{
    uint64_t old[MAX_PLAYERS];
    int i, j, old_count = member_count, count;

    if (!lobby_id)
        return;
    /* Steam elects a new lobby owner on exit, but the simulation has no host
     * migration. Never silently promote another player to world authority. */
    if (lobby_owner(lobby_id) != host_id_steam) {
        leave();
        status("Host left the lobby. Join their new lobby when they return.");
        return;
    }
    memcpy(old, members, sizeof(old));
    count = SMETHOD(matchmaking, MM_MEMBERS, int, uint64_t)(matchmaking, lobby_id);
    member_count = 0;
    for (i = 0; i < count && member_count < MAX_PLAYERS; i++) {
        uint64_t id = lobby_member(i);
        if (id)
            members[member_count++] = id;
    }
    for (i = 0; i < old_count; i++) {
        for (j = 0; j < member_count && members[j] != old[i]; j++) ;
        if (j == member_count && old[i] != self_id)
            close_peer(old[i]);
    }
    next_members = now_ms() + 1000.0;
}

int steam_peer_allowed(uint64_t peer)
{
    int i;
    if (!steam_ready() || !peer || peer == self_id || (!role_host && peer != host_id_steam))
        return 0;
    for (i = 0; i < member_count; i++)
        if (members[i] == peer)
            return 1;
    return 0;
}

static int can_change(int joining)
{
    if (!initialized) {
        status("Steam unavailable. Start MDK2 HD from Steam with Steam online.");
        return 0;
    }
    if (pending_call) {
        status("Waiting for Steam's lobby response");
        return 0;
    }
    if (cur_level >= 1 && !(joining && !role_host && !steam_ready())) {
        status("Return to the title screen before changing lobbies");
        return 0;
    }
    return 1;
}

static void start_call(uint64_t call, int kind)
{
    pending_call = call;
    pending_kind = kind;
    pending_since = now_ms();
    if (!call)
        status("Steam could not start the lobby request. Check your connection and retry.");
}

int steam_host_lobby(void)
{
    if (!can_change(0))
        return 0;
    leave();
    wanted_lobby = 0;
    start_call(SMETHOD(matchmaking, MM_CREATE, uint64_t, int, int)(matchmaking, 1, MAX_PLAYERS), CB_CREATED);
    if (pending_call)
        status("Creating a friends-only lobby");
    return pending_call != 0;
}

static int join(uint64_t id)
{
    if (!can_change(1))
        return 0;
    if (id == lobby_id)
        return 1;
    leave();
    wanted_lobby = id;
    start_call(SMETHOD(matchmaking, MM_JOIN, uint64_t, uint64_t)(matchmaking, id), CB_ENTER);
    if (pending_call)
        status("Joining Steam lobby");
    return pending_call != 0;
}

int steam_join_lobby(const char *text)
{
    uint64_t id;
    if (!parse_lobby(text, &id)) {
        status("Enter the host's numeric lobby ID");
        return 0;
    }
    return join(id);
}

int steam_leave_lobby(void)
{
    if (!can_change(0))
        return 0;
    leave();
    status("Lobby left. Host a lobby or accept an invite.");
    return 1;
}

int steam_invite(void)
{
    if (!steam_ready() || !role_host) {
        status("Create a lobby before inviting friends");
        return 0;
    }
    if (!SMETHOD(utils, 17, bool)(utils)) {
        status("Steam overlay is disabled. Share your lobby ID instead.");
        return 0;
    }
    SMETHOD(friends, SF_INVITE, void, uint64_t)(friends, lobby_id);
    return 1;
}

static void __thiscall callback_run(struct steam_callback *callback, void *data)
{
    if (callback->id == CB_JOIN) {
        join(((struct steam_join_requested *)data)->lobby);
    } else if (callback->id == CB_PRESENCE_JOIN) {
        struct steam_presence_join *p = data;
        char connect[sizeof(p->connect) + 1];
        uint64_t id;
        memcpy(connect, p->connect, sizeof(p->connect));
        connect[sizeof(p->connect)] = 0;
        if (parse_connect(connect, &id))
            join(id);
    } else if (callback->id == CB_CHAT_UPDATE) {
        struct steam_chat_update *p = data;
        if (p->lobby == lobby_id) {
            if (p->state & (2 | 4 | 8 | 16)) {
                if (p->changed == self_id || p->changed == host_id_steam) {
                    leave();
                    status("Disconnected from lobby. Accept an invite to reconnect.");
                    return;
                }
                close_peer(p->changed);
            }
            refresh_members();
        }
    } else if (callback->id == CB_SESSION_REQUEST) {
        const struct steam_identity *identity = data;
        refresh_members();
        if (steam_peer_allowed(identity_id(identity)))
            SMETHOD(messages, NM_ACCEPT, bool, const struct steam_identity *)(messages, identity);
        else
            SMETHOD(messages, NM_CLOSE, bool, const struct steam_identity *)(messages, identity);
    }
}

static void __thiscall callback_result(struct steam_callback *callback, void *data, bool failed, uint64_t call)
{
    if (!failed)
        callback_run(callback, data);
}

static int __thiscall callback_size(struct steam_callback *callback) { return callback->bytes; }
static const struct steam_callback_vtable callback_vtable = {callback_run, callback_result, callback_size};

static int initialize(void)
{
    HMODULE api = GetModuleHandleA("steam_api.dll");
    void *(__cdecl *get_client)(void);
    int (__cdecl *get_user)(void), (__cdecl *get_pipe)(void);
    void *client;
    int huser, pipe, i;
    static const int ids[] = {CB_JOIN, CB_PRESENCE_JOIN, CB_CHAT_UPDATE, CB_SESSION_REQUEST};
    static const int sizes[] = {sizeof(struct steam_join_requested), sizeof(struct steam_presence_join),
                               sizeof(struct steam_chat_update), sizeof(struct steam_identity)};

    if (!api)
        return 0;
    get_client = (void *)GetProcAddress(api, "SteamClient");
    get_user = (void *)GetProcAddress(api, "SteamAPI_GetHSteamUser");
    get_pipe = (void *)GetProcAddress(api, "SteamAPI_GetHSteamPipe");
    run_callbacks = (void *)GetProcAddress(api, "SteamAPI_RunCallbacks");
    register_callback = (void *)GetProcAddress(api, "SteamAPI_RegisterCallback");
    if (!get_client || !get_user || !get_pipe || !run_callbacks || !register_callback)
        return 0;
    huser = get_user();
    pipe = get_pipe();
    if (!huser || !pipe || !(client = get_client()))
        return 0;
    user = SMETHOD(client, SC_USER, void *, int, int, const char *)(client, huser, pipe, "SteamUser023");
    friends = SMETHOD(client, SC_FRIENDS, void *, int, int, const char *)(client, huser, pipe, "SteamFriends017");
    matchmaking = SMETHOD(client, SC_MATCHMAKING, void *, int, int, const char *)(client, huser, pipe, "SteamMatchMaking009");
    utils = SMETHOD(client, SC_UTILS, void *, int, const char *)(client, pipe, "SteamUtils010");
    messages = SMETHOD(client, SC_GENERIC, void *, int, int, const char *)(client, huser, pipe, "SteamNetworkingMessages002");
    if (!user || !friends || !matchmaking || !utils || !messages) {
        status("Steam networking interfaces unavailable. Update Steam or select UDP and restart.");
        return 0;
    }
    if (SMETHOD(utils, UT_APP_ID, uint32_t)(utils) != 213350) {
        status("Steam AppID must be MDK2 HD (213350). Remove any steam_appid.txt override.");
        return 0;
    }
    if (!SMETHOD(user, SU_LOGGED_ON, bool)(user))
        return 0;
    SMETHOD(user, SU_ID, uint64_t *, uint64_t *)(user, &self_id);
    if (!self_id)
        return 0;
    for (i = 0; i < 4; i++) {
        callbacks[i].vtable = &callback_vtable;
        callbacks[i].bytes = sizes[i];
        callbacks[i].id = ids[i];
        register_callback(&callbacks[i], ids[i]);
    }
    initialized = 1;
    logmsg("steam: connected through the game's Steam API (app 213350)");
    return 1;
}

void steam_open(const char *ini)
{
    snprintf(ini_path, sizeof(ini_path), "%s", ini);
    status("Waiting for Steam. Start the game from Steam with Steam online.");
}

static void entered(uint64_t id, int hosting)
{
    const char *mod, *protocol;
    char connect[64];

    lobby_id = id;
    host_id_steam = lobby_owner(id);
    /* A request can finish after the player starts a solo level. Switching a
     * running host world to client authority requires returning to the title. */
    if (!hosting && role_host && cur_level >= 1) {
        leave();
        status("Return to the title screen before joining a lobby");
        return;
    }
    if (hosting) {
        if (host_id_steam != self_id ||
            !SMETHOD(matchmaking, MM_SET_DATA, bool, uint64_t, const char *, const char *)(matchmaking, id, MOD_KEY, MOD_VALUE) ||
            !SMETHOD(matchmaking, MM_SET_DATA, bool, uint64_t, const char *, const char *)(matchmaking, id, PROTOCOL_KEY, PROTOCOL_VALUE)) {
            leave();
            status("Could not publish lobby data. Retry hosting.");
            return;
        }
    } else {
        mod = SMETHOD(matchmaking, MM_DATA, const char *, uint64_t, const char *)(matchmaking, id, MOD_KEY);
        /* Steam's strings can share scratch storage: compare before the next call. */
        if (!mod || strcmp(mod, MOD_VALUE)) {
            leave();
            status("That lobby is not an MDK2 co-op lobby");
            return;
        }
        protocol = SMETHOD(matchmaking, MM_DATA, const char *, uint64_t, const char *)(matchmaking, id, PROTOCOL_KEY);
        if (!protocol || strcmp(protocol, PROTOCOL_VALUE) || !host_id_steam || host_id_steam == self_id) {
            leave();
            status("Incompatible lobby. Everyone needs the same mod version.");
            return;
        }
    }
    net_steam_session(hosting, host_id_steam);
    WritePrivateProfileStringA("net", "role", hosting ? "host" : "client", ini_path);
    snprintf(lobby_text, sizeof(lobby_text), "%llu", (unsigned long long)id);
    refresh_members();
    if (!lobby_id)
        return;
    snprintf(connect, sizeof(connect), "+connect_lobby %s", lobby_text);
    presence(connect);
    status(hosting ? "Lobby ready. Invite friends or share the lobby ID." : "Lobby joined. Waiting for the host's game.");
}

static void poll_call(void)
{
    bool failed = false;
    int kind;
    uint64_t call;
    union { struct steam_created created; struct steam_enter enter; } result;
    int size;

    if (!pending_call)
        return;
    if (!SMETHOD(utils, UT_COMPLETED, bool, uint64_t, bool *)(utils, pending_call, &failed)) {
        if (now_ms() - pending_since > CALL_TIMEOUT_MS)
            status("Steam lobby request is taking too long. Check Steam's connection.");
        /* Keep the handle: discarding it could leak a lobby created by a late result. */
        return;
    }
    call = pending_call;
    kind = pending_kind;
    pending_call = 0;
    size = kind == CB_CREATED ? sizeof(result.created) : sizeof(result.enter);
    memset(&result, 0, sizeof(result));
    if (failed || !SMETHOD(utils, UT_RESULT, bool, uint64_t, void *, int, int, bool *)(utils, call, &result, size, kind, &failed) || failed) {
        status("Steam lobby request failed. Check Steam is online and retry.");
        return;
    }
    if (kind == CB_CREATED && result.created.result == 1 && result.created.lobby)
        entered(result.created.lobby, 1);
    else if (kind == CB_ENTER && result.enter.response == 1 && result.enter.lobby == wanted_lobby)
        entered(result.enter.lobby, 0);
    else if (kind == CB_ENTER && result.enter.response == 4)
        status("Lobby is full");
    else
        status("Lobby unavailable or access denied. Ask the host for a new invite.");
}

void steam_poll(void)
{
    int batch, i, count;
    double now = now_ms();

    if (!initialized) {
        if (now < next_init)
            return;
        next_init = now + 2000.0;
        if (!initialize())
            return;
    }
    if (!startup_done) {
        uint64_t id;
        char configured[32];
        startup_done = 1;
        GetPrivateProfileStringA("net", "lobby_id", "", configured, sizeof(configured), ini_path);
        if (parse_connect(GetCommandLineA(), &id))
            join(id);
        else if (role_host)
            steam_host_lobby();
        else if (configured[0])
            steam_join_lobby(configured);
        else
            status("Accept a Steam invite or enter the host's lobby ID");
    }
    run_callbacks();
    poll_call();
    if (!steam_ready())
        return;
    if (now >= next_members)
        refresh_members();
    /* Bound the work per frame, including rejected traffic; always release messages. */
    for (batch = 0; batch < 16; batch++) {
        struct steam_message *incoming[16];
        count = SMETHOD(messages, NM_RECEIVE, int, int, struct steam_message **, int)(messages, STEAM_CHANNEL, incoming, 16);
        for (i = 0; i < count; i++) {
            struct steam_message *m = incoming[i];
            uint64_t peer = identity_id(&m->identity);
            if (!peer)
                peer = identity_id((const struct steam_identity *)((const BYTE *)m + 12));
            if (m->size > 0 && m->size <= 1400 && m->data && steam_peer_allowed(peer))
                net_steam_receive(m->data, m->size, peer);
            m->release(m);
        }
        if (count < 16)
            break;
    }
}

int steam_send(uint64_t peer, const void *buf, int len)
{
    struct steam_identity identity;
    int result;
    static double next_error;
    if (!steam_peer_allowed(peer) || len <= 0 || len > 1400)
        return 0;
    identity_set(&identity, peer);
    /* Unreliable + NoNagle + AutoRestartBrokenSession: the bridge already resends
     * reliable events. Do not queue stale snapshots behind a reliable stream. */
    result = SMETHOD(messages, NM_SEND, int, const struct steam_identity *, const void *, uint32_t, int, int)
        (messages, &identity, buf, len, 1 | 32, STEAM_CHANNEL);
    if (result != 1 && now_ms() >= next_error) {
        logmsg("steam: send failed (%d), retrying through the normal packet loop", result);
        next_error = now_ms() + 5000.0;
    }
    return result == 1;
}
