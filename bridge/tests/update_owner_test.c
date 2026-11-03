/* One updater operation per run, for tools/test_update_ownership.py (which snapshots the folders around it):
 *   owner <game>                         prints 0 Setup, 1 copy (mod manager or by hand), 2 hard-linked (Vortex)
 *   startup <game> <data>                lu_startup + lu_recover_interrupted, prints the leftovers count
 *   apply <game> <data> <zip> <tag>      prints the lu_result of lu_apply_package
 *   rollback <game> <data>               prints the lu_result of lu_rollback */
#include <stdio.h>
#include <string.h>
#include "launcher_update.h"

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "owner")) {
        printf("%d\n", (int)lu_install_owner(argv[2]));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "startup")) {
        int left = lu_startup(argv[2], argv[3]);
        lu_recover_interrupted(argv[2], argv[3]);
        printf("%d\n", left);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "apply")) {
        unsigned char *zip;
        size_t len;
        if (!lu_read_bytes(argv[4], &zip, &len)) return 2;
        printf("%d\n", (int)lu_apply_package(argv[2], argv[3], zip, len, "mdk2-coop", "v0.3.5-alpha", argv[5], 0));
        free(zip);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "rollback")) {
        printf("%d\n", (int)lu_rollback(argv[2], argv[3]));
        return 0;
    }
    return 2;
}
