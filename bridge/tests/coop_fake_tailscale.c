/* Stand-in tailscale.exe for the Windows self-test: prints a status fixture (FAKE_PRINT) or
 * hangs (FAKE_HANG) so the caller's 2 s timeout is exercised. Ignores its arguments. */
#include <stdio.h>
#include <windows.h>
int main(void)
{
#ifdef FAKE_HANG
    Sleep(10000);   /* longer than the 2 s timeout, short enough that a broken timeout cannot leak it for long */
#else
    fputs("{\"BackendState\":\"Running\",\"Self\":{\"HostName\":\"me\",\"DNSName\":\"me.tail0.ts.net.\",\"TailscaleIPs\":[\"100.64.0.1\"]},"
          "\"Peer\":{\"a\":{\"HostName\":\"peer\",\"TailscaleIPs\":[\"100.64.0.2\",\"fd7a::2\"],\"Online\":true,\"OS\":\"windows\"},"
          "\"b\":{\"HostName\":\"shared\",\"TailscaleIPs\":[\"100.90.0.3\"],\"Online\":true,\"ShareeNode\":true},"
          "\"c\":{\"HostName\":\"off\",\"TailscaleIPs\":[\"100.64.0.4\"],\"Online\":false},"
          "\"d\":{\"HostName\":\"v6\",\"TailscaleIPs\":[\"fd7a::5\"],\"Online\":true}}}\n", stdout);
#endif
    return 0;
}
