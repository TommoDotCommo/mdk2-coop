/* Tiny command adapter. Inputs contain hashes, flags or hex-encoded UTF-8 text;
 * stdout is one integer. The same pure header is exercised by native T0 tests. */
#include <stdio.h>
#include <stdlib.h>
#include "install-policy.h"
static const char *value(const char *s) { return !strcmp(s, "-") ? "" : s; }
int main(int argc, char **argv)
{
    int result;
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
