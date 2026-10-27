/* POSIX transport fixture around the production probe policy, for a local fake server. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include "launcher_profiles.h"
int main(int argc, char **argv)
{
    int sock, n, expected, flen;
    struct sockaddr_in to = {0}, from;
    unsigned char query[8] = {'C','P','Q','1',0x78,0x56,0x34,0x12}, reply[128];
    struct lp_probe p = {0};
    fd_set ready;
    struct timeval timeout = {0,300000};
    assert(argc==3); expected=atoi(argv[2]);
    sock=socket(AF_INET,SOCK_DGRAM,0); assert(sock>=0);
    to.sin_family=AF_INET;to.sin_addr.s_addr=htonl(INADDR_LOOPBACK);to.sin_port=htons(atoi(argv[1]));
    lp_sent(&p,100,0x12345678);
    assert(sendto(sock,query,sizeof(query),0,(struct sockaddr *)&to,sizeof(to))==sizeof(query));
    FD_ZERO(&ready);FD_SET(sock,&ready);
    while(select(sock+1,&ready,NULL,NULL,&timeout)>0) {
        flen=sizeof(from);n=recvfrom(sock,reply,sizeof(reply),0,(struct sockaddr *)&from,(socklen_t *)&flen);
        if(n>0 && lp_reply(&p,110,reply,n,from.sin_addr.s_addr==to.sin_addr.s_addr && from.sin_port==to.sin_port,"v1")) break;
        FD_ZERO(&ready);FD_SET(sock,&ready);
    }
    if(p.pending) lp_due(&p,1300);
    close(sock);assert(p.status==expected);printf("PASS: fake endpoint status %d\n",p.status);return 0;
}
