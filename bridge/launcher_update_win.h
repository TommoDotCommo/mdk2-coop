/* Windows launcher adapter for GitHub release check, verified download and rollback.
 * Runs HTTP requests on a worker thread via WinINet; UI stays responsive. */
#include <wininet.h>
#include "launcher_update.h"

#define DEFAULT_RELEASE_API "https://api.github.com/repos/TommoDotCommo/mdk2-coop/releases?per_page=10"

enum { UP_IDLE, UP_CHECKING, UP_CURRENT, UP_AVAILABLE, UP_DOWNLOADING, UP_APPLIED, UP_ROLLED_BACK, UP_ERROR };

static CRITICAL_SECTION update_lock;
static int update_inited;
static volatile LONG update_busy;
static struct lu_schedule update_sched = { 1, 0, 0, 1 };
static struct lu_release update_rel;
static int update_state = UP_IDLE;
static int update_last_err = LU_OK;
static int update_rollback_ready;
static char update_api_url[LU_URL_MAX];
static char update_msg[220];

static int update_has_rollback(void)
{
    char state_path[MAX_PATH], dll_prev[MAX_PATH], lua_prev[MAX_PATH];
    snprintf(state_path, sizeof(state_path), "%s\\coop\\update-state.ini", game_dir);
    snprintf(dll_prev, sizeof(dll_prev), "%s\\dinput8.dll.previous", game_dir);
    snprintf(lua_prev, sizeof(lua_prev), "%s\\coop\\coop.lua.previous", game_dir);
    return GetFileAttributesA(state_path) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesA(dll_prev) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesA(lua_prev) != INVALID_FILE_ATTRIBUTES;
}

static void update_save_schedule(void)
{
    char num[32];
    put_int("update", "auto_check", update_sched.auto_enabled);
    put_int("update", "include_prereleases", update_sched.include_prereleases);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)update_sched.last_check);
    put("update", "last_check", num);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)update_sched.retry_after);
    put("update", "retry_after", num);
}

