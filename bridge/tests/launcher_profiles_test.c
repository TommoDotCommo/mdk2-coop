#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "launcher_profiles.h"
static const char *one = "COOP-PROFILES\t1\nFriends\t127.0.0.1\t27900\tnotes\t1234\n";
static void bad(const char *text)
{
    struct lp_store s, old;
    memset(&s, 0x5a, sizeof(s)); old = s;
    assert(!lp_decode(&s, text, strlen(text)));
    assert(!memcmp(&s, &old, sizeof(s)));
}
int main(void)
{
    struct lp_store s, again;
    char buffer[LP_BYTES];
    unsigned char query[8] = {'C','P','Q','1',0x78,0x56,0x34,0x12}, reply[64], copy[64];
    struct lp_probe p = {0};
    uint64_t number;
    int n, i;
    assert(lp_decode(&s, one, strlen(one)) && s.n == 1 && s.p[0].used == 1234);
    n = lp_encode(&s, buffer, sizeof(buffer)); assert(n == (int)strlen(one) && !strcmp(buffer, one));
    assert(lp_decode(&again, buffer, n) && !memcmp(&s, &again, sizeof(s)));
    assert(lp_decode(&s, "COOP-PROFILES\t0\nOld\tlocalhost\t10\n", 34) == 0); /* truncated/corrupt input */
    strcpy(buffer,"COOP-PROFILES\t0\nOld\tlocalhost\t10\n");
    assert(lp_decode(&s, buffer, strlen(buffer)) && s.n == 1 && !s.p[0].used && !s.p[0].notes[0]);
    bad("COOP-PROFILES\t2\n"); bad("COOP-PROFILES\t1\nA\tip\t0\t\t0\n");
    bad("COOP-PROFILES\t1\nA\tip\t65536\t\t0\n"); bad("COOP-PROFILES\t1\nA\tip\t-1\t\t0\n");
    bad("COOP-PROFILES\t1\nA\tip\t1\t\t18446744073709551616\n");
    bad("COOP-PROFILES\t1\nA\tbad host\t1\t\t0\n"); bad("COOP-PROFILES\t1\nA\thost\t1\tbad\rnotes\t0\n");
    bad("COOP-PROFILES\t1\nA\tip\t1\t\t0"); bad("COOP-PROFILES\t1\nA\tip\t1\t\t0\textra\n");
    bad("COOP-PROFILES\t1\nA\tip\t1\t\t0\nA\tip2\t2\t\t0\n");
    bad("COOP-PROFILES\t1\n\tip\t1\t\t0\n");
    strcpy(buffer,"COOP-PROFILES\t1\n");
    for(i=0;i<33;i++) { char row[100]; snprintf(row,sizeof(row),"Name%d\thost\t1\t\t0\n",i); strcat(buffer,row); }
    bad(buffer);
    memset(buffer, 'a', sizeof(buffer)); assert(!lp_decode(&s, buffer, sizeof(buffer)));
    memcpy(buffer,one,strlen(one)); buffer[20]=0; assert(!lp_decode(&s,buffer,strlen(one)));
    assert(!lp_number("", &number)); assert(!lp_number("+1", &number));
    assert(lp_decode(&s,one,strlen(one))); assert(!lp_encode(&s,buffer,16));
    assert(!lp_encode(&s,buffer,32));
    s.p[0].port = 0; assert(!lp_encode(&s,buffer,sizeof(buffer)));
    s.n = LP_MAX+1; assert(!lp_encode(&s,buffer,sizeof(buffer)));
    memset(&s,0,sizeof(s)); s.n=1; memset(s.p[0].name,'x',sizeof(s.p[0].name)-1); strcpy(s.p[0].host,"h");s.p[0].port=1;
    assert(lp_encode(&s,buffer,sizeof(buffer)));
    strcpy(s.p[0].name, "A"); s.p[1]=s.p[0];s.n=2;assert(!lp_encode(&s,buffer,sizeof(buffer)));
    assert(lp_due(&p,0)); lp_sent(&p,10,0x12345678); assert(!lp_due(&p,1209));
    assert(!lp_due(&p,1210) && p.status==LP_OFFLINE); assert(!lp_due(&p,1510)); assert(lp_due(&p,15010));
    lp_sent(&p,UINT32_MAX-100,0x12345678); p.pending=0; assert(!lp_due(&p,UINT32_MAX-50)); assert(!lp_due(&p,100)); assert(lp_due(&p,15000));
    n=lp_answer(query,8,3,0,"v1",reply);assert(n==14);
    assert(!lp_answer(query,7,3,0,"v1",reply));assert(!lp_answer(query,8,17,0,"v1",reply));
    query[3]='2';assert(!lp_answer(query,8,3,0,"v1",reply));query[3]='1';
    lp_sent(&p,100,0x12345678);
    assert(!lp_reply(&p,110,reply,n,0,"v1"));
    memcpy(copy,reply,n);copy[4]++;assert(!lp_reply(&p,110,copy,n,1,"v1"));
    assert(!lp_reply(&p,1300,reply,n,1,"v1"));
    assert(!lp_reply(&p,110,reply,n-1,1,"v1"));
    memcpy(copy,reply,n);copy[0]='X';assert(!lp_reply(&p,110,copy,n,1,"v1"));
    memcpy(copy,reply,n);copy[8]=17;assert(!lp_reply(&p,110,copy,n,1,"v1"));
    memcpy(copy,reply,n);copy[9]=0;assert(!lp_reply(&p,110,copy,n,1,"v1"));
    memcpy(copy,reply,n);copy[10]=3;assert(!lp_reply(&p,110,copy,n,1,"v1"));
    memcpy(copy,reply,n);copy[12]=0;assert(!lp_reply(&p,110,copy,n,1,"v1"));
    assert(lp_reply(&p,110,reply,n,1,"v1") && p.status==LP_LOBBY && p.rtt==10 && !strcmp(p.version,"v1"));
    assert(!lp_reply(&p,111,reply,n,1,"v1"));
    lp_sent(&p,100,0x12345678);assert(lp_reply(&p,110,reply,n,1,"v2") && p.status==LP_MISMATCH);
    reply[8]=16;lp_sent(&p,100,0x12345678);assert(lp_reply(&p,110,reply,n,1,"v1") && p.status==LP_FULL);
    reply[8]=3;reply[10]=1;lp_sent(&p,100,0x12345678);assert(lp_reply(&p,110,reply,n,1,"v1") && p.status==LP_GAME);
    reply[8]=1;reply[10]=2;lp_sent(&p,100,0x12345678);assert(lp_reply(&p,110,reply,n,1,"v1") && p.status==LP_REACHABLE);
    puts("PASS: store roundtrip, transactional corruption rejection, v0 migration, probe scheduling and validation");
    return 0;
}
