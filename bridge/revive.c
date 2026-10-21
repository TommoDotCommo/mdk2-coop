/*
 * Going down, reviving, dying and spectating.
 *
 * A hit that would kill us, with teammates in the game, puts us DOWNED: we lie where we fell
 * (the camera stays on us) for DOWN_MS, during which a teammate who holds the revive key (R)
 * beside us for REVIVE_MS stands us up with half our health. The clock stops while someone is
 * reviving. After that we are DEAD: we spectate a living teammate (click: the next one) and
 * come back next to them when the host finds no enemy standing in our zone (the room our body
 * lies in and the rooms visible from it, rooms.c) or a checkpoint is reached (which also
 * stands up whoever is down). Lava, black
 * holes and falling out of the world leave no body to reach: straight to DEAD. With nobody
 * else alive the host reloads the checkpoint for everyone; alone, the game's own death.
 *
 * Downed pose: the game's own death (ANIM_DIE, 17) cannot be used, since stopping its death
 * sequence (mdkDeathCancel) reloads the save, and omAnimPlay on a living player is overridden
 * by the player's own think. So our player is marked mirrored (its think and anim-loop slots
 * are skipped, game.c) and the death animation's state is applied every frame, played once
 * and then held. Other games see it through the normal animation sync.
 *
 * Spectating: the camera follows our player, so our (hidden, inert, untargetable) body rides
 * RIDE_BEHIND units behind the teammate we watch. Other games are told (life byte
 * LIFE_SPECTATING) and park our puppet out of the way.
 *
 * Lua (coop_dev.lua, coop.ReviveTick) carries out what needs the game's own calls: player
 * control, targetable, visible, the game's death, the reload; coop_revive_tick() hands it
 * one action at a time.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bridge.h"

#define DOWN_MS         30000.0     /* downed this long, unless someone is reviving */
#define REVIVE_MS       3000.0      /* hold the revive key this long */
#define REVIVE_RANGE    5.0f        /* units from the downed player */
#define REVIVE_SLACK    3.0f        /* the downed side allows this much more (positions lag) */
#define RIDE_DELAY_MS   2500.0      /* a dead player sees their body this long before spectating */
#define RIDE_BEHIND     3.0f
#define ZONE_CLEAR_MS   2000.0      /* host: a dead player's zone empty this long */
#define WIPE_MS         3000.0      /* host: nobody alive this long, then reload */
/* the revive key (R) is set on the Controls page: input.c gamekey_down(GK_REVIVE) */
#define KEY_NEXT        256         /* left mouse: next teammate to watch */
#define UNREACHABLE     (64 | 128 | 256 | 2048)   /* DAMAGE_LAVAFALL, LAVAFLOAT, BLACKHOLE, LAVADEATH */
#define ANIM_DIE        17
#define DIE_RATE        1.0714f     /* play position per second (the game's own state block) */
#define DIE_HOLD        0.98f       /* last frame */
#define LIFE_SPECTATING 3           /* wire only: dead and riding along with someone */

static int life = LIFE_ALIVE;
static double life_since, last_frame, down_ms;
static int riding, reviving = -1, revive_lock, spect = -1, fire_was;
static double revive_from;
static float body[3];               /* where we went down or died */
static const char *actions[8];      /* for Lua, oldest first */
static int nactions;

/* host: dead players' bodies and how long their zone has been clear */
static float dead_pos[MAX_PLAYERS][3];
static int dead_known[MAX_PLAYERS];
static double clear_since[MAX_PLAYERS], wipe_since;
static int wiped, force_reload;

static const char *life_name[] = { "alive", "downed", "dead", "spectating" };

static void act(const char *a)
{
    if (nactions < 8)
        actions[nactions++] = a;
}

