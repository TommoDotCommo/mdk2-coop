/* The launcher's updater against the release layout tools/release.sh produces: a GitHub release JSON listing the
 * single-file Setup.exe and its sidecar next to the zip and its sidecar, and the real zip applied over an installed older
 * version. tools/test_update_layout.py builds this twice: with the current bridge/launcher_update.h and with the header of
 * the last public release (the updater players already have). Arguments: releases.json zip game-dir current-tag. */
#include <assert.h>
#include <stdio.h>
#include "launcher_update.h"

static unsigned char *slurp(const char *path, size_t *len)
{
    unsigned char *buf;
    assert(lu_read_bytes(path, &buf, len));
    return buf;
}

int main(int argc, char **argv)
{
    struct lu_release rel;
    size_t json_len, zip_len, n;
    unsigned char *json, *zip, *dll, *lua;
    char sidecar[256], hash[65], zhash[65], p1[512], p2[512], d1[65], d2[65];
    unsigned char *a, *b;
    if (argc != 5) return 2;
    json = slurp(argv[1], &json_len);
    zip = slurp(argv[2], &zip_len);
    /* 1. release discovery picks the zip and its sidecar, not the Setup.exe next to them */
    if (lu_parse_release((const char *)json, json_len, "mdk2-coop", 0, &rel) != LU_OK) { puts("FAIL: release not recognised"); return 1; }
    assert(!strcmp(rel.zip_name + strlen(rel.zip_name) - 4, ".zip"));
    if (!strstr(rel.zip_url, "mdk2-coop-") || strstr(rel.zip_url, "Setup") || !strstr(rel.sha_url, ".zip.sha256") || strstr(rel.sha_url, "Setup")) {
        printf("FAIL: wrong assets chosen: %s %s\n", rel.zip_url, rel.sha_url); return 1;
    }
    if (rel.zip_size != zip_len) { puts("FAIL: zip size differs from the release listing"); return 1; }
    /* 2. the sidecar belongs to the zip and the download verifies */
    lu_sha256_hex(zip, zip_len, zhash);
    snprintf(sidecar, sizeof(sidecar), "%s  %s\n", zhash, rel.zip_name);
    if (lu_parse_sidecar(sidecar, strlen(sidecar), rel.zip_name, hash) != LU_OK || lu_verify_download(zip, zip_len, rel.zip_size, hash) != LU_OK) {
        puts("FAIL: zip does not verify"); return 1;
    }
    /* 3. the package applies over an installed older version and the files are the zip's */
    {
        char m1[200], m2[200];
        snprintf(m1, sizeof(m1), "mdk2-coop-%s/dinput8.dll", rel.tag);
        snprintf(m2, sizeof(m2), "mdk2-coop-%s/coop/coop.lua", rel.tag);
        if (lu_zip_extract(zip, zip_len, m1, &dll, &n) != LU_OK || !lu_valid_pe32(dll, n)) { puts("FAIL: dinput8.dll not found in the zip where the updater looks"); return 1; }
        if (lu_zip_extract(zip, zip_len, m2, &lua, &n) != LU_OK) { puts("FAIL: coop.lua not found in the zip where the updater looks"); return 1; }
    }
#ifdef LU_HAS_DATA_DIR
    char data_dir[600];
    snprintf(data_dir, sizeof(data_dir), "%s.data", argv[3]);
#define LU_DIRS argv[3], data_dir
#else
#define LU_DIRS argv[3]
#endif
    if (lu_apply_package(LU_DIRS, zip, zip_len, "mdk2-coop", argv[4], rel.tag, 0) != LU_OK) { puts("FAIL: update did not apply"); return 1; }
    snprintf(p1, sizeof(p1), "%s/dinput8.dll", argv[3]);
    snprintf(p2, sizeof(p2), "%s/coop/coop.lua", argv[3]);
    a = slurp(p1, &n); lu_sha256_hex(a, n, d1); lu_sha256_hex(dll, n, d2);
    if (strcmp(d1, d2)) { puts("FAIL: installed dinput8.dll is not the zip's"); return 1; }
    b = slurp(p2, &n);
    lu_sha256_hex(b, n, d1);
    lu_sha256_hex(lua, n, d2);
    if (strcmp(d1, d2)) { puts("FAIL: installed coop.lua is not the zip's"); return 1; }
    /* 4. and rolls back to the older bytes */
    if (lu_rollback(LU_DIRS) != LU_OK) { puts("FAIL: rollback"); return 1; }
    a = slurp(p1, &n);
    if (n != 12 || memcmp(a, "OLD-DLL-FILE", 12)) { puts("FAIL: rollback did not restore the old dinput8.dll"); return 1; }
    puts("PASS: updater finds, verifies and applies the new release layout and rolls it back");
    return 0;
}
