# SPDX-License-Identifier: GPL-3.0-or-later
"""Real receiver/service with explicit socket/Sony mocks; no console input/output."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload',type=Path,default=root/'payload')
parser.add_argument('--multi-only',action='store_true')
opts=parser.parse_args();payload=opts.payload
source=(payload/'wireless_ds4.c').read_text();end=source.index('} RemotePadReaderArgs;')+len('} RemotePadReaderArgs;');start=source.rfind('typedef struct {',0,end)
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "wireless_ds4.h"
#define POORDS4_REMOTE_PAD_CAPACITY 256u
#define MSG_DONTWAIT 0x80
''' + source[start:end]+r'''
#include "reader_rumble_channel.h"
static uint64_t clock_ns=1000000000;
static int mode_calls,vibrate_calls,close_calls,narrow_identity=1,device_result,device_fill=1;
static uint16_t device_vendor=0x054c,device_product=0x05c4;
static uint8_t motors[2],packet[49];static intptr_t packet_length=-1;
static uint8_t slot_motors[4][2];static unsigned slot_calls[4];
static int native_clock(int id,void *out){if(id!=4)return -1;int64_t *t=out;t[0]=clock_ns/1000000000;t[1]=clock_ns%1000000000;return 0;}
static int identity(int h){return h>=44 && h<48?narrow_identity:0;}
static int device_info(int h,void *out){
    if(h!=44)return -1;
    if(device_result)return device_result;
    if(device_fill){uint8_t *b=out;memset(b,0,256);b[16]=device_vendor;b[17]=device_vendor>>8;b[18]=device_product;b[19]=device_product>>8;}
    return 0;
}
static int mode(int h,int value){if(h<44 || h>=48 || value!=2)return -1;mode_calls++;return 0;}
static int vibrate(int h,const void *value){if(h<44 || h>=48)return -1;vibrate_calls++;memcpy(motors,value,2);memcpy(slot_motors[h-44],value,2);slot_calls[h-44]++;return 0;}
static int close_socket(int fd){if(fd!=17)return -1;close_calls++;return 0;}
static intptr_t receive(int fd,void *out,size_t length,int flags,void *addr,void *addr_length){
    if(fd!=17 || length!=49 || flags!=MSG_DONTWAIT || addr || addr_length)return -2;
    intptr_t n=packet_length;packet_length=-1;if(n<0)return -1;memcpy(out,packet,(size_t)n);return n;
}
static void enqueue(PoorDS4OutputCommand command,intptr_t length){memcpy(packet,&command,sizeof command);packet_length=length;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d\n",__LINE__);return 1;}}while(0)
static int multi(void){
    RemotePadReaderTransport t={0};t.socket_fd=17;t.fp_recvfrom=(intptr_t)receive;t.fp_socket_close=(intptr_t)close_socket;
    t.input.pad_handle=44;t.input.last_result=0;
    t.output.magic=POORDS4_READER_OUTPUT_MAGIC;t.output.version=1;t.output.size=sizeof t.output;t.output.slots=4;t.output.session=88;
    t.output.fp_clock=(intptr_t)native_clock;t.output.fp_is_ds4=(intptr_t)identity;t.output.fp_mode=(intptr_t)mode;t.output.fp_vibration=(intptr_t)vibrate;
    for(unsigned i=0;i<4;i++){
        t.input.slot_handles[i]=44+i;t.input.slot_results[i]=0;
        ScePadData *p=i?(void*)t.input.slot_data[i]:(void*)t.input.pad_data;p->connected=1;p->count=i+1;
        t.output.channels[i].state.handle=44+i;t.output.channels[i].state.user=9+i;
    }
    reader_rumble_service(&t,0);
    CHECK(t.output.channels[1].state.connected && t.output.channels[1].state.epoch==1);
    for(unsigned i=0;i<4;i++){
        PoorDS4OutputCommand c={.session=88,.epoch=1,.sequence=1,.deadline=1500000000,.handle=44+i,.user=9+i,.vibration={31+i,15+i}};
        enqueue(c,48);reader_rumble_service(&t,0);
        CHECK(t.output.channels[i].state.applied_sequence==1 && slot_calls[i]==1);
        CHECK(slot_motors[i][0]==31+i && slot_motors[i][1]==15+i);
        for(unsigned other=0;other<4;other++)CHECK(slot_calls[other]==(unsigned)(other<=i));
    }
    ((ScePadData*)(void*)t.input.slot_data[1])->connected=0;
    reader_rumble_service(&t,0);CHECK(slot_calls[1]==2 && !slot_motors[1][0] && !slot_motors[1][1]);
    CHECK(slot_calls[0]==1 && slot_calls[2]==1 && slot_calls[3]==1);
    PoorDS4OutputCommand stale={.session=88,.epoch=1,.sequence=2,.deadline=1500000000,.handle=45,.user=10,.vibration={90,91}};
    enqueue(stale,48);reader_rumble_service(&t,0);CHECK(slot_calls[1]==2 && t.output.channels[1].state.rejection==1);
    ((ScePadData*)(void*)t.input.slot_data[1])->connected=1;
    reader_rumble_service(&t,0);CHECK(t.output.channels[1].state.epoch==3);
    enqueue(stale,48);reader_rumble_service(&t,0);CHECK(slot_calls[1]==2);
    stale.sequence=3;stale.epoch=3;enqueue(stale,48);reader_rumble_service(&t,0);
    CHECK(slot_calls[1]==3 && slot_motors[1][0]==90 && slot_motors[1][1]==91);
    reader_rumble_service(&t,1);CHECK(close_calls==1 && t.socket_fd==-1);
    for(unsigned i=0;i<4;i++)CHECK(!slot_motors[i][0] && !slot_motors[i][1] && !t.output.channels[i].state.effect_active);
    puts("actual four-channel receive/service: independent motor commands, source-specific disconnect/reconnect, stale refusal and all-channel exit neutral");return 0;
}
int main(int argc,char **argv){
    if(argc>1 && !strcmp(argv[1],"multi"))return multi();
    RemotePadReaderTransport t={0};t.socket_fd=17;t.fp_recvfrom=(intptr_t)receive;t.fp_socket_close=(intptr_t)close_socket;
    t.input.pad_handle=44;t.input.last_result=0;
    ScePadData *pad=(void*)t.input.pad_data;pad->connected=1;pad->count=1;
    t.output.magic=POORDS4_READER_OUTPUT_MAGIC;t.output.version=1;t.output.size=sizeof t.output;t.output.slots=4;t.output.session=88;
    t.output.fp_clock=(intptr_t)native_clock;t.output.fp_is_ds4=(intptr_t)identity;t.output.fp_mode=(intptr_t)mode;t.output.fp_vibration=(intptr_t)vibrate;
    for(unsigned i=0;i<4;i++){t.output.channels[i].state.handle=-1;t.output.channels[i].state.user=-1;}
    t.output.channels[0].state.handle=44;t.output.channels[0].state.user=9;
    reader_rumble_service(&t,0);CHECK(t.output.channels[0].state.epoch==1 && !vibrate_calls);
    PoorDS4OutputCommand c={.session=88,.epoch=1,.sequence=1,.deadline=1500000000,.handle=44,.user=9,.vibration={31,15}};
    enqueue(c,48);reader_rumble_service(&t,0);CHECK(mode_calls==1 && vibrate_calls==1 && motors[0]==31 && motors[1]==15);
    CHECK(t.output.channels[0].state.applied_sequence==1 && t.output.channels[0].state.effect_active);
    enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==1);
    c.sequence=2;c.session=99;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==1);
    c.session=88;c.user=10;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==1);
    c.user=9;c.handle=45;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==1);
    c.handle=44;enqueue(c,47);reader_rumble_service(&t,0);CHECK(vibrate_calls==1);
    enqueue(c,49);reader_rumble_service(&t,0);CHECK(vibrate_calls==1);
    c.epoch=7;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==1 && t.output.channels[0].state.rejection==1);
    c.epoch=1;c.sequence=3;c.vibration=(PoorDS4Vibration){0,0};enqueue(c,48);reader_rumble_service(&t,0);
    CHECK(vibrate_calls==2 && !motors[0] && !motors[1] && !t.output.channels[0].state.effect_active);
    c.sequence=4;c.vibration=(PoorDS4Vibration){31,15};enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==3);
    clock_ns=1600000000;reader_rumble_service(&t,0);CHECK(vibrate_calls==4 && !t.output.channels[0].state.effect_active);
    c.sequence=5;c.deadline=2000000000;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==5);
    reader_rumble_service(&t,1);CHECK(vibrate_calls==6 && !motors[0] && !motors[1] && close_calls==1 && t.socket_fd==-1);
    t.socket_fd=17;t.output.fp_device_info=(intptr_t)device_info;
    t.output.accepted_vendor=0x054c;t.output.accepted_product=0x05c4;
    memset(&t.output.channels[0],0,sizeof t.output.channels[0]);
    t.output.channels[0].state.handle=44;t.output.channels[0].state.user=9;
    narrow_identity=0;reader_rumble_service(&t,0);
    c=(PoorDS4OutputCommand){.session=88,.epoch=1,.sequence=1,.deadline=2000000000,.handle=44,.user=9};
    enqueue(c,48);reader_rumble_service(&t,0);
    CHECK(vibrate_calls==7 && t.output.channels[0].state.applied_sequence==1);
    CHECK(t.output.channels[0].state.identity_path==2 && t.output.channels[0].state.device_vendor==0x054c);
    c.sequence++;device_product=0x0ce6;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==7 && t.output.channels[0].state.rejection==2);
    c.sequence++;device_product=0x05c4;device_vendor=0x045e;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==7);
    c.sequence++;device_vendor=0x054c;device_result=-9;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==7);
    c.sequence++;device_result=0;device_fill=0;enqueue(c,48);reader_rumble_service(&t,0);CHECK(vibrate_calls==7);
    c.sequence++;device_fill=1;narrow_identity=0x030d0301;enqueue(c,48);reader_rumble_service(&t,0);
    CHECK(vibrate_calls==8 && t.output.channels[0].state.identity_path==1);
    puts("valid delivery/native ack; command refusal; zero/expiry/exit stop; public DS4 identity with narrow zero, DS5/vendor/missing/error refusal, positive handle identity");return 0;
}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='reader-rumble-') as tmp:
    c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
    build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
    run=subprocess.run([str(exe)]+(['multi'] if opts.multi_only else []),capture_output=True,text=True) if not build.returncode else None
    passed=run is not None and run.returncode==0
    print(json.dumps({'compiled':not build.returncode,'passed':passed,'exit':run.returncode if run else None,'output':run.stdout.strip() if run else '', 'error':run.stderr if run else build.stderr[-2000:],'sources':{name:hashlib.sha256((payload/name).read_bytes()).hexdigest() for name in ('wireless_ds4.c','wireless_ds4.h','reader_rumble_channel.h','pad_feedback.h')},'native_console_verified':False},indent=2))
sys.exit(0 if passed else 1)
