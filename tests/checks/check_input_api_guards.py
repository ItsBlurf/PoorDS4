# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real queued callbacks and reset routing with native API controls."""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--payload',type=Path,required=True)
payload=p.parse_args().payload;s=(payload/'wireless_ds4.c').read_text(encoding='utf-8-sig')
types=s[s.index('#define POORDS4_GAME_BRIDGE_MAGIC'):s.index('static pid_t g_game_bridge_direct_pid')]
begin=s.rfind('static __attribute__',0,s.index('\ngame_bridge_find_slot('))
end=s.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',begin)
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+s[begin:end]+'\n'.join('int32_t\n'+function(s,n)for n in ['game_pad_read_stub','game_pad_read_ext_stub'])+r'''
static unsigned native_calls;
static int32_t native_read(int32_t h,void *out,int32_t n,int32_t mode){
 (void)h;(void)mode;native_calls++;
 if(!out || n<1 || n>64)return -17;
 ScePadData native={.connected=0,.buttons=0x8000};memcpy(out,&native,sizeof native);return 1;
}
static int32_t native_ext(int32_t h,void *out,int32_t n){return native_read(h,out,n,1);}
static void setup(GamePadBridgeArgs *a,unsigned slot,unsigned legacy){
 memset(a,0,sizeof *a);a->magic=POORDS4_GAME_BRIDGE_MAGIC;a->pad_size=sizeof(ScePadData);
 a->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;a->pad_handle=0x33000300+(int)slot;a->reserved0=slot;
 for(unsigned i=0;i<4;i++)a->slots[i].pad_handle=-1;
 a->slots[slot].pad_handle=a->pad_handle;a->slots[slot].pad_index=slot;
 a->slots[slot].active=1;a->slots[slot].direct_packets=legacy?0:1;
 a->slots[slot].direct_seq=7;a->slots[slot].direct_lease=1000;
 a->direct_active=legacy;a->direct_seq=7;a->direct_lease=1000;
 ScePadData pad={.connected=1,.count=3,.buttons=0x4000,.timestamp=91};
 memcpy(a->slots[slot].direct_pad_data[1],&pad,sizeof pad);memcpy(a->direct_pad_data[1],&pad,sizeof pad);
 a->fp_read_internal=(intptr_t)native_read;a->read_ext_address=(intptr_t)native_ext;
}
int main(void){
 unsigned capacity_failures=0,reset_failures=0,guards=0,cases=0;
 const int capacities[]={INT_MIN,-1,0,1,2,64,65,INT_MAX};
 for(unsigned api=0;api<2;api++)for(unsigned slot=0;slot<4;slot++)for(unsigned legacy=0;legacy<(slot?1:2);legacy++)
 for(unsigned c=0;c<8;c++)for(unsigned null_out=0;null_out<2;null_out++){
  GamePadBridgeArgs a;setup(&a,slot,legacy);ScePadData out[2];memset(out,0x6c,sizeof out);
  ScePadData before[2];memcpy(before,out,sizeof out);unsigned calls=native_calls;
  int n=capacities[c];int result=api?game_pad_read_ext_stub(a.pad_handle,null_out?NULL:out,n,&a):
   game_pad_read_stub(a.pad_handle,null_out?NULL:out,n,&a);cases++;
  if(null_out || n<1 || n>64){
   if(result!=-17 || memcmp(out,before,sizeof out))capacity_failures++;
   if(a.slots[slot].last_read_seq || a.read_calls || a.read_ext_calls)guards++;
  }else if(result!=1 || out[0].connected!=1 || out[0].buttons!=0x4000 ||
            memcmp(&out[1],&before[1],sizeof out[1]))guards++;
  if(native_calls!=calls+1)guards++;
 }
 GamePadBridgeArgs a;setup(&a,1,0);a.pad_handle=0x33000304;a.reserved0=4;
 a.slots[1].pad_handle=a.pad_handle;a.slots[1].pad_index=4;
 a.slots[0].pad_handle=0x33000300;a.slots[0].is_dualsense=1;
 ScePadData held={.connected=1,.buttons=0xc00,.analogButtons={255,255}},neutral={.connected=1};
 for(unsigned i=0;i<44;i++){
  game_bridge_check_reset_shortcut(&a,a.pad_handle,&held);
  game_bridge_check_reset_shortcut(&a,a.slots[0].pad_handle,&neutral);
 }
 if(a.reset_ticks[1]!=44 || a.reset_requested)reset_failures++;
 game_bridge_check_reset_shortcut(&a,0x41000305,&held);
 if(a.reset_ticks[1]!=44 || a.reset_requested)reset_failures++;
 game_bridge_check_reset_shortcut(&a,a.pad_handle,&held);
 if(!a.reset_requested)reset_failures++;
 a.reset_requested=0;a.reset_ticks[0]=44;
 game_bridge_check_reset_shortcut(&a,a.slots[0].pad_handle,&held);
 if(!a.reset_requested)reset_failures++;
 a.reset_requested=0;held.connected=0;
 game_bridge_check_reset_shortcut(&a,a.pad_handle,&held);
 if(a.reset_requested)reset_failures++;
 a.magic=0;held.connected=1;
 game_bridge_check_reset_shortcut(&a,a.pad_handle,&held);
 if(a.reset_requested)reset_failures++;
 printf("capacity_cases=%u capacity_failures=%u reset_failures=%u guard_failures=%u native_calls=%u\n",
  cases,capacity_failures,reset_failures,guards,native_calls);
 return capacity_failures || reset_failures || guards;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-api-guards-')as directory:
 td=Path(directory);src=td/'guards.c';exe=td/'guards.exe';src.write_text(program,encoding='utf-8')
 cc=os.environ.get('POORDS4_HOST_CC','gcc')
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(src),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
 result={'compiled':not build.returncode,'passed':bool(run and not run.returncode),
  'output':run.stdout.strip()if run else'','error':run.stderr if run else build.stderr[-4000:],
  'source_sha256':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest(),
  'console_contacted':False,'physical_reset_verified':False}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