static int update_wininet_get(const char *url, int *status_code, int *rl_rem, uint32_t *retry_after,
                              unsigned char **out_buf, size_t *out_len, size_t max_bytes)
{
    HINTERNET hnet = NULL, hreq = NULL;
    DWORD code = 0, sz = sizeof(code), idx = 0;
    char hdr[64];
    unsigned char *buf = NULL;
    size_t cap = 8192, len = 0;
    int ok = 0;

    *status_code = 0; *rl_rem = -1; *retry_after = 0; *out_buf = NULL; *out_len = 0;
    hnet = InternetOpenA("mdk2-coop-updater/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hnet) return 0;
    hreq = InternetOpenUrlA(hnet, url, "Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                            INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_AUTH, 0);
    if (!hreq) {
        InternetCloseHandle(hnet);
        return 0;
    }
    if (HttpQueryInfoA(hreq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &sz, &idx))
        *status_code = (int)code;
    sz = sizeof(hdr); idx = 0;
    if (HttpQueryInfoA(hreq, HTTP_QUERY_CUSTOM, hdr, &sz, &idx)) { /* unused fallback */ }
    strcpy(hdr, "X-RateLimit-Remaining"); sz = sizeof(hdr); idx = 0;
    if (HttpQueryInfoA(hreq, HTTP_QUERY_CUSTOM, hdr, &sz, &idx))
        *rl_rem = atoi(hdr);
    strcpy(hdr, "Retry-After"); sz = sizeof(hdr); idx = 0;
    if (HttpQueryInfoA(hreq, HTTP_QUERY_CUSTOM, hdr, &sz, &idx))
        *retry_after = (uint32_t)atoi(hdr);

    buf = (unsigned char *)malloc(cap + 1);
    if (buf) {
        ok = 1;
        for (;;) {
            DWORD got = 0;
            if (len + 4096 > cap) {
                size_t next_cap = cap * 2;
                unsigned char *nb;
                if (next_cap > max_bytes + 4096) next_cap = max_bytes + 4096;
                if (len + 1 >= next_cap) { ok = 0; break; }
                nb = (unsigned char *)realloc(buf, next_cap + 1);
                if (!nb) { ok = 0; break; }
                buf = nb; cap = next_cap;
            }
            if (!InternetReadFile(hreq, buf + len, 4096, &got)) { ok = 0; break; }
            if (got == 0) break;
            len += got;
            if (len > max_bytes) { ok = 0; break; }
        }
    }
    InternetCloseHandle(hreq);
    InternetCloseHandle(hnet);
    if (!ok) {
        free(buf);
        return 0;
    }
    buf[len] = 0;
    *out_buf = buf; *out_len = len;
    return 1;
}

static DWORD WINAPI update_check_worker(LPVOID arg)
{
    int manual = (int)(intptr_t)arg;
    int status = 0, rl_rem = -1;
    uint32_t retry_after = 0;
    unsigned char *body = NULL;
    size_t body_len = 0;
    uint64_t now = (uint64_t)time(NULL);
    struct lu_schedule sched;
    struct lu_release rel;
    char url[LU_URL_MAX];
    enum lu_result r;

    EnterCriticalSection(&update_lock);
    sched = update_sched;
    snprintf(url, sizeof(url), "%s", update_api_url);
    LeaveCriticalSection(&update_lock);

    if (!lu_can_check(&sched, now, manual)) {
        EnterCriticalSection(&update_lock);
        update_state = UP_ERROR;
        update_last_err = LU_THROTTLED;
        snprintf(update_msg, sizeof(update_msg), "Update check throttled by rate limit; try again later.");
        LeaveCriticalSection(&update_lock);
        InterlockedExchange(&update_busy, 0);
        return 0;
    }

    update_wininet_get(url, &status, &rl_rem, &retry_after, &body, &body_len, LU_MAX_JSON_BYTES);
    r = lu_http_classify(status, rl_rem, retry_after, now, &sched);
    if (r == LU_OK)
        r = lu_parse_release((const char *)body, body_len, "mdk2-coop", sched.include_prereleases, &rel);
    free(body);

    EnterCriticalSection(&update_lock);
    update_sched = sched;
    update_save_schedule();
    update_last_err = r;
    if (r == LU_OK) {
        int newer = lu_version_is_newer(MOD_VERSION, rel.tag);
        if (newer > 0) {
            update_rel = rel;
            update_state = UP_AVAILABLE;
            snprintf(update_msg, sizeof(update_msg), "Update available: %s (%u KB)", rel.tag, (unsigned)((rel.zip_size + 1023) / 1024));
        } else if (newer == 0) {
            update_state = UP_CURRENT;
            snprintf(update_msg, sizeof(update_msg), "Update check successful: up to date (%s).", MOD_VERSION);
        } else {
            update_state = UP_ERROR;
            update_last_err = LU_ERR_VERSION;
            snprintf(update_msg, sizeof(update_msg), "Release tag format unrecognized.");
        }
    } else if (r == LU_UP_TO_DATE) {
        update_state = UP_IDLE;
        snprintf(update_msg, sizeof(update_msg), "Update check successful: no eligible releases found.");
    } else if (r == LU_ERR_RATELIMIT) {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "GitHub API rate limit reached; try again later.");
    } else if (r == LU_ERR_OFFLINE) {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "Offline or GitHub unreachable.");
    } else {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "Could not read release metadata (code %d).", (int)r);
    }
    changed = 1;
    LeaveCriticalSection(&update_lock);
    InterlockedExchange(&update_busy, 0);
    return 0;
}

static DWORD WINAPI update_apply_worker(LPVOID arg)
{
    struct lu_release rel;
    int status = 0, rl_rem = -1;
    uint32_t retry_after = 0;
    unsigned char *sha_buf = NULL, *zip_buf = NULL;
    size_t sha_len = 0, zip_len = 0;
    char expected_sha[65];
    enum lu_result r;
    (void)arg;

    EnterCriticalSection(&update_lock);
    rel = update_rel;
    LeaveCriticalSection(&update_lock);

    if (!update_wininet_get(rel.sha_url, &status, &rl_rem, &retry_after, &sha_buf, &sha_len, 4096) ||
        status != 200 || sha_len != rel.sha_size) {
        r = LU_ERR_SIDECAR;
    } else {
        r = lu_parse_sidecar((const char *)sha_buf, sha_len, rel.zip_name, expected_sha);
    }
    free(sha_buf);

    if (r == LU_OK) {
        if (!update_wininet_get(rel.zip_url, &status, &rl_rem, &retry_after, &zip_buf, &zip_len, LU_MAX_ASSET_BYTES) ||
            status != 200) {
            r = LU_ERR_TRUNCATED;
        } else {
            r = lu_verify_download(zip_buf, zip_len, rel.zip_size, expected_sha);
            if (r == LU_OK)
                r = lu_apply_package(game_dir, zip_buf, zip_len, "mdk2-coop", MOD_VERSION, rel.tag, 0);
        }
        free(zip_buf);
    }

    EnterCriticalSection(&update_lock);
    update_last_err = r;
    update_rollback_ready = update_has_rollback();
    if (r == LU_OK) {
        update_state = UP_APPLIED;
        snprintf(update_msg, sizeof(update_msg), "Updated to %s. Restart MDK2 HD to load the new launcher.", rel.tag);
    } else if (r == LU_ERR_CHECKSUM) {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "Update aborted: SHA-256 checksum mismatch. Existing files kept.");
    } else if (r == LU_ERR_TRUNCATED) {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "Update aborted: download interrupted or incomplete. Existing files kept.");
    } else {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "Update failed (code %d); rolled back to previous version.", (int)r);
    }
    changed = 1;
    LeaveCriticalSection(&update_lock);
    InterlockedExchange(&update_busy, 0);
    return 0;
}

