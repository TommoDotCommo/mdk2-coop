/* Exercise the real capture/replay code with a fake engine. No game files needed. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "../bridge.h"

static void test_position(void *gob, float x, float y, float z);
static void test_ignore_owner(void *list, DWORD owner);
#undef gob_set_position
#define gob_set_position test_position
#define MAIN_SCENE ((void *)1)
#define SHOT_IGNORE_OWNER(list, owner) test_ignore_owner(list, owner)
#include "../shots.c"

int role_host = 1, have_remote = 1, my_slot;
int test_background;
unsigned short cur_epoch = 7;
BYTE *local_player, *puppets[MAX_PLAYERS];

struct fake_gob {
    BYTE gob[0xc0], info[0xc0], state[0x60], physics[0x120], body[0xe0], desc[0x58];
    int type, cls, killed, foreign, own, mirrored, live;
    unsigned int handle, netid;
};
static struct fake_gob gobs[64];
static int ngobs, sent, launches, creates, auto_launch, fail_create;
static struct event last;
static BYTE *last_created;
static BYTE *created_parent;
static DWORD ignored_owner;

static struct fake_gob *fake(BYTE *gob)
{
    int i;
    for (i = 0; i < ngobs; i++)
        if (gobs[i].gob == gob)
            return &gobs[i];
    return NULL;
}

static BYTE *new_gob(int type, int cls)
{
    struct fake_gob *g = &gobs[ngobs++];
    assert(ngobs <= 64);
    memset(g, 0, sizeof(*g));
    g->handle = 0x10000u + ngobs;
    g->type = type;
    g->cls = cls;
    g->live = 1;
    *(BYTE **)(g->gob + GOB_INFO) = g->info;
    *(BYTE **)(g->info + SHOT_STATE) = g->state;
    *(BYTE **)(g->gob + SHOT_PHYSICS) = g->physics;
    *(BYTE **)(g->gob + SHOT_BODY) = g->body;
    *(BYTE **)g->state = g->desc;
    if (type == 417)
        *(DWORD *)(g->desc + 0x54) = SHOT_BOUNCES;
    *(DWORD *)(g->state + SHOT_OWNER) = 0xffffffffu;
    *(DWORD *)(g->state + SHOT_TARGET) = 0xffffffffu;
    *(float *)(g->state + 0x10) = 8.0f;
    *(float *)(g->gob + GOB_ORIENTATION) = 1.0f;
    return g->gob;
}

int gob_slot(BYTE *gob) { return fake(gob) ? (int)(fake(gob) - gobs) : -1; }
unsigned int gob_handle(int slot) { return gobs[slot].handle; }
BYTE *gob_by_handle(unsigned int handle)
{
    int i;
    for (i = 0; i < ngobs; i++)
        if (gobs[i].live && gobs[i].handle == handle)
            return gobs[i].gob;
    return NULL;
}
int gob_type(BYTE *gob) { return fake(gob)->type; }
int gob_class(BYTE *gob) { return fake(gob)->cls; }
int gob_alive(BYTE *gob) { return fake(gob) && fake(gob)->live && !fake(gob)->killed; }
int gob_doomed(BYTE *gob) { return fake(gob)->killed; }
int gob_is_projectile(BYTE *gob) { return gob && gob_type(gob) >= 400 && gob_type(gob) <= 499; }
int gob_is_foreign(BYTE *gob) { return gob && fake(gob)->foreign; }
int gob_is_own_shot(BYTE *gob) { return gob && fake(gob)->own; }
int gob_is_mirrored(BYTE *gob) { return gob && fake(gob)->mirrored; }
void gob_set_own_shot(BYTE *gob, int on) { fake(gob)->own = on; }
void gob_set_foreign(BYTE *gob, int on) { fake(gob)->foreign = on; }
void gob_kill(BYTE *gob) { fake(gob)->killed = 1; }
int puppet_slot(BYTE *gob)
{
    int s;
    for (s = 0; gob && s < MAX_PLAYERS; s++)
        if (puppets[s] == gob)
            return s;
    return -1;
}
void gob_peername(BYTE *gob, char *out, int size)
{
    if (gob == local_player && gob)
        snprintf(out, size, "@p%d", my_slot + 1);
    else if (puppet_slot(gob) >= 0)
        snprintf(out, size, "@p%d", puppet_slot(gob) + 1);
    else
        out[0] = 0;
}
BYTE *gob_find_peername(const char *name)
{
    int s = atoi(name + 2) - 1;
    return !name[0] ? NULL : s == my_slot ? local_player : puppets[s];
}
BYTE *ent_gob(unsigned int netid)
{
    int i;
    for (i = 0; i < ngobs; i++)
        if (gobs[i].netid == netid)
            return gobs[i].gob;
    return NULL;
}
void event_init(struct event *e, int kind) { memset(e, 0, sizeof(*e)); e->kind = kind; e->epoch = cur_epoch; }
void event_send(const struct event *e, int to) { assert(to == TO_ALL); sent++; last = *e; }
void logmsg(const char *fmt, ...) { (void)fmt; }
void *hook_code(BYTE *fn, const BYTE *expect, int len, void *hook) { return NULL; }
static void test_ignore_owner(void *list, DWORD owner) { ignored_owner = owner; }
static void test_position(void *gob, float x, float y, float z)
{
    float pos[] = { x, y, z };
    memcpy((BYTE *)gob + GOB_POSITION, pos, sizeof(pos));
}

static void fake_launch(BYTE *gob, BYTE *shooter, BYTE *target, float *direction)
{
    struct fake_gob *g = fake(gob);
    const float velocity[] = { 3.0f, -4.0f, 12.5f };
    assert(shooter);
    launches++;
    *(DWORD *)(g->state + SHOT_ACTIVE) = 1;
    *(DWORD *)(g->state + SHOT_OWNER) = fake(shooter)->handle;
    *(DWORD *)(g->state + SHOT_TARGET) = target ? fake(target)->handle : 0xffffffffu;
    memcpy(g->type == 417 ? g->body + 0xb4 : g->physics + 4, velocity, sizeof(velocity));
    if (direction)
        memcpy(gob + GOB_ORIENTATION, direction, 4 * sizeof(float));
}

static BYTE *fake_create(const char *name, int type, void *scene, BYTE *parent, int slot,
                         float *pos, float *quat, const char *model, DWORD a8, DWORD a9, DWORD a10, DWORD a11)
{
    BYTE *gob;
    creates++;
    if (fail_create)
        return NULL;
    gob = last_created = new_gob(type, type == 450 ? 1 : SHOT_CLASS);
    created_parent = parent;
    if (pos)
        memcpy(gob + GOB_POSITION, pos, 3 * sizeof(float));
    if (quat)
        memcpy(gob + GOB_ORIENTATION, quat, 4 * sizeof(float));
    if (auto_launch && type != 406)
        launch_hook(gob, gob_by_handle(a8), parent, quat);
    return gob;
}

int main(void)
{
    const float pos[] = { 11.0f, 22.0f, 33.0f }, quat[] = { 0.5f, 0.5f, -0.5f, 0.5f };
    BYTE *enemy, *grenade, *peer_enemy, *peer_player;
    struct event thrown, immediate, legacy;
    int before;

    create_orig = fake_create;
    launch_orig = fake_launch;
    local_player = new_gob(100, 2);
    enemy = new_gob(207, 4);

    /* A grenade begins attached, with neither transform argument. No premature FIRE. */
    grenade = create_hook("", 417, (void *)2, enemy, 3, NULL, NULL, NULL, 0, 0, 0, 0);
    assert(sent == 0 && !fake(grenade)->own);
    test_position(grenade, pos[0], pos[1], pos[2]);
    *(float *)(fake(grenade)->state + 8) = 0.25f;
    *(DWORD *)(fake(grenade)->state + 0xc) = 123;
    *(float *)(fake(grenade)->state + 0x14) = 0.125f;
    launch_hook(grenade, enemy, local_player, (float *)quat);
    assert(sent == 1 && last.objtype == 417 && last.part == FIRE_LAUNCHED);
    assert(sizeof(struct event) == 128);
    assert(offsetof(struct event, shot.velocity) == offsetof(struct event, from));
    assert(offsetof(struct event, shot.state) == offsetof(struct event, from) + 12);
    assert(last.extra[0] == fake(enemy)->handle && !strcmp(last.name, "@p1"));
    assert(!memcmp(last.pos, pos, sizeof(pos)) && !memcmp(last.quat, quat, sizeof(quat)));
    assert(!memcmp(last.shot.velocity, fake(grenade)->body + 0xb4, 3 * sizeof(float)));
    thrown = last;

    /* A fireball launched inside its constructor gets exactly one event. */
    auto_launch = 1;
    before = sent;
    create_hook("", 403, MAIN_SCENE, local_player, -1, (float *)pos, (float *)quat, NULL,
                fake(enemy)->handle, 0, 0, 0);
    assert(sent == before + 1 && last.part == FIRE_LAUNCHED);
    assert(!memcmp(last.shot.velocity, fake(last_created)->physics + 4, 3 * sizeof(float)));
    immediate = last;
    auto_launch = 0;

    /* Replay translates enemy/player identities without rerunning native shooter scripts,
     * restores velocity/fuse and marks it foreign without sending another event. */
    role_host = 0;
    my_slot = 1;
    peer_player = local_player;
    local_player = new_gob(100, 2);
    puppets[0] = peer_player;
    peer_enemy = new_gob(207, 4);
    fake(peer_enemy)->netid = fake(enemy)->handle;
    before = sent;
    launches = 0;
    shots_replay(&thrown);
    assert(launches == 0 && sent == before && fake(last_created)->foreign && created_parent == NULL);
    assert(ignored_owner == fake(peer_enemy)->handle);
    assert(*(DWORD *)(fake(last_created)->state + SHOT_OWNER) == fake(peer_enemy)->handle);
    assert(*(DWORD *)(fake(last_created)->state + SHOT_TARGET) == fake(peer_player)->handle);
    assert(!memcmp(last_created + GOB_POSITION, pos, sizeof(pos)));
    assert(!memcmp(last_created + GOB_ORIENTATION, quat, sizeof(quat)));
    assert(!memcmp(fake(last_created)->body + 0xb4, thrown.shot.velocity, 3 * sizeof(float)));
    assert(!memcmp(fake(last_created)->state + SHOT_ACTIVE, thrown.shot.state, sizeof(thrown.shot.state)));
    auto_launch = 1;
    launches = 0;
    shots_replay(&immediate);
    assert(launches == 0 && sent == before);    /* suppress constructor launch on replay too */
    assert(!memcmp(fake(last_created)->physics + 4, immediate.shot.velocity, 3 * sizeof(float)));
    auto_launch = 0;

    /* Mirrored enemy animation throws do not become client-owned shots. */
    grenade = create_hook("", 417, MAIN_SCENE, peer_enemy, 3, NULL, NULL, NULL, 0, 0, 0, 0);
    launch_hook(grenade, peer_enemy, local_player, (float *)quat);
    assert(sent == before && fake(grenade)->killed && fake(grenade)->foreign && !fake(grenade)->own);
    role_host = 1;
    fake(peer_enemy)->mirrored = 1;
    launch_hook(grenade, peer_enemy, NULL, (float *)quat);
    assert(sent == before);
    fake(peer_enemy)->mirrored = 0;
    launch_hook(grenade, last_created, NULL, (float *)quat);
    assert(sent == before);    /* a foreign projectile cannot become an authoritative shooter */
    role_host = 0;

    /* A client's own grenade gets its player marker; the host resolves its puppet. */
    grenade = create_hook("", 417, MAIN_SCENE, local_player, -1, NULL, NULL, NULL, 0, 0, 0, 0);
    launch_hook(grenade, local_player, NULL, (float *)quat);
    assert(sent == before + 1 && last.extra[0] == (OWNER_PLAYER | 1) && fake(grenade)->own);
    thrown = last;
    role_host = 1;
    my_slot = 0;
    puppets[1] = local_player;
    local_player = peer_player;
    puppets[0] = NULL;
    shots_replay(&thrown);
    assert(*(DWORD *)(fake(last_created)->state + SHOT_OWNER) == fake(puppets[1])->handle);
    before = sent;
    grenade = create_hook("", 417, MAIN_SCENE, puppets[1], -1, NULL, NULL, NULL, 0, 0, 0, 0);
    launch_hook(grenade, puppets[1], NULL, (float *)quat);
    assert(sent == before && fake(grenade)->killed);

    /* Other projectile classes retain the original create-argument format. */
    create_hook("", 450, MAIN_SCENE, local_player, -1, (float *)pos, (float *)quat, NULL, 0, 1, 2, 3);
    assert(sent == before + 1 && last.part == 0 && last.extra[3] == 3);
    legacy = last;
    shots_replay(&legacy);
    assert(fake(last_created)->foreign && sent == before + 1);

    /* Stale levels are dropped; a missing shooter still permits a harmless visual replay. */
    before = creates;
    thrown.epoch--;
    shots_replay(&thrown);
    assert(creates == before);
    thrown.epoch++;
    thrown.extra[0] = 0x12345678;
    launches = 0;
    ignored_owner = 0;
    shots_replay(&thrown);
    assert(creates == before + 1 && launches == 0 && ignored_owner == 0);
    assert(*(DWORD *)(fake(last_created)->state + SHOT_OWNER) == 0xffffffffu);
    assert(fake(last_created)->foreign);
    /* Recycling an engine allocation before the next tick must not inherit foreign flags. */
    grenade = last_created;
    fake(grenade)->handle += 0x10000u;
    before = sent;
    launch_hook(grenade, local_player, NULL, (float *)quat);
    assert(sent == before + 1 && !fake(grenade)->foreign && fake(grenade)->own);
    fail_create = 1;
    before = sent;
    assert(!create_hook("", 417, MAIN_SCENE, NULL, -1, NULL, NULL, NULL, 0, 0, 0, 0));
    assert(sent == before);
    fail_create = 0;
    auto_launch = 1;
    have_remote = 0;
    grenade = create_hook("", 417, MAIN_SCENE, NULL, -1, (float *)pos, (float *)quat, NULL,
                         fake(local_player)->handle, 0, 0, 0);
    assert(sent == before && !fake(grenade)->foreign && !fake(grenade)->killed);
    have_remote = 1;
    auto_launch = 0;
    before = sent;
    shots_test_sniper();
    assert(sent == before);    /* no automation actions in normal gameplay */
    test_background = 1;
    shots_test_sniper();
    assert(sent == before + 1 && last.objtype == 406 && fake(last_created)->own);
    assert(last.pos[2] == *(float *)(local_player + GOB_POSITION + 8) + 1.0f);
    fake(last_created)->live = 0;
    fake(grenade)->live = 0;
    shots_frame();
    assert(!fake(last_created)->own);
    shots_reset();
    assert(!nown && !nforeign);
    puts("PASS: delayed grenades, constructor launches, replay state, ownership, duplicate suppression, legacy shots, lifecycle");
    return 0;
}