static float *pos_of(int slot)
{
    if (slot == my_slot)
        return gob_alive(local_player) ? (float *)(local_player + GOB_POSITION) : NULL;
    return slot >= 0 && slot < MAX_PLAYERS && gob_alive(puppets[slot]) ? (float *)(puppets[slot] + GOB_POSITION) : NULL;
}

static float dist(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int life_of(int slot)
{
    int l = slot == my_slot ? revive_life() : player_life(slot);
    return l == LIFE_SPECTATING ? LIFE_DEAD : l;
}

/* a living teammate: the next one round after `after`, else the nearest to p (any if p is
 * NULL); -1 if none */
static int nearest_alive(const float *p, int after)
{
    int s, best = -1;
    float bd = 0.0f;

    for (s = 0; s < MAX_PLAYERS; s++) {
        int k = after >= 0 ? (after + 1 + s) % MAX_PLAYERS : s;
        float *q;
        if (k == my_slot || !player_connected(k) || life_of(k) != LIFE_ALIVE || !(q = pos_of(k)))
            continue;
        if (after >= 0)
            return k;
        if (best < 0 || (p && dist(p, q) < bd)) {
            best = k;
            bd = p ? dist(p, q) : 0.0f;
        }
    }
    return best;
}

/* ---- the pose ---- */

static void pose(int on)
{
    if (!gob_alive(local_player))
        return;
    gob_set_mirrored(local_player, on);
    if (on)
        class_filter_think(gob_class(local_player));
}

static void pose_apply(void)
{
    struct snap s;
    float phase = (float)((now_ms() - life_since) / 1000.0) * DIE_RATE, one = 1.0f, four = 4.0f, rate = DIE_RATE;
    DWORD *st = s.anims[0].state;

    if (phase > DIE_HOLD)
        phase = DIE_HOLD;
    memset(&s, 0, sizeof(s));
    s.cur = ANIM_DIE;
    s.prev = NO_ANIM;
    s.nanims = 1;
    s.anims[0].id = ANIM_DIE;
    st[0] = 1;
    st[1] = 1;
    st[2] = 3;                          /* ANIMSTYLE_PLAYONCE */
    memcpy(&st[ANIM_TIME0], &phase, 4);
    memcpy(&st[ANIM_TIME1], &phase, 4);
    memcpy(&st[5], &rate, 4);
    memcpy(&st[ANIM_WEIGHT], &one, 4);
    memcpy(&st[7], &four, 4);
    st[9] = 1;
    anim_apply(local_player, &s);
}

/* ---- our life ---- */

static void tell_host_died(void)
{
    struct event e;

    if (role_host) {
        memcpy(dead_pos[my_slot], body, sizeof(body));
        dead_known[my_slot] = 1;
        clear_since[my_slot] = 0.0;
        return;
    }
    event_init(&e, EV_DIED);
    e.type = my_slot;
    memcpy(e.pos, body, sizeof(e.pos));
    event_send(&e, 0);
}

static void set_life(int l, const char *why)
{
    int was = life;

    if (l == life)
        return;
    logmsg("life: %s -> %s (%s)", life_name[was], life_name[l], why);
    life = l;
    life_since = now_ms();
    if (l != LIFE_ALIVE && was == LIFE_ALIVE) {
        if (gob_alive(local_player))
            memcpy(body, local_player + GOB_POSITION, sizeof(body));
        pose(1);
        act("down");
        down_ms = 0.0;
    }
    if (l == LIFE_DEAD) {
        spect = -1;
        tell_host_died();
    }
    if (l == LIFE_ALIVE) {
        pose(0);
        riding = 0;
        if (gob_alive(local_player) && local_max_hp > 0)
            gob_set_hp(local_player, local_max_hp / 2 > 0 ? local_max_hp / 2 : 1);
        act("up");
        hud_status(NULL, NULL, -1.0f);
    }
    reviving = -1;
}

int revive_life(void)
{
    return life == LIFE_DEAD && riding ? LIFE_SPECTATING : life;
}

int revive_reviving(void)
{
    return reviving;
}

/* lua.c, damage to our player: 1 if it is ours to handle (not applied) */
int revive_hit(int damage, int type)
{
    int hp = gob_hp(local_player);

    if (life != LIFE_ALIVE)
        return 1;                       /* down or dead: nothing more happens to us */
    if (damage < hp || hp <= 0 || !have_remote || players_connected() == 0)
        return 0;
    logmsg("down: %d damage (type 0x%x) on %d hp", damage, type, hp);
    if (type & UNREACHABLE)
        set_life(LIFE_DEAD, "no body to reach");
    else
        set_life(LIFE_DOWNED, "lethal hit");
    return 1;
}

/* rooms.c: fell out of the world */
void revive_unreachable(const char *why)
{
    if (life != LIFE_DEAD && have_remote && players_connected() > 0)
        set_life(LIFE_DEAD, why);
}

/* ---- events ---- */

static void respawn(const char *why)
{
    int s = spect >= 0 && life_of(spect) == LIFE_ALIVE ? spect : nearest_alive(body, -1);
    float *p = s >= 0 ? pos_of(s) : NULL;

    if (p && gob_alive(local_player))
        gob_set_position(local_player, p[0], p[1], p[2]);
    set_life(LIFE_ALIVE, why);
}

void revive_event(const struct event *e, int from)
{
    int target = e->damage;

    if (e->kind == EV_DIED) {           /* host: where a player's body is */
        if (role_host && e->type > 0 && e->type < MAX_PLAYERS) {
            memcpy(dead_pos[e->type], e->pos, sizeof(e->pos));
            dead_known[e->type] = 1;
            clear_since[e->type] = 0.0;
            logmsg("revive: player %d died at (%.1f, %.1f, %.1f)", e->type + 1, e->pos[0], e->pos[1], e->pos[2]);
        }
        return;
    }
    if (role_host && target != my_slot) {
        if (target > 0 && target < MAX_PLAYERS)
            event_send(e, target);      /* for a client: pass it on */
        return;
    }
    if (e->kind == EV_REVIVE && life == LIFE_DOWNED) {
        int by = e->type;
        float *me = pos_of(my_slot), *them = pos_of(by);
        char why[64];

        if (me && them && dist(me, them) > REVIVE_RANGE + REVIVE_SLACK) {
            logmsg("revive: player %d is %.1f away, too far", by + 1, dist(me, them));
            return;
        }
        snprintf(why, sizeof(why), "revived by player %d", by + 1);
        set_life(LIFE_ALIVE, why);
    } else if (e->kind == EV_RESPAWN && life == LIFE_DEAD) {
        respawn(e->type ? "a checkpoint was reached" : "our zone is clear");
    } else if (e->kind == EV_RESPAWN && life == LIFE_DOWNED && e->type) {
        set_life(LIFE_ALIVE, "a checkpoint was reached");      /* stand up where we lie */
    }
    (void)from;
}

/* host: the game reached a checkpoint; the dead come back and the downed stand up */
void revive_checkpoint(void)
{
    int s;

    if (!role_host)
        return;
    for (s = 0; s < MAX_PLAYERS; s++) {
        if ((s != my_slot && !player_connected(s)) || life_of(s) == LIFE_ALIVE)
            continue;
        if (s == my_slot) {
            if (life == LIFE_DEAD)
                respawn("a checkpoint was reached");
            else
                set_life(LIFE_ALIVE, "a checkpoint was reached");
        } else {
            struct event e;
            event_init(&e, EV_RESPAWN);
            e.damage = s;
            e.type = 1;
            event_send(&e, s);
            logmsg("revive: checkpoint, player %d back", s + 1);
        }
        dead_known[s] = 0;
    }
}

/* ---- every frame ---- */

static void host_frame(double now)
{
    int s, anyone = 0, alive = 0;

    for (s = 0; s < MAX_PLAYERS; s++) {
        int l;
        if (s != my_slot && !player_connected(s))
            continue;
        l = life_of(s);
        anyone++;
        if (l == LIFE_ALIVE) {
            alive++;
            dead_known[s] = 0;
        }
        if (l != LIFE_DEAD || !dead_known[s])
            continue;
        if (ents_alive_in_zone(dead_pos[s]) > 0) {
            clear_since[s] = 0.0;
            continue;
        }
        if (clear_since[s] == 0.0)
            clear_since[s] = now;
        if (now - clear_since[s] < ZONE_CLEAR_MS)
            continue;
        dead_known[s] = 0;
        if (s == my_slot) {
            respawn("our zone is clear");
        } else {
            struct event e;
            event_init(&e, EV_RESPAWN);
            e.damage = s;
            event_send(&e, s);
            logmsg("revive: player %d's zone is clear: back", s + 1);
        }
    }
    /* nobody standing: reload the checkpoint for everyone (the level start resets us all) */
    if (anyone > 1 && alive == 0 && !wiped) {
        if (wipe_since == 0.0)
            wipe_since = now;
        if (now - wipe_since >= WIPE_MS) {
            logmsg("revive: everyone is down: reloading the checkpoint");
            act("wipe");
            wiped = 1;
            force_reload = 1;           /* clients at this very checkpoint load it too */
        }
    } else if (alive > 0) {
        wipe_since = 0.0;
    }
}

static void reviver_frame(double now)
{
    float *me = pos_of(my_slot);
    int s, who = -1, key = gamekey_down(GK_REVIVE);
    float nd = 0.0f;
    char line[96];

    if (!me)
        return;
    if (!key)
        revive_lock = 0;
    for (s = 0; s < MAX_PLAYERS; s++) {
        float *q;
        if (s == my_slot || !player_connected(s) || player_life(s) != LIFE_DOWNED || !(q = pos_of(s)))
            continue;
        if (who < 0 || dist(me, q) < nd) {
            who = s;
            nd = dist(me, q);
        }
    }
    if (who < 0) {
        hud_status(NULL, NULL, -1.0f);
        reviving = -1;
        return;
    }
    if (nd <= REVIVE_RANGE && key && !revive_lock) {
        if (reviving != who) {
            reviving = who;
            revive_from = now;
            logmsg("revive: reviving player %d (%.1f away)", who + 1, nd);
        }
        if (now - revive_from >= REVIVE_MS) {
            struct event e;
            event_init(&e, EV_REVIVE);
            e.damage = who;
            e.type = my_slot;
            event_send(&e, role_host ? who : 0);
            logmsg("revive: player %d revived", who + 1);
            reviving = -1;
            revive_lock = 1;            /* let go of the key before the next one */
            hud_status(NULL, NULL, -1.0f);
            return;
        }
        snprintf(line, sizeof(line), "Reviving %s", player_name(who));
        hud_status(line, NULL, (float)((now - revive_from) / REVIVE_MS));
        return;
    }
    reviving = -1;
    snprintf(line, sizeof(line), "%s is down", player_name(who));
    if (nd <= REVIVE_RANGE) {
        hud_status(line, "Hold R to revive", -1.0f);
    } else {
        char l2[64];
        snprintf(l2, sizeof(l2), "%.0f away: get next to them to revive", nd);
        hud_status(line, l2, -1.0f);
    }
}

static void downed_frame(double dt)
{
    int s, by = -1;
    char l1[96], l2[96];

    for (s = 0; s < MAX_PLAYERS; s++)
        if (s != my_slot && player_connected(s) && player_reviving(s) == my_slot && player_life(s) == LIFE_ALIVE)
            by = s;
    if (by < 0)
        down_ms += dt;              /* the clock stops while someone is reviving us */
    pose_apply();
    if (down_ms >= DOWN_MS) {
        set_life(LIFE_DEAD, "bled out");
        return;
    }
    snprintf(l1, sizeof(l1), "You're down: %d s", (int)((DOWN_MS - down_ms) / 1000.0) + 1);
    if (by >= 0)
        snprintf(l2, sizeof(l2), "%s is reviving you", player_name(by));
    else
        snprintf(l2, sizeof(l2), "A teammate can revive you");
    hud_status(l1, l2, -1.0f);
}

static void dead_frame(double now)
{
    int fire = input_down(KEY_NEXT, VK_LBUTTON);
    char l1[96];
    float *t, *q;

    if (now - life_since < RIDE_DELAY_MS) {
        pose_apply();
        hud_status("You died", "Back when your area is clear or at a checkpoint", -1.0f);
        fire_was = fire;
        return;
    }
    if (spect < 0 || life_of(spect) != LIFE_ALIVE || !player_connected(spect))
        spect = nearest_alive(body, -1);
    else if (fire && !fire_was)
        spect = nearest_alive(NULL, spect);
    fire_was = fire;
    if (spect < 0 || !(t = pos_of(spect))) {
        if (!riding)
            pose_apply();
        hud_status("Everyone is down", NULL, -1.0f);
        return;
    }
    if (!riding) {
        riding = 1;
        act("hide");
        logmsg("revive: spectating player %d", spect + 1);
    }
    /* RIDE_BEHIND behind them: their forward is +y turned by their rotation (w, x, y, z) */
    q = (float *)(puppets[spect] + GOB_ORIENTATION);
    {
        float w = q[0], x = q[1], y = q[2], z = q[3];
        float fx = 2.0f * (x * y - w * z), fy = 1.0f - 2.0f * (x * x + z * z);
        float n = sqrtf(fx * fx + fy * fy);
        if (n > 0.001f) {
            fx /= n;
            fy /= n;
        }
        gob_set_position(local_player, t[0] - fx * RIDE_BEHIND, t[1] - fy * RIDE_BEHIND, t[2]);
        memcpy(local_player + GOB_ORIENTATION, q, 16);
    }
    snprintf(l1, sizeof(l1), "Dead: watching %s", player_name(spect));
    hud_status(l1, "Back when your area is clear or at a checkpoint. Click: next player", -1.0f);
}

void revive_frame(void)
{
    double now = now_ms(), dt = last_frame > 0.0 ? now - last_frame : 0.0;

    last_frame = now;
    if (dt > 250.0)
        dt = 250.0;                     /* a load or a pause does not bleed us out */
    if (!gob_alive(local_player) || cur_level < 1)
        return;
    if (life != LIFE_ALIVE && (!have_remote || players_connected() == 0)) {
        /* nobody left to help: the game's own death */
        set_life(LIFE_ALIVE, "alone");
        act("die");
        return;
    }
    if (role_host)
        host_frame(now);
    if (life == LIFE_ALIVE)
        reviver_frame(now);
    else if (life == LIFE_DOWNED)
        downed_frame(dt);
    else
        dead_frame(now);
}

/* lua.c, host announcing a level load: 1 if it is the wipe's reload (once) */
int revive_take_forced(void)
{
    int f = force_reload;
    force_reload = 0;
    return f;
}

/* Lua, every frame: the next thing to do with the game's own calls, or NULL */
const char *revive_tick(void)
{
    const char *a;

    if (!nactions)
        return NULL;
    a = actions[0];
    memmove(actions, actions + 1, --nactions * sizeof(actions[0]));
    return a;
}

void revive_level_reset(void)
{
    if (life != LIFE_ALIVE) {
        life = LIFE_ALIVE;
        pose(0);
        act("up");
        logmsg("life: alive (new level)");
    }
    riding = 0;
    reviving = -1;
    spect = -1;
    memset(dead_known, 0, sizeof(dead_known));
    wipe_since = 0.0;
    wiped = 0;
    hud_status(NULL, NULL, -1.0f);
}
