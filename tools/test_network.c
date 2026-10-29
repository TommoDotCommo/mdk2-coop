/* Offline Win32 regression tests: real bridge packet code, simulated Steam
 * interfaces. No Steam client, account, game, overlay or game files are used. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../bridge/bridge.h"
#include "../bridge/steam_abi.h"

static HMODULE fake_module(const char *name);
static FARPROC fake_proc(HMODULE module, const char *name);
static char *fake_command_line(void);
static DWORD fake_profile_string(const char *, const char *, const char *, char *, DWORD, const char *);
static BOOL fake_profile_write(const char *, const char *, const char *, const char *);
static UINT fake_profile_int(const char *, const char *, int, const char *);
/* Keep profile tests independent of filesystem behavior. Simulate configuration storage as
 * well as Steam; loopback sockets below still use the actual Windows API. */
#define GetPrivateProfileStringA fake_profile_string
#define GetPrivateProfileIntA fake_profile_int
#define WritePrivateProfileStringA fake_profile_write
#define GetModuleHandleA fake_module
#define GetProcAddress fake_proc
#define GetCommandLineA fake_command_line
#include "../bridge/steam.c"
#undef GetModuleHandleA
#undef GetProcAddress
#undef GetCommandLineA
/* the game's animation lookup does not exist in this process; no case here animates a gob */
DWORD *anim_state(void *gob, int id) { return NULL; }
#ifndef NET_SOURCE
#define NET_SOURCE "../bridge/net.c"
#endif
#include NET_SOURCE

#define LOBBY_A UINT64_C(109775241000000001)
#define LOBBY_B UINT64_C(109775241000000002)
#define SELF UINT64_C(76561198000000001)
#define PEER UINT64_C(76561198000000002)
#define OTHER UINT64_C(76561198000000003)
#define STRANGER UINT64_C(76561198000000004)

char coop_dir[MAX_PATH];
int local_max_hp;
int test_background = 1;
static int passed;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } passed++; } while (0)
static struct { char file[MAX_PATH], section[32], key[32], value[128]; } profile[128];
static int profile_count;
static DWORD fake_profile_string(const char *section, const char *key, const char *def, char *out, DWORD size, const char *file)
{
    int i;
    const char *value = def;
    for (i=0; i<profile_count; i++)
        if (!strcmp(profile[i].file,file) && !_stricmp(profile[i].section,section) && !_stricmp(profile[i].key,key))
            value = profile[i].value;
    snprintf(out,size,"%s",value);
    return strlen(out);
}
static BOOL fake_profile_write(const char *section, const char *key, const char *value, const char *file)
{
    int i;
    for (i=0; i<profile_count; i++)
        if (!strcmp(profile[i].file,file) && !_stricmp(profile[i].section,section) && !_stricmp(profile[i].key,key)) break;
    CHECK(i<128);
    if (i==profile_count) profile_count++;
    snprintf(profile[i].file,sizeof(profile[i].file),"%s",file);
    snprintf(profile[i].section,sizeof(profile[i].section),"%s",section);
    snprintf(profile[i].key,sizeof(profile[i].key),"%s",key);
    snprintf(profile[i].value,sizeof(profile[i].value),"%s",value);
    return TRUE;
}
static UINT fake_profile_int(const char *section, const char *key, int def, const char *file)
{
    char value[128], fallback[24];
    snprintf(fallback,sizeof(fallback),"%d",def);
    fake_profile_string(section,key,fallback,value,sizeof(value),file);
    return strtol(value,NULL,10);
}
void logmsg(const char *fmt, ...) { }
void canopy_capture(BYTE *gob, struct canopy_state *s) { memset(s,0,sizeof(*s)); }
void canopy_apply(BYTE *gob, const struct canopy_state *s) { }
int gob_hp(BYTE *gob) { return 100; }
int gob_max_hp(BYTE *gob) { return 150; }
BYTE *gob_by_handle(unsigned int handle) { return (BYTE *)1; }
int gob_type(BYTE *gob) { return 200; }
void gob_peername(BYTE *gob, char *out, int size) { out[0] = 0; }
int revive_life(void) { return 0; }
int revive_reviving(void) { return -1; }
void ents_resend(int to) { }
void pickups_resend(int to) { }
void rules_fill(struct event *e) { event_init(e, EV_RULES); }
int voice_is_packet(const void *buf, int len) { return 0; }
void voice_receive(int from, const void *buf, int len) { }
static int entity_packets, sent_packets;
void ents_receive(const void *buf, int len) { entity_packets++; }
int ents_is_packet(const void *buf, int len) { return len >= 4 && *(const unsigned int *)buf == 0x464b444d; }
void netstats_sent(int kind, int len) { sent_packets++; }