static void update_start_check(int manual)
{
    HANDLE th;
    if (InterlockedCompareExchange(&update_busy, 1, 0) != 0) return;
    EnterCriticalSection(&update_lock);
    update_state = UP_CHECKING;
    snprintf(update_msg, sizeof(update_msg), "Checking GitHub releases...");
    LeaveCriticalSection(&update_lock);
    th = CreateThread(NULL, 0, update_check_worker, (LPVOID)(intptr_t)manual, 0, NULL);
    if (th) CloseHandle(th);
    else InterlockedExchange(&update_busy, 0);
    changed = 1;
}

static void update_start_apply(void)
{
    HANDLE th;
    if (update_state != UP_AVAILABLE) return;
    if (InterlockedCompareExchange(&update_busy, 1, 0) != 0) return;
    EnterCriticalSection(&update_lock);
    update_state = UP_DOWNLOADING;
    snprintf(update_msg, sizeof(update_msg), "Downloading and verifying %s...", update_rel.zip_name);
    LeaveCriticalSection(&update_lock);
    th = CreateThread(NULL, 0, update_apply_worker, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else InterlockedExchange(&update_busy, 0);
    changed = 1;
}

static void update_do_rollback(void)
{
    enum lu_result r;
    if (InterlockedCompareExchange(&update_busy, 1, 0) != 0) return;
    r = lu_rollback(game_dir);
    EnterCriticalSection(&update_lock);
    update_last_err = r;
    update_rollback_ready = update_has_rollback();
    if (r == LU_OK) {
        update_state = UP_ROLLED_BACK;
        snprintf(update_msg, sizeof(update_msg), "Rolled back to the previous version. Restart MDK2 HD.");
    } else {
        update_state = UP_ERROR;
        snprintf(update_msg, sizeof(update_msg), "Rollback refused: backup missing or failed SHA-256 verification.");
    }
    LeaveCriticalSection(&update_lock);
    InterlockedExchange(&update_busy, 0);
    changed = 1;
}

static void update_init(void)
{
    char num[32];
    int recovered, default_pre = 0;
    if (update_inited) return;
    InitializeCriticalSection(&update_lock);
    update_inited = 1;
#if defined(MOD_VERSION)
    if (strstr(MOD_VERSION, "-alpha") != NULL || !strcmp(MOD_VERSION, "dev"))
        default_pre = 1;
#else
    default_pre = 1;
#endif
    update_sched.include_prereleases = GetPrivateProfileIntA("update", "include_prereleases", default_pre, ini) != 0;
    update_sched.auto_enabled = GetPrivateProfileIntA("update", "auto_check", 1, ini) != 0;
    GetPrivateProfileStringA("update", "last_check", "0", num, sizeof(num), ini);
    lp_number(num, &update_sched.last_check);
    GetPrivateProfileStringA("update", "retry_after", "0", num, sizeof(num), ini);
    lp_number(num, &update_sched.retry_after);
    GetPrivateProfileStringA("update", "api_url", DEFAULT_RELEASE_API, update_api_url, sizeof(update_api_url), ini);
    if (!lu_url_valid(update_api_url))
        strcpy(update_api_url, DEFAULT_RELEASE_API);

    recovered = lu_recover_interrupted(game_dir);
    update_rollback_ready = update_has_rollback();
    if (recovered > 0) {
        update_state = UP_ROLLED_BACK;
        snprintf(update_msg, sizeof(update_msg), "Recovered from an interrupted update by restoring the previous version.");
    } else if (!test_background || strcmp(update_api_url, DEFAULT_RELEASE_API) != 0) {
        if (lu_can_check(&update_sched, (uint64_t)time(NULL), 0))
            update_start_check(0);
    }
}
