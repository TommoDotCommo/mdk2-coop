#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_update.h"

static void make_pe32(unsigned char *buf, size_t len, unsigned char tag_byte)
{
    memset(buf, tag_byte, len);
    buf[0] = 'M'; buf[1] = 'Z';
    buf[60] = 64; buf[61] = 0; buf[62] = 0; buf[63] = 0;
    memcpy(buf + 64, "PE\0\0", 4);
    buf[68] = 0x4c; buf[69] = 0x01;
}

static size_t make_stored_zip(unsigned char *out, const char *name1, const unsigned char *d1, size_t s1,
                              const char *name2, const unsigned char *d2, size_t s2)
{
    const char *names[2] = { name1, name2 };
    const unsigned char *data[2] = { d1, d2 };
    size_t sizes[2] = { s1, s2 }, lhoff[2], pos = 0, cd_start, cd_size;
    uint32_t crcs[2];
    int i;
    for (i = 0; i < 2; i++) {
        uint16_t nlen = (uint16_t)strlen(names[i]);
        crcs[i] = lu_crc32(data[i], sizes[i]);
        lhoff[i] = pos;
        memset(out + pos, 0, 30);
        memcpy(out + pos, "PK\x03\x04", 4);
        out[pos+4] = 20;
        memcpy(out + pos + 14, &crcs[i], 4);
        memcpy(out + pos + 18, &sizes[i], 4);
        memcpy(out + pos + 22, &sizes[i], 4);
        memcpy(out + pos + 26, &nlen, 2);
        pos += 30;
        memcpy(out + pos, names[i], nlen); pos += nlen;
        memcpy(out + pos, data[i], sizes[i]); pos += sizes[i];
    }
    cd_start = pos;
    for (i = 0; i < 2; i++) {
        uint16_t nlen = (uint16_t)strlen(names[i]);
        uint32_t off32 = (uint32_t)lhoff[i], sz32 = (uint32_t)sizes[i];
        memset(out + pos, 0, 46);
        memcpy(out + pos, "PK\x01\x02", 4);
        out[pos+4] = 20; out[pos+6] = 20;
        memcpy(out + pos + 16, &crcs[i], 4);
        memcpy(out + pos + 20, &sz32, 4);
        memcpy(out + pos + 24, &sz32, 4);
        memcpy(out + pos + 28, &nlen, 2);
        memcpy(out + pos + 42, &off32, 4);
        pos += 46;
        memcpy(out + pos, names[i], nlen); pos += nlen;
    }
    cd_size = pos - cd_start;
    memset(out + pos, 0, 22);
    memcpy(out + pos, "PK\x05\x06", 4);
    out[pos+8] = 2; out[pos+10] = 2;
    {
        uint32_t cds = (uint32_t)cd_size, cdo = (uint32_t)cd_start;
        memcpy(out + pos + 12, &cds, 4);
        memcpy(out + pos + 16, &cdo, 4);
    }
    pos += 22;
    return pos;
}

