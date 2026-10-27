/*
 * Diagnostics, so a report from a machine nobody can touch is enough to find the fault.
 *
 *  - Session logs: every run writes its own coop\logs\<UTC start>_p<pid>.log, opened for appending only and
 *    never replaced (bridge.log is still rewritten each run, for the test tools). A line is
 *    "<n> <utc ms> <chain> <text>"; chain is FNV-1a/64 of "<n> <utc ms> <text>" continued from the previous
 *    line's chain, so a missing, cut or edited line breaks every chain value after it (tools/diag_report.py
 *    checks). That shows accidents, not forgery. "session end: clean exit" closes a run that ended normally;
 *    <session>.crash marks one that crashed and keeps it from being pruned (the logs folder is capped).
 *  - Flight recorder: the last 60 s at 10 Hz (frame rate, level, where every player is, round trip, jitter and
 *    loss to each link) and the last log lines, in memory. It is written to <session>.flight-<marker>-<why>.txt
 *    on a crash, a desync, or when anyone presses F2 (the existing mark, which every game already receives): each
 *    machine dumps the same moment and the files line up by the mark's number.
 *  - Desync check: once a second the host sends what it believes (level, epoch, players, enemies, who is down
 *    or dead) in an EV_DIAG; a client that disagrees for 4 samples in a row logs which field, dumps, and tells
 *    the host, which dumps too.
 *  - The launcher's "Send diagnostics": runs the embedded diag.ps1 (dist/diag.ps1) hidden, which zips logs,
 *    settings and system facts, redacted, to the desktop.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "bridge.h"
#include "diag_ps1.h"

#define FNV_BASIS       0xcbf29ce484222325ULL
#define FNV_PRIME       0x100000001b3ULL
#define LINE_MAX_TEXT   1000
#define LOG_CAP_MB      200
#define LOG_RING        256
#define LOG_RING_TEXT   200
#define FLIGHT_N        600             /* 60 s at 10 Hz */
#define SAMPLE_MS       100.0
#define WORLD_MS        1000.0
#define NETQ_MS         5000.0
#define DESYNC_RUN      4               /* disagreeing samples in a row before it counts */
#define MAX_DUMPS       12

/* EV_DIAG parts */
enum { DP_WORLD, DP_DESYNC };

static CRITICAL_SECTION lock;
static int lock_ready;
static HANDLE sess_file = INVALID_HANDLE_VALUE;
static unsigned long long chain = FNV_BASIS;
static unsigned int line_no;
static char logs_dir[MAX_PATH], sess_base[MAX_PATH], sess_key[64];

static struct { double utc; char text[LOG_RING_TEXT]; } log_ring[LOG_RING];
static unsigned int log_ring_n;

