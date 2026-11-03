#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
#include <string.h>
#include "launcher_invite_command.h"
int main(void)
{
    char out[LI_INPUT_CAP];
    const wchar_t *good[] = { L"C:\\Game folder\\mdk2hd.exe", L"--coop-invite", L"mdk2-coop://MDK2-ABC12-DE345" };
    const wchar_t *missing[] = { L"game.exe", L"--coop-invite" };
    const wchar_t *duplicate[] = { L"game.exe", L"--coop-invite", L"first", L"--coop-invite", L"second" };
    const wchar_t *lookalike[] = { L"game.exe", L"prefix--coop-invite", L"wrong" };
    const wchar_t *empty[] = { L"game.exe", L"--coop-invite", L"" };
    const wchar_t *control[] = { L"game.exe", L"--coop-invite", L"invite\nmore" };
    const wchar_t *unicode[] = { L"game.exe", L"--coop-invite", L"\x2603" };
    assert(li_invite_argument(3, good, out, sizeof(out)) == 1);
    assert(!strcmp(out, "mdk2-coop://MDK2-ABC12-DE345"));
    assert(li_invite_argument(3, good, out, 5) == -1 && !out[0]);
    assert(li_invite_argument(2, missing, out, sizeof(out)) == -1 && !out[0]);
    assert(li_invite_argument(5, duplicate, out, sizeof(out)) == -1 && !out[0]);
    assert(li_invite_argument(3, lookalike, out, sizeof(out)) == 0 && !out[0]);
    assert(li_invite_argument(3, empty, out, sizeof(out)) == -1 && !out[0]);
    assert(li_invite_argument(3, control, out, sizeof(out)) == -1 && !out[0]);
    assert(li_invite_argument(3, unicode, out, sizeof(out)) == -1 && !out[0]);
#ifdef _WIN32
    {
        int argc;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        assert(argv);
        if (argc > 1) {
            assert(li_invite_argument(argc, (const wchar_t *const *)argv, out, sizeof(out)) == 1);
            assert(!strcmp(out, "mdk2-coop://MDK2-ABC12-DE345"));
        }
        LocalFree(argv);
    }
#endif
    return 0;
}
