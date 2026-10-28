/* POSIX HTTP client fixture driving launcher_update.h against a local fake release server. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "launcher_update.h"

static int http_get_local(const char *url, int *status_code, int *rl_rem, uint32_t *retry_after,
                          unsigned char **body, size_t *body_len)
{
    int port = 0, sock;
    char path[256] = "/", req[512];
    struct sockaddr_in addr;
    unsigned char *buf = NULL;
    size_t cap = 65536, len = 0;
    const char *p, *hdr_end;

    *status_code = 0; *rl_rem = -1; *retry_after = 0; *body = NULL; *body_len = 0;
    if (sscanf(url, "http://127.0.0.1:%d%255s", &port, path) < 1 || port <= 0 || port > 65535) return 0;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock); return 0;
    }
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nUser-Agent: mdk2-coop-updater\r\nConnection: close\r\n\r\n", path, port);
    if (send(sock, req, strlen(req), 0) != (ssize_t)strlen(req)) {
        close(sock); return 0;
    }
    buf = (unsigned char *)malloc(cap + 1);
    assert(buf);
    for (;;) {
        ssize_t n;
        if (len + 4096 > cap) {
            cap *= 2;
            buf = (unsigned char *)realloc(buf, cap + 1);
            assert(buf);
        }
        n = recv(sock, buf + len, 4096, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sock);
    buf[len] = 0;
    hdr_end = strstr((const char *)buf, "\r\n\r\n");
    if (!hdr_end || sscanf((const char *)buf, "HTTP/%*d.%*d %d", status_code) != 1) {
        free(buf); return 0;
    }
    if ((p = strstr((const char *)buf, "X-RateLimit-Remaining:")) && p < hdr_end)
        *rl_rem = atoi(p + 22);
    if ((p = strstr((const char *)buf, "Retry-After:")) && p < hdr_end)
        *retry_after = (uint32_t)atoi(p + 12);
    {
        size_t hlen = (size_t)(hdr_end + 4 - (const char *)buf);
        *body_len = len - hlen;
        *body = (unsigned char *)malloc(*body_len + 1);
        assert(*body);
        memcpy(*body, buf + hlen, *body_len);
        (*body)[*body_len] = 0;
    }
    free(buf);
    return 1;
}

int main(int argc, char **argv)
{
    const char *api_url, *game_dir, *cur_ver;
    int expected_result, fail_step, include_prereleases = 1;
    int status = 0, rl_rem = -1;
    uint32_t retry_after = 0;
    unsigned char *body = NULL, *sha_body = NULL, *zip_body = NULL;
    size_t body_len = 0, sha_len = 0, zip_len = 0;
    struct lu_schedule sched = { 1, 0, 0, 1 };
    struct lu_release rel;
    char expected_sha[65];
    enum lu_result r;

    assert(argc == 6 || argc == 7);
    api_url = argv[1];
    game_dir = argv[2];
    cur_ver = argv[3];
    expected_result = atoi(argv[4]);
    fail_step = atoi(argv[5]);
    if (argc == 7) include_prereleases = atoi(argv[6]);
    sched.include_prereleases = include_prereleases;

    http_get_local(api_url, &status, &rl_rem, &retry_after, &body, &body_len);
    r = lu_http_classify(status, rl_rem, retry_after, 1000, &sched);
    if (r == LU_OK) {
        r = lu_parse_release((const char *)body, body_len, "mdk2-coop", include_prereleases, &rel);
    }
    free(body);
    if (r == LU_OK) {
        int newer = lu_version_is_newer(cur_ver, rel.tag);
        if (newer < 0) r = LU_ERR_VERSION;
        else if (newer == 0) r = LU_UP_TO_DATE;
        else {
            http_get_local(rel.sha_url, &status, &rl_rem, &retry_after, &sha_body, &sha_len);
            if (status != 200 || sha_len != rel.sha_size) r = LU_ERR_SIDECAR;
            else r = lu_parse_sidecar((const char *)sha_body, sha_len, rel.zip_name, expected_sha);
            free(sha_body);
            if (r == LU_OK) {
                http_get_local(rel.zip_url, &status, &rl_rem, &retry_after, &zip_body, &zip_len);
                if (status != 200) r = LU_ERR_HTTP;
                else r = lu_verify_download(zip_body, zip_len, rel.zip_size, expected_sha);
                if (r == LU_OK)
                    r = lu_apply_package(game_dir, zip_body, zip_len, "mdk2-coop", cur_ver, rel.tag, fail_step);
                free(zip_body);
            }
        }
    }
    if ((int)r != expected_result) {
        fprintf(stderr, "Expected result %d, got %d\n", expected_result, (int)r);
        return 1;
    }
    return 0;
}