static double utc_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static unsigned long long fnv(unsigned long long h, const char *s, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ---- session log ---- */

static void sess_write(const char *text)
{
    char payload[LINE_MAX_TEXT + 64], out[LINE_MAX_TEXT + 96];
    int n, m, i;
    DWORD w;

    if (sess_file == INVALID_HANDLE_VALUE)
        return;
    n = snprintf(payload, sizeof(payload), "%u %.0f ", line_no, utc_ms());
    for (i = 0; text[i] && n < (int)sizeof(payload) - 1 && n < LINE_MAX_TEXT + 40; i++)
        payload[n++] = text[i] == '\r' || text[i] == '\n' ? ' ' : text[i];
    payload[n] = 0;
    chain = fnv(chain, payload, n);
    {   /* "<n> <utc> " then the chain, then the text: the chain covers the text as logged */
        const char *sp1 = strchr(payload, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        int head = sp2 ? (int)(sp2 + 1 - payload) : n;
        m = snprintf(out, sizeof(out), "%.*s%08lx%08lx %s\n", head, payload, (unsigned long)(chain >> 32),
                     (unsigned long)chain, payload + head);
    }
    WriteFile(sess_file, out, m, &w, NULL);
    line_no++;
}

struct logfile { char name[80]; DWORD size; };

/* keep the logs folder under the cap: the oldest sessions go first, a session that crashed or the running one never */
static void prune(unsigned long long cap)
{
    static struct logfile files[4096];
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;
    unsigned long long total = 0;

    snprintf(pattern, MAX_PATH, "%s\\*", logs_dir);
    if ((h = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n >= 4096 || strlen(fd.cFileName) >= sizeof(files[0].name))
            continue;
        snprintf(files[n].name, sizeof(files[n].name), "%s", fd.cFileName);
        files[n].size = fd.nFileSizeLow;
        total += fd.nFileSizeLow;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    while (total > cap) {
        int best = -1;
        for (i = 0; i < n; i++) {
            char key[80], crash[MAX_PATH];
            int k, j;
            if (!files[i].name[0])
                continue;
            for (k = 0; files[i].name[k] && files[i].name[k] != '.'; k++)
                key[k] = files[i].name[k];
            key[k] = 0;
            if (!strcmp(key, sess_key))
                continue;
            snprintf(crash, MAX_PATH, "%s.crash", key);
            for (j = 0; j < n && strcmp(files[j].name, crash); j++)
                ;
            if (j < n)
                continue;
            if (best < 0 || strcmp(files[i].name, files[best].name) < 0)
                best = i;
        }
        if (best < 0)
            return;
        snprintf(path, MAX_PATH, "%s\\%s", logs_dir, files[best].name);
        DeleteFileA(path);
        total -= files[best].size;
        files[best].name[0] = 0;
    }
}

void diag_log_open(void)
{
    SYSTEMTIME st;
    char path[MAX_PATH], ini[MAX_PATH];
    int cap_mb;

    InitializeCriticalSection(&lock);
    lock_ready = 1;
    snprintf(logs_dir, MAX_PATH, "%s\\logs", coop_dir);
    CreateDirectoryA(logs_dir, NULL);
    GetSystemTime(&st);
    snprintf(sess_key, sizeof(sess_key), "%04d-%02d-%02dT%02d-%02d-%02dZ_p%lu", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, GetCurrentProcessId());
    snprintf(sess_base, MAX_PATH, "%s\\%s", logs_dir, sess_key);
    snprintf(path, MAX_PATH, "%s.log", sess_base);
    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    cap_mb = GetPrivateProfileIntA("diag", "max_log_mb", LOG_CAP_MB, ini);
    prune((unsigned long long)(cap_mb < 5 ? 5 : cap_mb) * 1024 * 1024);
    /* appending only, others may read it while the game runs, nobody can delete it from under us */
    sess_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (sess_file == INVALID_HANDLE_VALUE)
        OutputDebugStringA("mdk2-coop: cannot open the session log; check permissions and free disk space\n");
    logmsg("session %s: mod %s", sess_key, MOD_VERSION);
}

void diag_log_close(void)
{
    if (!lock_ready)
        return;
    logmsg("session end: clean exit");
    EnterCriticalSection(&lock);
    if (sess_file != INVALID_HANDLE_VALUE)
        CloseHandle(sess_file);
    sess_file = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&lock);
}

void diag_log_line(const char *text)
{
    unsigned int i;

    if (!lock_ready)
        return;
    EnterCriticalSection(&lock);
    i = log_ring_n++ % LOG_RING;
    log_ring[i].utc = utc_ms();
    snprintf(log_ring[i].text, LOG_RING_TEXT, "%s", text);
    sess_write(text);
    LeaveCriticalSection(&lock);
}

/* ---- flight recorder ---- */

struct fplayer {
    short rtt;                  /* ms, 0 no link or unknown */
    unsigned short jitter10;    /* tenths of a ms */
    unsigned short loss10;      /* tenths of a percent, since they connected */
    unsigned char here;         /* connected, or us */
    unsigned char life;
    float pos[3];
};

struct fsample {
    double utc;
    short level, ents;
    unsigned short epoch, fps10;
    float pos[3];
    struct fplayer p[MAX_PLAYERS];
};

static struct fsample flight[FLIGHT_N];
static unsigned int flight_n;
static int dumps;

static void sample(double now, int frames, double dt)
{
    struct fsample *f = &flight[flight_n++ % FLIGHT_N];
    int s;

    memset(f, 0, sizeof(*f));
    f->utc = utc_ms();
    f->level = (short)cur_level;
    f->epoch = cur_epoch;
    f->ents = (short)ent_count();
    f->fps10 = dt > 0 ? (unsigned short)(frames * 10000.0 / dt) : 0;
    memcpy(f->pos, local_pos, sizeof(f->pos));
    for (s = 0; s < MAX_PLAYERS; s++) {
        struct fplayer *p = &f->p[s];
        int rtt;
        double jit;
        unsigned int rx, lost;
        struct snap *v;

        if (s == my_slot) {
            p->here = 1;
            memcpy(p->pos, local_pos, sizeof(p->pos));
            p->life = (unsigned char)revive_life();
            continue;
        }
        if (!player_connected(s))
            continue;
        p->here = 1;
        p->life = (unsigned char)player_life(s);
        if ((v = player_view(s, 0)))
            memcpy(p->pos, v->pos, sizeof(p->pos));
        if (player_netq(s, &rtt, &jit, &rx, &lost)) {
            p->rtt = (short)rtt;
            p->jitter10 = (unsigned short)(jit * 10);
            p->loss10 = rx + lost ? (unsigned short)(lost * 1000.0 / (rx + lost)) : 0;
        }
    }
    (void)now;
}

struct out {
    HANDLE h;
    char buf[16384];
    int n;
};

static void out_flush(struct out *o)
{
    DWORD w;

    if (o->n)
        WriteFile(o->h, o->buf, o->n, &w, NULL);
    o->n = 0;
}

static void out_f(struct out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->n > (int)sizeof(o->buf) - 1200)
        out_flush(o);
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->n, 1100, fmt, ap);
    va_end(ap);
    if (n > 1099)
        n = 1099;
    if (n > 0)
        o->n += n;
}

