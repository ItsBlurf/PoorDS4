# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the complete copied production reader with explicit Sony host models.

The primary stays healthy while a DS4 appears for an already known user.
This does not emulate or inject console input and is not physical acceptance.
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--payload',type=Path,default=ROOT/'payload')
payload=p.parse_args().payload
source=(payload/'wireless_ds4.c').read_text()
has_discovery=(payload/'source_device_discovery.h').exists() and 'poords4_source_discovery_poll(' in source
end=source.index('} RemotePadReaderArgs;')+len('} RemotePadReaderArgs;')
types=source[source.rfind('typedef struct {',0,end):end]
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
#define POORDS4_REMOTE_PAD_CAPACITY 256
#define MSG_DONTWAIT 0x80
'''+types+r'''
#include "reader_rumble_channel.h"
static unsigned frames,lookups,open_calls,closes,primary_reads,bad,checks;
static unsigned probe_open_frame,probe_live,probe_longest;
static int mode;
static RemotePadReaderTransport *active;
#define EXPECT(x) do{checks++;if(!(x)){fprintf(stderr,"mode=%d line=%d check=%s\n",mode,__LINE__,#x);return 1;}}while(0)
static int32_t model_state(int32_t h,void *out){
 ScePadData *s=out;memset(s,0,sizeof *s);s->connected=1;s->timestamp=frames*8333;s->count=1;
 if(mode==19 || mode==20){s->buttons=1u<<((unsigned)h%8);s->leftStick.x=(uint8_t)(h*3+7);s->analogButtons.l2=(uint8_t)(h+30);s->count=(uint8_t)h;s->timestamp+=(unsigned)h;}
 if(h==44)primary_reads++;
 if(h==55 && (mode==8 || mode==15))s->connected=0;
 if(h==55 && mode>=21)s->connected=probe_live && mode!=22 && frames-probe_open_frame>=16;
 return 0;
}
static int32_t model_events(int32_t h,void *out,int32_t count){
 if(mode==19){model_state(h,out);return count==1?1:-1;}
 if(h==55 && mode==15){model_state(h,out);((ScePadData*)out)->connected=1;return count==1?1:-1;}return 0;
}
static void model_sleep(unsigned us){(void)us;if(++frames>=((mode==23 || mode==24)?200u:1000u))active->input.stop=1;}
static int32_t model_close(int32_t h){
 if(h==44)bad++;else if(h!=55)bad++;closes++;
 if(mode>=21){if(!probe_live)bad++;probe_live=0;unsigned age=frames-probe_open_frame;if(age>probe_longest)probe_longest=age;}
 return mode==12 || mode==24?-1:0;
}
static int32_t model_lookup(uintptr_t u,uintptr_t type,uintptr_t i,uintptr_t z,uintptr_t z2,uintptr_t z3){
 lookups++;if(type||z||z2||z3)bad++;
 if(u==11 && i==0)return 44;
 if(u!=22 || i!=0 || frames<20)return -1;
 if(mode>=21)return probe_live?55:-1;
 if(mode==1 || mode==11 || mode==12 || mode==14)return -1;
 if(mode==2)return -1;
 if(mode==9 && frames>=190)return -1;
 return 55;
}
static int32_t model_open(uintptr_t u,uintptr_t type,uintptr_t i,uintptr_t z,uintptr_t z2,uintptr_t z3){
 open_calls++;if(type||z||z2||z3)bad++;
 if(u!=22 || i!=0 || frames<20)return -1;
 if(mode>=21){if(probe_live)bad++;probe_live=1;probe_open_frame=frames;return 55;}
 if(mode==11)return 44;
 return (mode==1 || mode==12 || mode==14)?55:-1;
}
static int32_t model_id(uintptr_t h,uintptr_t out,uintptr_t z,uintptr_t z2,uintptr_t z3,uintptr_t z4){
 if(h!=55 || z||z2||z3||z4)bad++;
 if(mode==6)return -1;
 if(mode>=21 && (!probe_live || frames-probe_open_frame<(mode==25?48u:16u))){*(int32_t*)out=-1;return 0;}
 *(int32_t*)out=mode==10?(int)(frames/16):900;return 0;
}
static int32_t model_info(uintptr_t id,uintptr_t out,uintptr_t z,uintptr_t z2,uintptr_t z3,uintptr_t z4){
 if((mode!=10 && id!=900)||z||z2||z3||z4)bad++;
 if(mode==7)return -1;
 uint8_t *v=(uint8_t*)out;v[16]=0x4c;v[17]=5;
 unsigned product=mode==3||mode==14?0x0ce6:mode==4?0x0df2:mode==5?0:0x05c4;
 v[18]=(uint8_t)product;v[19]=(uint8_t)(product>>8);return 0;
}
void *'''+function(source,'remote_pad_reader_stub')+r'''
static int exercise(int scenario){
 mode=scenario;frames=lookups=open_calls=closes=primary_reads=bad=0;
 probe_open_frame=probe_live=probe_longest=0;
 RemotePadReaderTransport t={0};active=&t;t.socket_fd=-1;
 t.input.pad_handle=44;t.input.slot_handles[0]=44;
 for(unsigned s=1;s<4;s++)t.input.slot_handles[s]=-1;
 t.input.fp_readstate=(intptr_t)model_state;t.input.fp_read=(intptr_t)model_events;
 t.input.fp_usleep=(intptr_t)model_sleep;t.input.fp_closepad=(intptr_t)model_close;
 for(unsigned s=0;s<4;s++){t.output.channels[s].state.handle=-1;t.output.channels[s].state.user=-1;}
#if HAVE_DISCOVERY
 PoorDS4SourceDiscovery *d=&t.discovery;d->magic=POORDS4_DISCOVERY_MAGIC;
 d->fp_gethandle=(intptr_t)model_lookup;d->fp_open=(intptr_t)model_open;
 d->fp_getdeviceid=(intptr_t)model_id;d->fp_getdeviceinfo=(intptr_t)model_info;
 d->users[0]=11;d->users[1]=22;d->user_count=2;d->admitted_count=1;d->interval_frames=16;
 d->admitted_users[0]=11;d->admitted_indices[0]=0;
 if(mode==13)t.input.slot_handles[1]=55;
 if(mode==16 || mode==17 || mode==19 || mode==20){
  d->admitted_count=4;
  for(unsigned s=1;s<4;s++){d->admitted_users[s]=(mode==17 && s==1)?22:30+(int)s;d->admitted_indices[s]=0;t.input.slot_handles[s]=46+(int)s;}
 }
 if(mode==18)d->fp_getdeviceid=0;
#endif
 remote_pad_reader_stub(&t);
 EXPECT(t.input.ready==2 && primary_reads==((mode==23 || mode==24)?200u:1000u) && t.input.read_error_frames==0 && !bad);
 int expect_refresh=mode==0 || mode==1 || mode==15 || mode==17 || mode==21 || mode==25;
#if HAVE_DISCOVERY
 EXPECT((int)d->refresh_requested==expect_refresh);
 if(expect_refresh){EXPECT(d->refresh_user==22 && d->refresh_index==0);EXPECT(d->queries<lookups+1);}
 EXPECT(lookups<=1000/16 && open_calls<=lookups);
 if(mode==1){EXPECT(d->opened>=1 && d->opened==d->closed && d->closed==closes);}
 if(mode==12){EXPECT(d->disabled && d->close_errors==1 && closes==1 && d->opened==1);}
 if(mode==14){EXPECT(d->opened==d->closed && d->opened>0 && !d->refresh_requested);}
 if(mode==0 || mode==11 || mode==13){EXPECT(closes==0);}
 if(mode==16){EXPECT(d->opened==0 && !d->refresh_requested);}
 if(mode==18){EXPECT(lookups==0 && open_calls==0);}
 if(mode==19 || mode==20){
  EXPECT(d->opened==0 && !d->refresh_requested);
  EXPECT(t.input.read_success_frames==(mode==19?1000u:0u));
  for(unsigned slot=0;slot<4;slot++){
   int handle=slot?46+(int)slot:44;
   const ScePadData *pad=(const ScePadData *)(slot?t.input.slot_data[slot]:t.input.pad_data);
   EXPECT(t.input.slot_results[slot]==0 && t.input.slot_seqs[slot]==2000);
   EXPECT(pad->connected && pad->buttons==(1u<<((unsigned)handle%8)));
   EXPECT(pad->leftStick.x==(uint8_t)(handle*3+7) && pad->analogButtons.l2==(uint8_t)(handle+30));
   EXPECT(pad->count==(uint8_t)handle && pad->timestamp==999u*8333u+(unsigned)handle);
  }
 }
 if(mode==11){EXPECT(d->identity_queries==0);}
 if(mode>=21){
  EXPECT(!probe_live && probe_longest>=16 && probe_longest<=64);
  if(mode!=24)EXPECT(d->opened==d->closed && d->closed==closes);
  if(mode==21 || mode==25)EXPECT(d->opened==1 && closes==1);
  if(mode==23)EXPECT(d->opened==1 && closes==1 && !d->refresh_requested);
  if(mode==24)EXPECT(d->opened==1 && closes==1 && d->close_errors==1 && d->disabled);
 }
#else
 /* A legacy reader can only request its existing reset/refresh path. Check
  * that actual observable output, rather than asserting the test expectation. */
 EXPECT((t.input.reset_requested!=0)==expect_refresh);
#endif
 return 0;
}
int main(void){for(int s=0;s<26;s++)if(exercise(s))return 1;printf("checks=%u cases=26 healthy-primary=1 four-distinct-reader-feeds=queued+state delayed-identity=16+48 timeout=64 pending-stop+close-failure=1\n",checks);return 0;}
'''
program='#define HAVE_DISCOVERY '+str(int(has_discovery))+'\n'+program
with tempfile.TemporaryDirectory(prefix='poords4-discovery-') as td:
 td=Path(td);src=td/'model.c';exe=td/'model.exe';src.write_text(program)
 build=subprocess.run([os.environ.get('POORDS4_HOST_CC','gcc'),'-std=c11','-O2','-Wall','-I',str(payload),str(src),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if build.returncode==0 else None
 names=['wireless_ds4.c','wireless_ds4.h','reader_rumble_channel.h']
 if has_discovery:names.append('source_device_discovery.h')
 result={'compiled':build.returncode==0,'passed':bool(run and run.returncode==0),'exit':run.returncode if run else None,'output':run.stdout.strip()if run else '', 'error':run.stderr if run else build.stderr[-5000:],'sources':{n:hashlib.sha256((payload/n).read_bytes()).hexdigest()for n in names},'native_controller_join_verified':False}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
