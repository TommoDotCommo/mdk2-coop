#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "snapshot.h"
typedef unsigned char BYTE;
typedef uint32_t DWORD;
#define PLAYER_NAME 16
#define EV_NAME 32
static int sent_size, sent_slot;
static BYTE sent[1400];
#include PACKET_SOURCE
static void send_to(int slot, const void *buf, int len)
{
    assert(len > 0 && len <= 1400);
    sent_size = len; sent_slot = slot; memcpy(sent,buf,len);
}
int main(void)
{
    struct player_packet packet = {0}, received;
    BYTE wire[1400];
    int i,j,n;
    packet.magic=PLAYER_MAGIC; packet.flags=PK_STATE;
    packet.state.quat[0]=1; packet.state.nanims=SNAP_ANIMS;
    packet.state.cur=packet.state.prev=NO_ANIM;
    packet.state.canopy.present=1; packet.state.canopy.cur=66; packet.state.canopy.prev=67;
    for(i=0;i<SNAP_ANIMS;i++) {
        packet.state.anims[i].id=i;
        for(j=0;j<ANIM_STATE_DWORDS;j++) packet.state.anims[i].state[j]=0x40404040;
    }
    packet.nevents=PACKET_EVENTS;
    for(i=0;i<PACKET_EVENTS;i++) packet.events[i].id=i+1;
    assert(!packet_pack(&packet,wire,sizeof(wire))); /* full state plus queue exceeds MTU */
    send_packet(2,&packet);
    assert(sent_size>0 && sent_slot==2);
    assert(packet_unpack(sent,sent_size,&received));
    assert(received.nevents>0 && received.nevents<PACKET_EVENTS);
    assert(received.state.canopy.present && received.state.canopy.cur==66);
    for(i=0;i<received.nevents;i++) assert(received.events[i].id==(unsigned int)i+1);
    packet.nevents=0; packet.flags=0; packet.magic=0x434b444d;
    n=packet_pack(&packet,wire,sizeof(wire)); assert(n>0);
    assert(!packet_unpack(wire,n,&received)); /* old protocol cannot apply */
    packet.magic=PLAYER_MAGIC; packet.flags=PK_STATE;
    n=packet_pack(&packet,wire,sizeof(wire)); assert(n>0);
    for(i=0;i<n;i++) assert(!packet_unpack(wire,i,&received));
    assert(packet_unpack(wire,n,&received));
    puts("PASS production packets: bounded MTU with reliable backlog, legacy rejection, truncation");
    return 0;
}