/* kind: "crash", "mark", "desync" or "report"; marker 0 for a crash. The file is new each time: never replaced. */
static void dump_flight(const char *kind, unsigned int marker, const char *why)
{
    static struct out o;
    static const char *const limited[] = { "desync", "report" };       /* not twice within 5 s; marks are the player's call */
    static double last_at[2];
    char path[MAX_PATH];
    unsigned int i, from, count;
    double now = now_ms();
    int k, crash = !strcmp(kind, "crash");

    if (!lock_ready)
        return;
    if (!crash) {
        if (dumps >= MAX_DUMPS)
            return;
        for (k = 0; k < 2; k++) {
            if (strcmp(limited[k], kind))
                continue;
            if (last_at[k] && now - last_at[k] < 5000.0)
                return;
            last_at[k] = now;
        }
    }
    snprintf(path, MAX_PATH, "%s.flight-%u-%s.txt", sess_base, marker, kind);
    o.h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (o.h == INVALID_HANDLE_VALUE)
        return;
    dumps++;
    o.n = 0;
    out_f(&o, "# flight recorder: kind=%s marker=%u utc=%.0f session=%s mod=%s\n", kind, marker, utc_ms(), sess_key,
          MOD_VERSION);
    out_f(&o, "# reason: %s\n", why ? why : "");
    out_f(&o, "# me: slot %d, level %d, epoch %u, %s, %d connected\n", my_slot, cur_level, cur_epoch,
          role_host ? "host" : "client", players_connected());
    out_f(&o, "# S utc level epoch fps ents x y z | slot rtt jitter loss%% life x y z ; ... (10 samples a second, oldest first)\n");
    count = flight_n < FLIGHT_N ? flight_n : FLIGHT_N;
    from = flight_n - count;
    for (i = from; i < flight_n; i++) {
        struct fsample *f = &flight[i % FLIGHT_N];
        int s;

        out_f(&o, "S %.0f %d %u %.1f %d %.2f %.2f %.2f |", f->utc, f->level, f->epoch, f->fps10 / 10.0, f->ents, f->pos[0],
              f->pos[1], f->pos[2]);
        for (s = 0; s < MAX_PLAYERS; s++) {
            struct fplayer *p = &f->p[s];
            if (!p->here)
                continue;
            out_f(&o, " %d %d %.1f %.1f %d %.2f %.2f %.2f ;", s + 1, p->rtt, p->jitter10 / 10.0, p->loss10 / 10.0, p->life,
                  p->pos[0], p->pos[1], p->pos[2]);
        }
        out_f(&o, "\n");
    }
    out_f(&o, "# log tail\n");
    count = log_ring_n < LOG_RING ? log_ring_n : LOG_RING;
    from = log_ring_n - count;
    for (i = from; i < log_ring_n; i++)
        out_f(&o, "L %.0f %s\n", log_ring[i % LOG_RING].utc, log_ring[i % LOG_RING].text);
    out_flush(&o);
    CloseHandle(o.h);
}

/* In the crash handler: no waiting for the log lock if another thread holds it */
void diag_crash(void)
{
    char path[MAX_PATH];
    HANDLE h;

    if (!lock_ready)
        return;
    snprintf(path, MAX_PATH, "%s.crash", sess_base);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    dump_flight("crash", 0, "unhandled exception, see the crash line in the session log");
}

