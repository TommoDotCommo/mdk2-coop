/* Tiny command adapter. Inputs contain hashes, flags or hex-encoded UTF-8 text;
 * stdout is one integer. The same pure header is exercised by native T0 tests. */
#include <stdio.h>
#include <stdlib.h>
#include "install-policy.h"
#include "../bridge/build_sigs.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
/* "build <exe>": which known game build the file is, read from disk without running it.
 * Prints the CB_SIGS index (-1 unknown) and, on a second line, the build name. The path may hold any character, so on
 * Windows it is taken from the wide command line. Exit 2 when the file cannot be read. */
static int build_of(const char *narrow)
{
    FILE *f;
    unsigned char *buf;
    long n;
    int index;
#ifdef _WIN32
    int argc = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)narrow;
    if (!wide || argc != 3) return 2;
    f = _wfopen(wide[2], L"rb");
#else
    f = fopen(narrow, "rb");
#endif
    if (!f) return 2;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 64L * 1024 * 1024 || fseek(f, 0, SEEK_SET)) { fclose(f); return 2; }
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 2; }
    fclose(f);
    index = cb_identify(buf, (size_t)n);
    free(buf);
    printf("%d\n%s\n", index, index >= 0 ? CB_SIGS[index].name : "unknown");
    return 0;
}
static const char *value(const char *s) { return !strcmp(s, "-") ? "" : s; }
int main(int argc, char **argv)
{
    int result;
    if (argc == 3 && !strcmp(argv[1], "build")) return build_of(argv[2]);
    if (argc == 10 && !strcmp(argv[1], "file")) {
        result = ci_file(value(argv[2]), value(argv[3]), atoi(argv[4]), value(argv[5]),
                         value(argv[6]), value(argv[7]), value(argv[8]), atoi(argv[9]));
    } else if (argc == 4 && !strcmp(argv[1], "actions")) {
        result = ci_actions((unsigned)strtoul(argv[2], NULL, 10), atoi(argv[3]));
    } else if (argc == 3 && !strcmp(argv[1], "loader")) {
        char text[8193];
        size_t n = strlen(argv[2]), i;
        if (n > 16384 || n % 2) return 2;
        for (i = 0; i < n / 2; i++) {
            unsigned c;
            char pair[3] = { argv[2][2*i], argv[2][2*i+1], 0 };
            if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%x", &c) != 1 || !c) return 2;
            text[i] = (char)c;
        }
        text[n/2] = 0;
        result = ci_loader(text);
    } else return 2;
    printf("%d\n", result);
    return 0;
}