struct fake_object { void **vtable; };
static void *client_vt[13], *user_vt[3], *friends_vt[44], *utils_vt[18], *mm_vt[36], *nm_vt[4];
static struct fake_object client_obj = {client_vt}, user_obj = {user_vt}, friends_obj = {friends_vt},
    utils_obj = {utils_vt}, mm_obj = {mm_vt}, nm_obj = {nm_vt};
static int available = 1, handles = 1, online = 1, app = 213350, registered, overlay = 1;
static int create_count, join_count, leave_count, invite_count, accept_count, close_count, release_count;
static int result_ready = 1, result_failed, result_code = 1, send_result = 1, publish_ok = 1;
static int last_flags, last_channel, send_calls;
static uint64_t owner = SELF, lobby_members[16] = {SELF, PEER, OTHER}, last_join, api_call = 100;
static int lobby_count = 3, compatible = 1, protocol_ok = 1;
static char presence_text[64], command_line[128] = "mdk2hd.exe";
static BYTE sent_data[1400];
static int sent_size;
static struct steam_message queued;
static BYTE queued_payload[1500];
static int has_message;

static bool __thiscall m_logged(void *obj) { return online; }
static uint64_t * __thiscall m_id(void *obj, uint64_t *out) { *out = SELF; return out; }
static void * __thiscall c_user(void *obj, int h, int p, const char *v) { CHECK(!strcmp(v,"SteamUser023")); return &user_obj; }
static void * __thiscall c_friends(void *obj, int h, int p, const char *v) { CHECK(!strcmp(v,"SteamFriends017")); return &friends_obj; }
static void * __thiscall c_mm(void *obj, int h, int p, const char *v) { CHECK(!strcmp(v,"SteamMatchMaking009")); return &mm_obj; }
static void * __thiscall c_utils(void *obj, int p, const char *v) { CHECK(!strcmp(v,"SteamUtils010")); return &utils_obj; }
static void * __thiscall c_generic(void *obj, int h, int p, const char *v) { CHECK(!strcmp(v,"SteamNetworkingMessages002")); return &nm_obj; }
static uint32_t __thiscall m_app(void *obj) { return app; }
static bool __thiscall m_overlay(void *obj) { return overlay; }
static bool __thiscall m_completed(void *obj, uint64_t call, bool *failed) { *failed = result_failed; return result_ready; }
static bool __thiscall m_result(void *obj, uint64_t call, void *out, int size, int kind, bool *failed)
{
    *failed = result_failed;
    if (kind == CB_CREATED) {
        struct steam_created r = {result_code, LOBBY_A};
        CHECK(size == sizeof(r)); memcpy(out, &r, size);
    } else {
        struct steam_enter r = {last_join, 0, false, result_code};
        CHECK(kind == CB_ENTER && size == sizeof(r)); memcpy(out, &r, size);
    }
    return true;
}
static uint64_t __thiscall m_create(void *obj, int type, int limit)
{
    CHECK(type == 1 && limit == 16); create_count++; return ++api_call;
}
static uint64_t __thiscall m_join(void *obj, uint64_t id) { join_count++; last_join = id; return ++api_call; }
static void __thiscall m_leave(void *obj, uint64_t id) { leave_count++; }
static int __thiscall m_members(void *obj, uint64_t id) { return lobby_count; }
static uint64_t * __thiscall m_member(void *obj, uint64_t *out, uint64_t id, int index) { *out = lobby_members[index]; return out; }
static uint64_t * __thiscall m_owner(void *obj, uint64_t *out, uint64_t id) { *out = owner; return out; }
static const char * __thiscall m_data(void *obj, uint64_t id, const char *key)
{
    if (!strcmp(key, MOD_KEY)) return compatible ? MOD_VALUE : "other-mod";
    return protocol_ok ? PROTOCOL_VALUE : "older-protocol";
}
static bool __thiscall m_set_data(void *obj, uint64_t id, const char *key, const char *value) { return publish_ok; }
static bool __thiscall m_joinable(void *obj, uint64_t id, bool on) { CHECK(!on); return true; }
static bool __thiscall m_presence(void *obj, const char *key, const char *value)
{
    CHECK(!strcmp(key,"connect")); snprintf(presence_text,sizeof(presence_text),"%s",value ? value : ""); return true;
}
static void __thiscall m_invite(void *obj, uint64_t id) { CHECK(id == lobby_id); invite_count++; }
static bool __thiscall m_accept(void *obj, const struct steam_identity *id) { accept_count++; return true; }
static bool __thiscall m_close(void *obj, const struct steam_identity *id) { close_count++; return true; }
static int __thiscall m_send(void *obj, const struct steam_identity *id, const void *data, uint32_t size, int flags, int channel)
{
    CHECK(identity_id(id) != SELF); CHECK(size <= sizeof(sent_data));
    memcpy(sent_data,data,size); sent_size=size; last_flags=flags; last_channel=channel; send_calls++; return send_result;
}
static int __thiscall m_receive(void *obj, int channel, struct steam_message **out, int max)
{
    CHECK(channel == STEAM_CHANNEL);
    if (!has_message) return 0;
    has_message=0; *out=&queued; return 1;
}
static void m_release(struct steam_message *m) { CHECK(m == &queued); release_count++; }
static void mock_run(void) { }
static void mock_register(struct steam_callback *cb, int id)
{
    CHECK(cb->id == id); CHECK(cb->vtable->size(cb) == cb->bytes); registered++;
}
static void *mock_client(void) { return &client_obj; }
static int mock_handle(void) { return handles; }
static HMODULE fake_module(const char *name) { CHECK(!strcmp(name,"steam_api.dll")); return available ? (HMODULE)1 : NULL; }
static FARPROC fake_proc(HMODULE module, const char *name)
{
    if (!strcmp(name,"SteamClient")) return (FARPROC)(void *)mock_client;
    if (!strcmp(name,"SteamAPI_GetHSteamUser") || !strcmp(name,"SteamAPI_GetHSteamPipe")) return (FARPROC)(void *)mock_handle;
    if (!strcmp(name,"SteamAPI_RunCallbacks")) return (FARPROC)(void *)mock_run;
    if (!strcmp(name,"SteamAPI_RegisterCallback")) return (FARPROC)(void *)mock_register;
    return NULL;
}
static char *fake_command_line(void) { return command_line; }
static void setup(void)
{
    client_vt[SC_USER]=c_user; client_vt[SC_FRIENDS]=c_friends; client_vt[SC_MATCHMAKING]=c_mm;
    client_vt[SC_UTILS]=c_utils; client_vt[SC_GENERIC]=c_generic;
    user_vt[SU_LOGGED_ON]=m_logged; user_vt[SU_ID]=m_id;
    utils_vt[UT_APP_ID]=m_app; utils_vt[UT_COMPLETED]=m_completed; utils_vt[UT_RESULT]=m_result; utils_vt[17]=m_overlay;
    friends_vt[SF_INVITE]=m_invite; friends_vt[SF_PRESENCE]=m_presence;
    mm_vt[MM_CREATE]=m_create; mm_vt[MM_JOIN]=m_join; mm_vt[MM_LEAVE]=m_leave;
    mm_vt[MM_MEMBERS]=m_members; mm_vt[MM_MEMBER]=m_member; mm_vt[MM_OWNER]=m_owner;
    mm_vt[MM_DATA]=m_data; mm_vt[MM_SET_DATA]=m_set_data; mm_vt[MM_JOINABLE]=m_joinable;
    nm_vt[NM_SEND]=m_send; nm_vt[NM_RECEIVE]=m_receive; nm_vt[NM_ACCEPT]=m_accept; nm_vt[NM_CLOSE]=m_close;
}
static void queue_packet(uint64_t peer, const struct player_packet *p, int offset)
{
    struct steam_identity identity;
    memset(&queued,0,sizeof(queued));
    queued.size=packet_pack(p,queued_payload,sizeof(queued_payload));
    queued.data=queued_payload; queued.release=m_release;
    identity_set(&identity,peer); memcpy((BYTE *)&queued+offset,&identity,sizeof(identity)); has_message=1;
}
static struct player_packet packet(unsigned int id, unsigned int process, unsigned int seq)
{
    struct player_packet p;
    memset(&p,0,sizeof(p)); p.magic=PLAYER_MAGIC; p.id=id; p.nonce=process; p.seq=seq;
    p.slot=NO_SLOT; p.epoch=cur_epoch; p.level=0; p.you=1;
    return p;
}
static void dispatch(int id, void *data)
{
    int i;
    for (i=0;i<4;i++) if (callbacks[i].id==id) callbacks[i].vtable->run(&callbacks[i],data);
}
static void host(void)
{
    cur_level=0; owner=SELF; lobby_members[0]=SELF; lobby_members[1]=PEER; lobby_members[2]=OTHER; lobby_count=3;
    CHECK(steam_host_lobby()); poll_call(); CHECK(steam_ready() && role_host);
}
static void client_join(void)
{
    cur_level=0; owner=PEER; CHECK(steam_join_lobby("109775241000000002")); poll_call();
    CHECK(steam_ready() && !role_host && links[0].steam_id==PEER);
}
int main(void)
{
    char path[MAX_PATH];
    uint64_t parsed;
    struct player_packet p;
    struct steam_identity identity;
    int before;
    setup(); GetCurrentDirectoryA(MAX_PATH,coop_dir); snprintf(path,sizeof(path),"%s\\test.ini",coop_dir);
    /* A wounded or late-joined enemy keeps its full scaled denominator. */
    links[1].used=1; links[1].out_next=links[1].out_acked=0;
    event_queue_spawn(1,EV_SPAWN,123,200,75,NULL,"enemy");
    CHECK(links[1].out_next==1);
    CHECK(links[1].outbox[0].damage==75);
    CHECK(links[1].outbox[0].extra[0]==150);
    /* Joining protections operate before packet/world application. */
    {
        BYTE wire[1400];
        struct sockaddr_in from = {0};
        unsigned int old_nonce = nonce;
        int size;
        role_host=0; net_steam=0; have_remote=0;
        links[0].addr=from;
        p=packet(321,654,1); p.slot=0; p.you=1;
        size=packet_pack(&p,wire,sizeof(wire)); CHECK(size>0);
        net_cancel_join(); CHECK(net_join_cancelled() && !net_ready());
        accept_datagram(wire,size,&from,0,1000); CHECK(!have_remote);
        net_retry_join(); CHECK(!net_join_cancelled() && nonce!=old_nonce && my_slot==-1);
        accept_datagram(wire,size,&from,0,1001); CHECK(have_remote && my_slot==1);
        *(unsigned int *)wire=0x434b444d;
        from.sin_port=htons(1234);
        accept_datagram(wire,size,&from,0,1002); CHECK(!net_version_mismatch());
        from=links[0].addr;
        accept_datagram(wire,size,&from,0,1003); CHECK(net_version_mismatch());
    }
    DeleteFileA(path); snprintf(ini_path,sizeof(ini_path),"%s",path);
    CHECK(!parse_lobby("",&parsed)); CHECK(!parse_lobby("-1",&parsed));
    CHECK(!parse_lobby("18446744073709551616",&parsed)); CHECK(!parse_lobby("76561198000000001",&parsed));
    CHECK(!parse_lobby("109775241000000001x",&parsed)); CHECK(parse_lobby("109775241000000001",&parsed) && parsed==LOBBY_A);
    CHECK(parse_connect("mdk2hd.exe +connect_lobby 109775241000000001 -arg",&parsed));
    CHECK(!parse_connect("x+connect_lobby 109775241000000001",&parsed));
    CHECK(!parse_connect("+connect_lobby 109775241000000001suffix",&parsed));
    CHECK(!parse_connect("+connect_lobby_bad 109775241000000001",&parsed));
    available=0; CHECK(!initialize()); available=1; handles=0; CHECK(!initialize()); handles=1;
    app=480; CHECK(!initialize() && registered==0); app=213350;
    online=0; CHECK(!initialize()); online=1; CHECK(initialize() && registered==4 && self_id==SELF);
    net_steam=1; role_host=0; cur_level=0;
    steam_poll(); CHECK(startup_done && !steam_ready() && create_count==0 && join_count==0);
    host(); CHECK(!strcmp(steam_lobby_id(),"109775241000000001")); CHECK(strstr(presence_text,"+connect_lobby "));
    CHECK(sock==INVALID_SOCKET && net_ready()); CHECK(steam_peer_allowed(PEER) && !steam_peer_allowed(STRANGER));
    CHECK(steam_invite() && invite_count==1); overlay=0; CHECK(!steam_invite() && invite_count==1); overlay=1;
    identity_set(&identity,STRANGER); before=close_count; dispatch(CB_SESSION_REQUEST,&identity); CHECK(close_count==before+1);
    identity_set(&identity,PEER); before=accept_count; dispatch(CB_SESSION_REQUEST,&identity); CHECK(accept_count==before+1);

    /* Host slot assignment is keyed by authenticated SteamID, even if IDs in
     * player.ini collide. Reliable events still deduplicate and acknowledge. */
    p=packet(42,100,1); p.nevents=1; p.events[0].kind=EV_MARK; p.events[0].id=1;
    queue_packet(PEER,&p,16); steam_poll(); CHECK(links[1].used && links[1].steam_id==PEER && players[1].connected);
    CHECK(links[1].inbox_count==1); queue_packet(PEER,&p,12); steam_poll(); CHECK(links[1].inbox_count==1);
    queue_packet(OTHER,&p,16); steam_poll(); CHECK(links[2].used && links[2].steam_id==OTHER && links[1].steam_id==PEER);
    before=release_count; queue_packet(STRANGER,&p,16); steam_poll(); CHECK(release_count==before+1 && !links[3].used);
    queue_packet(PEER,&p,16); queued.size=1500; steam_poll(); CHECK(release_count==before+2);
    queue_packet(PEER,&p,16); queued.size=4; steam_poll(); CHECK(release_count==before+3);
    before=sent_packets; net_send_player(NULL); CHECK(send_calls>=2 && sent_packets==before+2);
    CHECK(last_flags==(1|32) && last_channel==STEAM_CHANNEL && sent_size>0);
    send_result=2; before=sent_packets; net_send("test",4); CHECK(sent_packets==before); send_result=1;
    /* Welcome now queues the host's rules before gameplay events. Acknowledge both. */
    CHECK(links[1].out_next==2 && links[1].outbox[1].kind==EV_RULES);
    event_queue(EV_MARK,"marker",0,0,0); CHECK(links[1].out_next==3);
    p.ack=2; p.seq=2; queue_packet(PEER,&p,16); steam_poll(); CHECK(links[1].out_acked==2);
    sim_loss_pct=100; p.seq=3; queue_packet(PEER,&p,16); steam_poll(); CHECK(players[1].seq==2); sim_loss_pct=0;
    sim_latency_ms=100; p.seq=4; queue_packet(PEER,&p,16); steam_poll(); CHECK(nsimq==1 && players[1].seq==2);
    simq[0].due=now_ms()-1; net_poll(); CHECK(nsimq==0 && players[1].seq==4); sim_latency_ms=0;
    net_steam_left(PEER); CHECK(!links[1].used && !players[1].connected);
    queue_packet(PEER,&p,16); steam_poll(); CHECK(links[1].used && links[1].steam_id==PEER);
    p.nonce++; p.seq=1; queue_packet(PEER,&p,16); steam_poll();
    CHECK(links[1].out_next==2 && links[1].outbox[1].kind==EV_RULES && links[1].nonce==p.nonce);
    memset(slot_steam_id,0,sizeof(slot_steam_id)); slots_load();
    CHECK(slot_steam_id[1]==PEER && slot_steam_id[2]==OTHER);
    before=join_count; cur_level=1; CHECK(!steam_join_lobby("109775241000000002") && join_count==before); cur_level=0;

    /* Async failures, single pending request, metadata/version rejection. */
    result_ready=0; CHECK(steam_host_lobby()); before=create_count; CHECK(!steam_host_lobby() && create_count==before);
    CHECK(!steam_ready()); result_ready=1; result_failed=1; poll_call(); CHECK(!pending_call && !steam_ready()); result_failed=0;
    host(); publish_ok=0; CHECK(steam_host_lobby()); poll_call(); CHECK(!steam_ready()); publish_ok=1;
    compatible=0; owner=PEER; CHECK(steam_join_lobby("109775241000000002")); poll_call(); CHECK(!steam_ready()); compatible=1;
    protocol_ok=0; CHECK(steam_join_lobby("109775241000000002")); poll_call(); CHECK(!steam_ready()); protocol_ok=1;
    result_code=4; CHECK(steam_join_lobby("109775241000000002")); poll_call(); CHECK(!steam_ready() && strstr(steam_status(),"full")); result_code=1;

    /* A client trusts only its lobby's original host, including entity/relay
     * traffic. Session reset clears queued packets and old reliable events. */
    client_join(); CHECK(!steam_peer_allowed(OTHER));
    p=packet(99,200,1); p.slot=0; p.gen=3;
    queue_packet(OTHER,&p,16); steam_poll(); CHECK(!have_remote);
    queue_packet(PEER,&p,16); steam_poll(); CHECK(have_remote && my_slot==1 && players[0].connected);
    queue_packet(PEER,&p,16); *(unsigned int *)queued_payload=0x464b444d; steam_poll(); CHECK(entity_packets==1);
    queue_packet(OTHER,&p,16); *(unsigned int *)queued_payload=0x464b444d; steam_poll(); CHECK(entity_packets==1);
    identity_set(&identity,OTHER); before=accept_count; dispatch(CB_SESSION_REQUEST,&identity); CHECK(accept_count==before);
    sim_latency_ms=100; queue_packet(PEER,&p,16); steam_poll(); CHECK(nsimq==1);
    CHECK(steam_leave_lobby()); CHECK(!nsimq && !have_remote && !links[0].used && !steam_ready() && !presence_text[0]); sim_latency_ms=0;
    client_join(); owner=OTHER; refresh_members(); CHECK(!steam_ready() && !role_host && strstr(steam_status(),"Host left"));
    owner=PEER; cur_level=1; CHECK(steam_join_lobby("109775241000000002")); poll_call();
    CHECK(steam_ready() && !role_host && host_changed_take());
    cur_level=0; CHECK(steam_leave_lobby());
    owner=PEER; client_join();
    { struct steam_chat_update change={LOBBY_B,SELF,PEER,8}; dispatch(CB_CHAT_UPDATE,&change); CHECK(!steam_ready()); }
    host();
    { struct steam_join_requested invite={LOBBY_B,PEER}; owner=PEER; dispatch(CB_JOIN,&invite); poll_call(); CHECK(lobby_id==LOBBY_B && !role_host); }
    { struct steam_presence_join invite={PEER,"+connect_lobby 109775241000000001"}; dispatch(CB_PRESENCE_JOIN,&invite); poll_call(); CHECK(lobby_id==LOBBY_A); }
    CHECK(steam_leave_lobby());

    /* Real loopback UDP still works without Steam. A missing transport key
     * preserves existing configurations; client retarget is UDP-only. */
    net_steam=0; memset(links,0,sizeof(links)); memset(players,0,sizeof(players));
    WritePrivateProfileStringA("net","role","host",path); WritePrivateProfileStringA("net","listen_port","0",path);
    net_open(path); CHECK(!net_steam && net_ready());
    {
        SOCKET peer=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        struct sockaddr_in target, peer_addr;
        int len=sizeof(target), received_len, udp_slot;
        BYTE data[1400]; DWORD timeout=1000;
        CHECK(peer!=INVALID_SOCKET); CHECK(!getsockname(sock,(struct sockaddr *)&target,&len)); target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        p=packet(123,789,1); len=packet_pack(&p,data,sizeof(data));
        CHECK(sendto(peer,(char *)data,len,0,(struct sockaddr *)&target,sizeof(target))==len);
        net_poll();
        for (udp_slot=1; udp_slot<MAX_PLAYERS && !links[udp_slot].used; udp_slot++) ;
        CHECK(udp_slot<MAX_PLAYERS && !links[udp_slot].steam_id && players[udp_slot].connected);
        before=sent_packets; send_packet(udp_slot,&p); CHECK(sent_packets==before+1);
        setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,(char *)&timeout,sizeof(timeout)); len=sizeof(peer_addr);
        received_len=recvfrom(peer,(char *)data,sizeof(data),0,(struct sockaddr *)&peer_addr,&len);
        CHECK(received_len>0 && packet_unpack(data,received_len,&p));
        role_host=0; CHECK(net_retarget("127.0.0.1",27900)); net_steam=1; CHECK(!net_retarget("127.0.0.1",27900));
        closesocket(peer); closesocket(sock); sock=INVALID_SOCKET; WSACleanup();
    }
    /* A checkpoint number does not identify a world. A new epoch at the same
     * checkpoint must wait for the actual load; a reconnect in the same epoch must not. */
    {
        struct event level, spawn;
        role_host=0; cur_level=1; cur_cp=2; cur_epoch=pending_epoch=7;
        event_init(&level,EV_LEVEL); level.type=1; level.damage=2; level.netid=8;
        net_level_event(&level);
        CHECK(pending_epoch==8 && cur_epoch==7);
        event_init(&spawn,EV_SPAWN); spawn.epoch=8;
        CHECK(event_must_wait(&spawn) && !event_must_wait(&level));
        spawn.epoch=7; CHECK(!event_must_wait(&spawn));
        net_level_reset(0); CHECK(cur_epoch==8);
        spawn.epoch=8; CHECK(!event_must_wait(&spawn));
        net_level_event(&level); CHECK(cur_epoch==8 && !event_must_wait(&spawn));
        cur_epoch=pending_epoch=65535; level.netid=0; spawn.epoch=0;
        net_level_event(&level); CHECK(cur_epoch==65535 && event_must_wait(&spawn));
        net_level_reset(0); CHECK(cur_epoch==0 && !event_must_wait(&spawn));
    }
    /* A wounded or late-joined enemy keeps its full scaled denominator. */
    links[1].used=1; links[1].out_next=links[1].out_acked=0;
    event_queue_spawn(1,EV_SPAWN,123,200,75,NULL,"enemy");
    CHECK(links[1].out_next==1);
    CHECK(links[1].outbox[0].damage==75);
    CHECK(links[1].outbox[0].extra[0]==150);
    /* Joining protections operate before packet/world application. */
    {
        BYTE wire[1400];
        struct sockaddr_in from = {0};
        unsigned int old_nonce = nonce;
        int size;
        role_host=0; net_steam=0; have_remote=0;
        links[0].addr=from;
        p=packet(321,654,1); p.slot=0; p.you=1;
        size=packet_pack(&p,wire,sizeof(wire)); CHECK(size>0);
        net_cancel_join(); CHECK(net_join_cancelled() && !net_ready());
        accept_datagram(wire,size,&from,0,1000); CHECK(!have_remote);
        net_retry_join(); CHECK(!net_join_cancelled() && nonce!=old_nonce && my_slot==-1);
        accept_datagram(wire,size,&from,0,1001); CHECK(have_remote && my_slot==1);
        *(unsigned int *)wire=0x434b444d;
        from.sin_port=htons(1234);
        accept_datagram(wire,size,&from,0,1002); CHECK(!net_version_mismatch());
        from=links[0].addr;
        accept_datagram(wire,size,&from,0,1003); CHECK(net_version_mismatch());
    }
    DeleteFileA(path);
    printf("PASS: %d network checks (simulated Steam + real loopback UDP)\n",passed);
    return 0;
}