/* ---- network quality, once in a while ---- */

static void netq_log(void)
{
    static unsigned int prev_rx[MAX_PLAYERS], prev_lost[MAX_PLAYERS];
    int s;

    for (s = 0; s < MAX_PLAYERS; s++) {
        int rtt;
        double jit;
        unsigned int rx, lost, drx, dlost;

        if (!player_netq(s, &rtt, &jit, &rx, &lost))
            continue;
        drx = rx - prev_rx[s];
        dlost = lost - prev_lost[s];
        if (rx < prev_rx[s]) {
            drx = rx;
            dlost = lost;
        }
        prev_rx[s] = rx;
        prev_lost[s] = lost;
        if (rtt || drx)
            logmsg("netq: player %d rtt %d ms, jitter %.1f ms, loss %.1f%% (%u states, %u missing in %.0f s)", s + 1, rtt, jit,
                   drx + dlost ? dlost * 100.0 / (drx + dlost) : 0.0, drx, dlost, NETQ_MS / 1000.0);
    }
}

/* ---- the desync check ---- */

struct world {
    int level, epoch, players, ents;
    unsigned int lives;
};

static int test_skew, test_deaf;

void diag_test(int skew, int deaf)
{
    test_skew = skew;
    test_deaf = deaf;
    logmsg("diag: test skew %d, deaf %d", skew, deaf);
}

static const char *const world_field[] = { "level", "epoch", "players", "enemies", "life states" };
#define WORLD_FIELDS 5

static void world_now(struct world *w)
{
    int s;

    w->level = cur_level;
    w->epoch = cur_epoch;
    w->players = players_connected() + 1;
    w->ents = ent_count();
    w->lives = 0;
    for (s = 0; s < MAX_PLAYERS; s++) {
        int life = s == my_slot ? revive_life() : player_connected(s) ? player_life(s) : LIFE_ALIVE;
        w->lives |= (unsigned int)(life & 3) << (2 * s);
    }
}

static int world_get(const struct world *w, int field)
{
    return field == 0 ? w->level : field == 1 ? w->epoch : field == 2 ? w->players : field == 3 ? w->ents : (int)w->lives;
}

static void send_world(void)
{
    struct world w;
    struct event e;

    if (!role_host || !have_remote || cur_level < 0)
        return;
    world_now(&w);
    w.ents += test_skew;
    event_init(&e, EV_DIAG);
    e.part = DP_WORLD;
    e.objtype = (short)w.level;
    e.damage = (short)w.players;
    e.type = w.ents;
    e.extra[0] = w.lives;
    snprintf(e.name, EV_NAME, "world");
    event_send(&e, TO_ALL);
}

static void world_check(const struct event *e)
{
    static int run[WORLD_FIELDS], told[WORLD_FIELDS];
    struct world mine, host;
    int f;

    if (test_deaf)
        return;
    if (e->epoch != cur_epoch || cur_level < 0 || e->objtype != cur_level) {
        /* one of us is between levels: nothing to compare yet */
        for (f = 0; f < WORLD_FIELDS; f++)
            run[f] = 0;
        return;
    }
    world_now(&mine);
    host.level = e->objtype;
    host.epoch = e->epoch;
    host.players = e->damage;
    host.ents = e->type;
    host.lives = e->extra[0];
    for (f = 0; f < WORLD_FIELDS; f++) {
        int a = world_get(&host, f), b = world_get(&mine, f);

        if (a == b) {
            if (told[f])
                logmsg("desync cleared: %s agrees again", world_field[f]);
            run[f] = told[f] = 0;
            continue;
        }
        if (++run[f] < DESYNC_RUN || told[f])
            continue;
        told[f] = 1;
        {
            char why[160];
            struct event r;

            snprintf(why, sizeof(why), "%s: host has %d (0x%x), we have %d (0x%x), for %d s in level %d epoch %u",
                     world_field[f], a, a, b, b, run[f], cur_level, cur_epoch);
            logmsg("DESYNC %s", why);
            dump_flight("desync", (unsigned int)(utc_ms() / 1000), why);
            event_init(&r, EV_DIAG);
            r.part = DP_DESYNC;
            r.netid = (unsigned int)f;
            r.type = a;
            r.damage = (short)b;
            r.objtype = (short)cur_level;
            snprintf(r.name, EV_NAME, "%s", world_field[f]);
            event_send(&r, TO_ALL);
        }
    }
}

