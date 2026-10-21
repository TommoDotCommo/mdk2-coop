/*
 * Upload accounting: every datagram we send is counted by kind (own player state with
 * events, relayed player state, enemy snapshots, other) including the 28 bytes of UDP/IPv4
 * header, and logged every 10 s as kbit/s, in total and per link. Tools read the
 * "net out:" lines (tools/netmeasure.sh).
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

#define UDP_IP_HEADER   28
#define STATS_MS        10000.0

static const char *ns_name[NS_KINDS] = { "player", "relay", "ent", "voice", "other" };
static double bytes[NS_KINDS], packets[NS_KINDS];
static double since;

void netstats_sent(int kind_hint, int len)
{
    double now = now_ms();
    int k = kind_hint >= 0 && kind_hint < NS_KINDS ? kind_hint : NS_OTHER;
    int i, links;
    double total = 0, secs;
    char line[256];
    int n;

    if (since == 0.0)
        since = now;
    bytes[k] += len + (net_steam ? 0 : UDP_IP_HEADER);  /* Steam: payload only; relay overhead is unknown */
    packets[k] += 1;
    if (now - since < STATS_MS)
        return;
    secs = (now - since) / 1000.0;
    for (i = 0; i < NS_KINDS; i++)
        total += bytes[i];
    links = players_connected();
    n = snprintf(line, sizeof(line), "net out: %.0f kbit/s to %d link%s (%.0f per link):", total * 8 / 1000 / secs,
                 links, links == 1 ? "" : "s", links ? total * 8 / 1000 / secs / links : 0.0);
    for (i = 0; i < NS_KINDS && n < (int)sizeof(line); i++)
        n += snprintf(line + n, sizeof(line) - n, " %s %.0f (%.0f pk/s, %.0f B)", ns_name[i],
                      bytes[i] * 8 / 1000 / secs, packets[i] / secs, packets[i] ? bytes[i] / packets[i] : 0.0);
    logmsg("%s", line);
    memset(bytes, 0, sizeof(bytes));
    memset(packets, 0, sizeof(packets));
    since = now;
}