int main(int argc, char **argv)
{
    char hash[65], zip_hash[65], sidecar[256], json[2048];
    struct lu_schedule sched = { 1, 0, 0, 1 };
    struct lu_release rel;
    unsigned char dll1[320], dll2[320], zip_buf[4096], *ext = NULL;
    const unsigned char *lua1 = (const unsigned char *)"-- lua v1\n", *lua2 = (const unsigned char *)"-- lua v2\n";
    size_t zip_len, ext_len = 0;

    /* SHA-256 FIPS 180-4 vectors */
    lu_sha256_hex("", 0, hash);
    assert(!strcmp(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    lu_sha256_hex("abc", 3, hash);
    assert(!strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* Version comparisons */
    assert(lu_version_is_newer("v0.3.1", "v0.3.2") == 1);
    assert(lu_version_is_newer("0.3.1", "v0.4.0") == 1);
    assert(lu_version_is_newer("v0.3.2", "v1.0.0") == 1);
    assert(lu_version_is_newer("v0.3.2", "v0.3.2") == 0);
    assert(lu_version_is_newer("v0.3.2", "v0.3.1") == 0);
    assert(lu_version_is_newer("v0.3.2-alpha.1", "v0.3.2") == 1);
    assert(lu_version_is_newer("v0.3.2", "v0.3.2-alpha.1") == 0);
    assert(lu_version_is_newer("v0.3.2-alpha.1", "v0.3.2-alpha.2") == 1);
    assert(lu_version_is_newer("0.3.1-alpha", "0.3.2-alpha") == 1);
    assert(lu_version_is_newer("v0.3.1-alpha", "0.3.2-alpha") == 1);
    assert(lu_version_is_newer("0.3.1-alpha", "v0.3.2-alpha") == 1);
    assert(lu_version_is_newer("0.3.2-alpha", "0.3.1-alpha") == 0);
    assert(lu_version_is_newer("0.3.2-alpha", "0.3.2") == 1);
    assert(lu_version_is_newer("v0.3.2-alpha", "0.3.2") == 1);
    assert(lu_version_is_newer("0.3.2-alpha", "v0.3.2") == 1);
    assert(lu_version_is_newer("0.3.2", "0.3.2-alpha") == 0);
    assert(lu_version_is_newer("v0.3.2", "v0.3.2-alpha") == 0);
    assert(lu_version_is_newer("0.3.2-alpha", "v0.3.2-alpha") == 0);
    assert(lu_version_is_newer("v0.3.2-alpha", "0.3.2-alpha") == 0);
    assert(lu_version_is_newer("dev", "v0.1.0") == 1);
    assert(lu_version_is_newer("v0.3.2", "not-a-version") == -1);
    assert(lu_version_is_newer("v0.3.2", "v1.2") == -1);
    assert(lu_version_is_newer("v0.3.2", "v1.2.3-") == -1);
    assert(lu_version_is_newer("v0.3.2", "v1.2.3;bad") == -1);

    /* Schedule & rate-limit throttling */
    assert(lu_can_check(&sched, 1000, 0) == 1);
    sched.auto_enabled = 0;
    assert(lu_can_check(&sched, 1000, 0) == 0);
    assert(lu_can_check(&sched, 1000, 1) == 1);
    sched.auto_enabled = 1;
    assert(lu_http_classify(200, 50, 0, 1000, &sched) == LU_OK && sched.last_check == 1000);
    assert(lu_can_check(&sched, 1000 + LU_INTERVAL_SEC - 1, 0) == 0);
    assert(lu_can_check(&sched, 1000 + LU_INTERVAL_SEC - 1, 1) == 1);
    assert(lu_can_check(&sched, 1000 + LU_INTERVAL_SEC, 0) == 1);
    assert(lu_http_classify(403, 0, 600, 2000, &sched) == LU_ERR_RATELIMIT && sched.retry_after == 2600);
    assert(lu_can_check(&sched, 2599, 1) == 0);
    assert(lu_can_check(&sched, 2600, 1) == 1);
    assert(lu_http_classify(429, 10, 120, 3000, &sched) == LU_ERR_RATELIMIT && sched.retry_after == 3120);
    assert(lu_http_classify(0, 10, 0, 4000, &sched) == LU_ERR_OFFLINE);
    assert(lu_http_classify(500, 10, 0, 4000, &sched) == LU_ERR_HTTP);

    /* Release JSON & asset selection */
    snprintf(json, sizeof(json),
             "{\"tag_name\":\"v0.4.0\",\"body\":\"Fix 1\\r\\nFix 2\\u0007\\x01\","
             "\"assets\":["
             "{\"name\":\"other.zip\",\"browser_download_url\":\"https://github.com/x/other.zip\",\"size\":100},"
             "{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip\",\"size\":512},"
             "{\"name\":\"mdk2-coop-v0.4.0.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip.sha256\",\"size\":90}"
             "]}");
    assert(lu_parse_release(json, strlen(json), "mdk2-coop", 1, &rel) == LU_OK);
    assert(!strcmp(rel.tag, "v0.4.0") && rel.zip_size == 512 && rel.sha_size == 90);
    assert(strstr(rel.notes, "Fix 1\nFix 2") != NULL);
    {
        char bad_json[2048];
        strcpy(bad_json, json);
        strstr(bad_json, "Fix 2")[2] = 0;
        assert(lu_parse_release(bad_json, strlen(json), "mdk2-coop", 1, &rel) == LU_ERR_JSON);
    }
    assert(lu_parse_release("{\"tag_name\":\"bad_tag\",\"assets\":[]}", 34, "mdk2-coop", 1, &rel) == LU_ERR_VERSION);
    assert(lu_parse_release("{\"tag_name\":\"v0.4.0\",\"assets\":[]}", 33, "mdk2-coop", 1, &rel) == LU_ERR_ASSET);
    {
        const char *no_sidecar = "{\"tag_name\":\"v0.4.0\",\"assets\":[{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"https://github.com/a.zip\",\"size\":10}]}";
        assert(lu_parse_release(no_sidecar, strlen(no_sidecar), "mdk2-coop", 1, &rel) == LU_ERR_ASSET);
    }
    {
        const char *bad_url = "{\"tag_name\":\"v0.4.0\",\"assets\":[{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"http://evil.example.com/a.zip\",\"size\":10},{\"name\":\"mdk2-coop-v0.4.0.zip.sha256\",\"browser_download_url\":\"https://github.com/a.sha256\",\"size\":90}]}";
        assert(lu_parse_release(bad_url, strlen(bad_url), "mdk2-coop", 1, &rel) == LU_ERR_ASSET);
    }

    /* List containing only pre-releases */
    {
        const char *pre_list = "["
            "{\"tag_name\":\"v0.3.1-alpha\",\"prerelease\":true,\"draft\":false,\"published_at\":\"2026-10-27T12:00:00Z\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.3.1-alpha.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.3.1-alpha.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.3.1-alpha.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.3.1-alpha.zip.sha256\",\"size\":90}]}"
            "]";
        /* With include_prereleases = 1, it must be selected */
        assert(lu_parse_release(pre_list, strlen(pre_list), "mdk2-coop", 1, &rel) == LU_OK);
        assert(!strcmp(rel.tag, "v0.3.1-alpha"));
        /* With include_prereleases = 0, pre-releases are excluded -> returns LU_UP_TO_DATE */
        assert(lu_parse_release(pre_list, strlen(pre_list), "mdk2-coop", 0, &rel) == LU_UP_TO_DATE);
    }

    /* Draft release must be ignored even if newer */
    {
        const char *draft_list = "["
            "{\"tag_name\":\"v0.5.0-alpha\",\"draft\":true,\"prerelease\":true,\"published_at\":\"2026-10-28T12:00:00Z\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.5.0-alpha.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.5.0-alpha.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.5.0-alpha.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.5.0-alpha.zip.sha256\",\"size\":90}]},"
            "{\"tag_name\":\"v0.4.0\",\"draft\":false,\"prerelease\":false,\"published_at\":\"2026-10-20T12:00:00Z\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.4.0.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip.sha256\",\"size\":90}]}"
            "]";
        assert(lu_parse_release(draft_list, strlen(draft_list), "mdk2-coop", 1, &rel) == LU_OK);
        assert(!strcmp(rel.tag, "v0.4.0"));
    }

    /* Older release listed first: newest by version must be selected */
    {
        const char *older_first = "["
            "{\"tag_name\":\"v0.3.0\",\"draft\":false,\"prerelease\":false,\"published_at\":\"2026-10-01T12:00:00Z\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.3.0.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.3.0.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.3.0.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.3.0.zip.sha256\",\"size\":90}]},"
            "{\"tag_name\":\"v0.4.0\",\"draft\":false,\"prerelease\":false,\"published_at\":\"2026-10-20T12:00:00Z\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.4.0.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip.sha256\",\"size\":90}]}"
            "]";
        assert(lu_parse_release(older_first, strlen(older_first), "mdk2-coop", 1, &rel) == LU_OK);
        assert(!strcmp(rel.tag, "v0.4.0"));
    }

    /* Version tie broken by published_at */
    {
        const char *tie_list = "["
            "{\"tag_name\":\"v0.4.0\",\"draft\":false,\"prerelease\":false,\"published_at\":\"2026-10-01T12:00:00Z\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.4.0.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip.sha256\",\"size\":90}]},"
            "{\"tag_name\":\"v0.4.0\",\"draft\":false,\"prerelease\":false,\"published_at\":\"2026-10-25T12:00:00Z\",\"body\":\"newer build\","
            "\"assets\":[{\"name\":\"mdk2-coop-v0.4.0.zip\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip\",\"size\":100},"
            "{\"name\":\"mdk2-coop-v0.4.0.zip.sha256\",\"browser_download_url\":\"https://github.com/x/mdk2-coop-v0.4.0.zip.sha256\",\"size\":90}]}"
            "]";
        assert(lu_parse_release(tie_list, strlen(tie_list), "mdk2-coop", 1, &rel) == LU_OK);
        assert(!strcmp(rel.tag, "v0.4.0") && !strcmp(rel.notes, "newer build"));
    }

    /* Sidecar & download verification */
    make_pe32(dll1, sizeof(dll1), 0x11);
    make_pe32(dll2, sizeof(dll2), 0x22);
    zip_len = make_stored_zip(zip_buf, "mdk2-coop-v0.4.0/dinput8.dll", dll2, sizeof(dll2),
                              "mdk2-coop-v0.4.0/coop/coop.lua", lua2, strlen((const char *)lua2));
    lu_sha256_hex(zip_buf, zip_len, zip_hash);
    snprintf(sidecar, sizeof(sidecar), "%s  mdk2-coop-v0.4.0.zip\n", zip_hash);
    assert(lu_parse_sidecar(sidecar, strlen(sidecar), "mdk2-coop-v0.4.0.zip", hash) == LU_OK);
    assert(!strcmp(hash, zip_hash));
    assert(lu_parse_sidecar(sidecar, strlen(sidecar), "mdk2-coop-v0.3.9.zip", hash) == LU_ERR_SIDECAR);
    assert(lu_verify_download(zip_buf, zip_len, zip_len, hash) == LU_OK);
    assert(lu_verify_download(zip_buf, zip_len - 1, zip_len, hash) == LU_ERR_TRUNCATED);
    zip_buf[zip_len - 1] ^= 1;
    assert(lu_verify_download(zip_buf, zip_len, zip_len, hash) == LU_ERR_CHECKSUM);
    zip_buf[zip_len - 1] ^= 1;

    /* ZIP extraction, CRC-32 and PE validation */
    assert(lu_zip_extract(zip_buf, zip_len, "mdk2-coop-v0.4.0/dinput8.dll", &ext, &ext_len) == LU_OK);
    assert(ext_len == sizeof(dll2) && !memcmp(ext, dll2, sizeof(dll2)) && lu_valid_pe32(ext, ext_len));
    free(ext);
    zip_buf[30 + 28 + 10] ^= 0xff; /* corrupt payload byte inside ZIP -> CRC mismatch */
    assert(lu_zip_extract(zip_buf, zip_len, "mdk2-coop-v0.4.0/dinput8.dll", &ext, &ext_len) == LU_ERR_ARCHIVE);
    zip_buf[30 + 28 + 10] ^= 0xff;
    {
        unsigned char bad_pe[320];
        make_pe32(bad_pe, sizeof(bad_pe), 0x33);
        bad_pe[0] = 'X';
        assert(!lu_valid_pe32(bad_pe, sizeof(bad_pe)));
        make_pe32(bad_pe, sizeof(bad_pe), 0x33);
        bad_pe[68] = 0x64; bad_pe[69] = 0x86; /* x86_64 instead of i386 */
        assert(!lu_valid_pe32(bad_pe, sizeof(bad_pe)));
    }

    /* Transactional self-update, fault-injected rollback, and manual rollback */
    if (argc == 2) {
        const char *dir = argv[1];
        char dll_path[512], lua_path[512], orig_dll_hash[65], orig_lua_hash[65], cur_dll_hash[65], cur_lua_hash[65];
        snprintf(dll_path, sizeof(dll_path), "%s/dinput8.dll", dir);
        snprintf(lua_path, sizeof(lua_path), "%s/coop/coop.lua", dir);
        lu_sha256_hex(dll1, sizeof(dll1), orig_dll_hash);
        lu_sha256_hex(lua1, strlen((const char *)lua1), orig_lua_hash);
        assert(lu_write_verified(dll_path, dll1, sizeof(dll1), orig_dll_hash));
        assert(lu_write_verified(lua_path, lua1, strlen((const char *)lua1), orig_lua_hash));

        /* Fault-injected failure after DLL replacement must roll back DLL and Lua automatically */
        assert(lu_apply_package(dir, zip_buf, zip_len, "mdk2-coop", "v0.3.1", "v0.4.0", 1) == LU_ERR_IO);
        assert(lu_file_sha256(dll_path, cur_dll_hash) && !strcmp(cur_dll_hash, orig_dll_hash));
        assert(lu_file_sha256(lua_path, cur_lua_hash) && !strcmp(cur_lua_hash, orig_lua_hash));

        /* Normal update succeeds and keeps .previous backups for manual rollback */
        assert(lu_apply_package(dir, zip_buf, zip_len, "mdk2-coop", "v0.3.1", "v0.4.0", 0) == LU_OK);
        assert(lu_file_sha256(dll_path, cur_dll_hash) && strcmp(cur_dll_hash, orig_dll_hash) != 0);
        assert(lu_file_sha256(lua_path, cur_lua_hash) && strcmp(cur_lua_hash, orig_lua_hash) != 0);

        /* Tampered .previous backup is refused by rollback without corrupting active DLL */
        {
            char prev_dll[512], v2_dll_hash[65];
            FILE *f;
            lu_sha256_hex(dll2, sizeof(dll2), v2_dll_hash);
            snprintf(prev_dll, sizeof(prev_dll), "%s/dinput8.dll.previous", dir);
            f = fopen(prev_dll, "wb"); assert(f); fwrite("bad", 1, 3, f); fclose(f);
            assert(lu_rollback(dir) == LU_ERR_ROLLBACK);
            assert(lu_file_sha256(dll_path, cur_dll_hash) && !strcmp(cur_dll_hash, v2_dll_hash));
            assert(lu_write_verified(prev_dll, dll1, sizeof(dll1), orig_dll_hash));
        }

        /* Manual rollback restores exact v1 bytes */
        assert(lu_rollback(dir) == LU_OK);
        assert(lu_file_sha256(dll_path, cur_dll_hash) && !strcmp(cur_dll_hash, orig_dll_hash));
        assert(lu_file_sha256(lua_path, cur_lua_hash) && !strcmp(cur_lua_hash, orig_lua_hash));
    }

    puts("PASS: version comparator, rate-limit policy, SHA-256, release/sidecar parser, ZIP/PE verifier and transactional rollback");
    return 0;
}