/* ---- marks: F2 ("something is wrong now") is in every player's game, and each saves the last minute ---- */

void diag_mark(unsigned int n, int from)
{
    char why[64], kind[24];

    if (from)
        snprintf(why, sizeof(why), "F2 mark %u from player %d", n, from);
    else
        snprintf(why, sizeof(why), "F2 mark %u pressed here", n);
    snprintf(kind, sizeof(kind), from ? "markp%d" : "mark", from);
    dump_flight(kind, n, why);
}

void diag_event(const struct event *e, int from)
{
    if (e->part == DP_WORLD) {
        if (!role_host)
            world_check(e);
    } else if (e->part == DP_DESYNC && role_host) {
        char why[160];

        snprintf(why, sizeof(why), "player %d sees %s differently: we have %d, they have %d (level %d)", from + 1, e->name,
                 e->type, (int)e->damage, (int)e->objtype);
        logmsg("DESYNC reported: %s", why);
        dump_flight("report", (unsigned int)(utc_ms() / 1000), why);
    }
}

/* ---- per frame ---- */

void diag_tick(void)
{
    static double next_sample, next_world, next_netq, last_sample;
    static int frames;
    double now = now_ms();

    frames++;
    if (now >= next_sample) {
        if (next_sample)
            sample(now, frames, now - last_sample);
        next_sample = now + SAMPLE_MS;
        last_sample = now;
        frames = 0;
    }
    if (now >= next_world) {
        next_world = now + WORLD_MS;
        send_world();
    }
    if (now >= next_netq) {
        next_netq = now + NETQ_MS;
        netq_log();
    }
}

/* ---- the diagnostics bundle (the launcher's button) ---- */

static HANDLE bundle_proc;
static char bundle_result[MAX_PATH];

int diag_bundle_start(void)
{
    char script[MAX_PATH], cmd[5 * MAX_PATH], sysdir[MAX_PATH], ini[MAX_PATH], out_dir[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    FILE *f;

    if (bundle_proc)
        return 1;
    snprintf(script, MAX_PATH, "%s\\diag.ps1", coop_dir);
    snprintf(bundle_result, MAX_PATH, "%s\\diag-result.txt", coop_dir);
    DeleteFileA(bundle_result);
    if (!(f = fopen(script, "wb")))
        return 0;
    fwrite(diag_script, 1, sizeof(diag_script) - 1, f);
    fclose(f);
    if (!GetSystemDirectoryA(sysdir, MAX_PATH))
        return 0;
    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    GetPrivateProfileStringA("diag", "out_dir", "", out_dir, MAX_PATH, ini);     /* tests: not the desktop, and no Explorer */
    snprintf(cmd, sizeof(cmd),
             "\"%s\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
             "-WindowStyle Hidden -File \"%s\" -CoopDir \"%s\" -GameDir \"%s\" -Version \"%s\"%s%s%s",
             sysdir, script, coop_dir, game_dir, MOD_VERSION, out_dir[0] ? " -NoOpen -OutDir \"" : "", out_dir,
             out_dir[0] ? "\"" : "");
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, coop_dir, &si, &pi)) {
        logmsg("diagnostics: cannot start PowerShell (error %lu)", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    bundle_proc = pi.hProcess;
    logmsg("diagnostics: bundling started");
    return 1;
}

int diag_bundle_poll(char *msg, int size)
{
    FILE *f;
    char line[MAX_PATH + 8];

    msg[0] = 0;
    if (!bundle_proc)
        return 0;
    if (WaitForSingleObject(bundle_proc, 0) == WAIT_TIMEOUT)
        return 1;
    CloseHandle(bundle_proc);
    bundle_proc = NULL;
    line[0] = 0;
    if ((f = fopen(bundle_result, "r"))) {
        if (fgets(line, sizeof(line), f))
            line[strcspn(line, "\r\n")] = 0;
        fclose(f);
    }
    logmsg("diagnostics: %s", line[0] ? line : "no result");
    if (!strncmp(line, "OK ", 3)) {
        snprintf(msg, size, "%s", line + 3);
        return 2;
    }
    snprintf(msg, size, "%s", !strncmp(line, "ERR ", 4) ? line + 4 : "the bundle script did not finish");
    return 3;
}
