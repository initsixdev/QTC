#include "test.h"
#include "qtc/protocol.h"
#include <string.h>

typedef struct { int calls; uint8_t data[128]; size_t len; } capture;
static void cb(const uint8_t *f, size_t n, void *u) { capture *c=u; c->calls++; c->len=n; memcpy(c->data,f,n); }
static void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
int main(void) {
    uint8_t cmd[256], wrapped[300]; size_t n, wn;
    n = qtc_cmd_app_start(cmd, sizeof(cmd), "QTC Terminal 1.0.0");
    ASSERT_TRUE(n > 8); ASSERT_EQ_INT(cmd[0],1); ASSERT_EQ_INT(cmd[1],3);
    n = qtc_cmd_device_query(cmd, sizeof(cmd)); ASSERT_EQ_INT(n,2); ASSERT_EQ_INT(cmd[0],0x16); ASSERT_EQ_INT(cmd[1],3);
    uint8_t prefix[6]={1,2,3,4,5,6}; n=qtc_cmd_send_direct(cmd,sizeof(cmd),prefix,1234,2,"hi");
    ASSERT_EQ_INT(n,15); ASSERT_EQ_INT(cmd[0],2); ASSERT_EQ_INT(cmd[2],2); ASSERT_TRUE(memcmp(cmd+7,prefix,6)==0);
    ASSERT_EQ_INT(qtc_protocol_wrap_command(cmd,n,wrapped,sizeof(wrapped),&wn),0); ASSERT_EQ_INT(wrapped[0],'<'); ASSERT_EQ_INT(wn,n+3);
    uint8_t incoming[]={'>',5,0,0x02,1,0,0,0}; qtc_serial_parser p; qtc_serial_parser_init(&p); capture cap={0};
    ASSERT_EQ_INT(qtc_serial_parser_feed(&p,incoming,2,cb,&cap),0); ASSERT_EQ_INT(cap.calls,0);
    ASSERT_EQ_INT(qtc_serial_parser_feed(&p,incoming+2,sizeof(incoming)-2,cb,&cap),0); ASSERT_EQ_INT(cap.calls,1); ASSERT_EQ_INT(cap.len,5);
    qtc_radio_event e; uint8_t channel[50]={0}; channel[0]=0x12; channel[1]=2; memcpy(channel+2,"Family",6); for(int i=0;i<16;i++)channel[34+i]=(uint8_t)(i+1);
    ASSERT_EQ_INT(qtc_protocol_parse(channel,sizeof(channel),&e),0); ASSERT_EQ_INT(e.type,QTC_RADIO_CHANNEL_INFO); ASSERT_STREQ(e.channel.name,"Family"); ASSERT_TRUE(e.channel.is_private);
    uint8_t msg[64]={0}; msg[0]=0x10; memcpy(msg+4,prefix,6); msg[10]=0xff; msg[11]=0; put32(msg+12,1700000000); memcpy(msg+16,"hello",5);
    ASSERT_EQ_INT(qtc_protocol_parse(msg,21,&e),0); ASSERT_EQ_INT(e.type,QTC_RADIO_CONTACT_MESSAGE); ASSERT_STREQ(e.message.conversation_key,"010203040506"); ASSERT_STREQ(e.message.text,"hello");
    char key[160]; strcpy(key,e.message.message_key); ASSERT_EQ_INT(qtc_protocol_parse(msg,21,&e),0); ASSERT_STREQ(key,e.message.message_key);
    uint8_t self[64]={0}; self[0]=5; self[2]=10; self[3]=22; put32(self+48,869525); put32(self+52,250000); self[56]=11; self[57]=5; memcpy(self+58,"Radio",5);
    ASSERT_EQ_INT(qtc_protocol_parse(self,63,&e),0); ASSERT_EQ_INT((int)e.freq,869); ASSERT_EQ_INT((int)e.bw,250); ASSERT_STREQ(e.radio_name,"Radio");
    uint8_t sent[10]={6,0,0x78,0x56,0x34,0x12,0x70,0x17,0,0};
    ASSERT_EQ_INT(qtc_protocol_parse(sent,sizeof(sent),&e),0); ASSERT_EQ_INT(e.type,QTC_RADIO_MSG_SENT);
    ASSERT_EQ_INT(e.expected_ack_len,4); ASSERT_EQ_INT(e.expected_ack[0],0x78); ASSERT_EQ_INT(e.suggested_timeout_ms,6000);
    uint8_t ack[9]={0x82,0x78,0x56,0x34,0x12,0x2c,1,0,0};
    ASSERT_EQ_INT(qtc_protocol_parse(ack,sizeof(ack),&e),0); ASSERT_EQ_INT(e.type,QTC_RADIO_ACK);
    ASSERT_EQ_INT(e.expected_ack_len,4); ASSERT_EQ_INT(e.round_trip_ms,300);
    uint8_t advert_push[33]={0x80};
    ASSERT_EQ_INT(qtc_protocol_parse(advert_push,sizeof(advert_push),&e),0);
    ASSERT_EQ_INT(e.type,QTC_RADIO_CONTACTS_DIRTY);
    advert_push[0]=0x81;
    ASSERT_EQ_INT(qtc_protocol_parse(advert_push,sizeof(advert_push),&e),0);
    ASSERT_EQ_INT(e.type,QTC_RADIO_CONTACTS_DIRTY);
    uint8_t pubkey[32]; for(int i=0;i<32;i++)pubkey[i]=(uint8_t)i;
    n=qtc_cmd_reset_path(cmd,sizeof(cmd),pubkey); ASSERT_EQ_INT(n,33); ASSERT_EQ_INT(cmd[0],13); ASSERT_TRUE(memcmp(cmd+1,pubkey,32)==0);
    n=qtc_cmd_export_self(cmd,sizeof(cmd)); ASSERT_EQ_INT(n,1); ASSERT_EQ_INT(cmd[0],17);
    uint8_t card[]={11,0xde,0xad,0xbe,0xef};
    ASSERT_EQ_INT(qtc_protocol_parse(card,sizeof(card),&e),0); ASSERT_EQ_INT(e.type,QTC_RADIO_EXPORT_CONTACT);
    ASSERT_EQ_INT(e.card_data_len,4); ASSERT_TRUE(memcmp(e.card_data,card+1,4)==0);
    n=qtc_cmd_set_radio_params(cmd,sizeof(cmd),867.5,250.0,10,5,false);
    ASSERT_EQ_INT(n,12); ASSERT_EQ_INT(cmd[0],11);
    ASSERT_EQ_INT(cmd[1],0xac); ASSERT_EQ_INT(cmd[2],0x3c); ASSERT_EQ_INT(cmd[3],0x0d); ASSERT_EQ_INT(cmd[4],0x00);
    ASSERT_EQ_INT(cmd[5],0x90); ASSERT_EQ_INT(cmd[6],0xd0); ASSERT_EQ_INT(cmd[7],0x03); ASSERT_EQ_INT(cmd[8],0x00);
    ASSERT_EQ_INT(cmd[9],10); ASSERT_EQ_INT(cmd[10],5); ASSERT_EQ_INT(cmd[11],0);
    puts("protocol tests passed"); return 0;
}
