#include "test.h"
#include "qtc/ipc.h"
#include <sys/socket.h>
#include <unistd.h>

typedef struct {int calls;qtc_ipc_frame f;} cap;
static void cb(const qtc_ipc_frame *f,void *u){cap*c=u;c->calls++;c->f=*f;}
int main(void){
 int sv[2];ASSERT_EQ_INT(socketpair(AF_UNIX,SOCK_STREAM,0,sv),0);const char p[]="hello";ASSERT_EQ_INT(qtc_ipc_send(sv[0],42,p,sizeof(p)),0);qtc_ipc_frame f;ASSERT_EQ_INT(qtc_ipc_recv_blocking(sv[1],&f,1000),0);ASSERT_EQ_INT(f.type,42);ASSERT_EQ_INT(f.length,sizeof(p));ASSERT_STREQ((char*)f.payload,"hello");close(sv[0]);close(sv[1]);
 qtc_ipc_reader r;qtc_ipc_reader_init(&r);uint8_t raw[]={3,0,0,0,9,'a','b','c'};cap c={0};ASSERT_EQ_INT(qtc_ipc_reader_feed(&r,raw,2,cb,&c),0);ASSERT_EQ_INT(c.calls,0);ASSERT_EQ_INT(qtc_ipc_reader_feed(&r,raw+2,6,cb,&c),0);ASSERT_EQ_INT(c.calls,1);ASSERT_EQ_INT(c.f.type,9);ASSERT_TRUE(memcmp(c.f.payload,"abc",3)==0);puts("ipc tests passed");return 0;
}
