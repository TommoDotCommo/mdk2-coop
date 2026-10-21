/* Use the exact in-game parser before a runner starts or changes any game copies. */
#include "../bridge/route.h"
int main(int argc, char **argv)
{
    struct route r;
    char error[160];
    FILE *f;
    if (argc != 2 || !(f = fopen(argv[1], "r"))) { fprintf(stderr, "cannot open route\n"); return 1; }
    int ok = route_load(f, &r, error, sizeof(error));
    fclose(f);
    if (!ok) { fprintf(stderr, "%s\n", error); return 1; }
    printf("%d %d %u %u\n", r.start.level, r.start.cp, r.duration, r.count);
    route_free(&r);
    return 0;
}
