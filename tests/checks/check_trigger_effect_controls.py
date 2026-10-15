# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual effect callbacks: ownership, masked updates and bounded state.

Native endpoints, live packet buffers and caller pointers are explicit host
models. No console input, output command or physical controller is simulated.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--payload',type=Path,default=ROOT/'payload');payload=p.parse_args().payload
s=(payload/'wireless_ds4.c').read_text(encoding='utf-8-sig');types=s[s.index('#define POORDS4_GAME_BRIDGE_MAGIC'):s.index('static pid_t g_game_bridge_direct_pid')]
a=s.rfind('static __attribute__',0,s.index('\ngame_bridge_find_slot('));b=s.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',a)
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+s[a:b]+r'''
static int native_get_result,native_set_result=-9,native_get_calls,native_set_calls;
static int32_t native_get(int32_t h,void *out){(void)h;native_get_calls++;if(!native_get_result && (uintptr_t)out>=0x10000)memset(out,0x42,8);return native_get_result;}
static int32_t native_set(int32_t h,const void *in){(void)h;(void)in;native_set_calls++;return native_set_result;}
int32_t '''+function(s,'game_pad_get_trigger_effect_state_stub')+'\nint32_t '+function(s,'game_pad_set_trigger_effect_stub')+r'''
static unsigned checks;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static GamePadBridgeTriggerArgs t;
static void packet(unsigned slot,uint8_t l2,uint8_t r2,uint32_t buttons){
 ScePadData p={0};p.connected=1;p.count=1;p.analogButtons.l2=l2;p.analogButtons.r2=r2;p.buttons=buttons;
 t.input.slots[slot].active=1;t.input.slots[slot].direct_lease=100000;t.input.slots[slot].direct_packets=1;
 for(unsigned b=0;b<2;b++)memcpy(t.input.slots[slot].direct_pad_data[b],&p,sizeof p);
 t.input.slots[slot].direct_seq=2;
}
static int query(int h,int32_t left,int32_t right){
 struct {uint64_t before;int32_t state[2];uint64_t after;} out={17,{-1,-1},19};
 int result=game_pad_get_trigger_effect_state_stub(h,out.state,&t.input);
 CHECK(result==0 && out.state[0]==left && out.state[1]==right && out.before==17 && out.after==19);return 0;
}
int main(void){
 t.input.magic=POORDS4_GAME_BRIDGE_MAGIC;t.input.active=1;t.input.reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;t.input.pad_handle=0x3400;t.input.direct_active=1;t.input.direct_lease=100000;
 t.magic=POORDS4_TRIGGER_ARGS_MAGIC;t.effect_magic=POORDS4_EFFECT_ARGS_MAGIC;t.fp_get_trigger_effect_state=(intptr_t)native_get;t.fp_set_trigger_effect=(intptr_t)native_set;
#ifdef POORDS4_EFFECT_DIAG_MAGIC
 t.effect_diag_magic=POORDS4_EFFECT_DIAG_MAGIC;
#endif
 for(unsigned slot=0;slot<4;slot++){t.input.slots[slot].pad_handle=0x3400+(int)slot;t.input.slots[slot].pad_index=(int)slot;packet(slot,255,255,0);}
 /* No request: successful native bytes stay native. */
 CHECK(!query(0x3400,0x42424242,0x42424242));
 PoorDS4TriggerParam p={0};p.mask=3;p.commands[0].mode=p.commands[1].mode=1;p.commands[0].data[0]=p.commands[1].data[0]=4;p.commands[0].data[1]=p.commands[1].data[1]=3;
 for(unsigned slot=0;slot<4;slot++)CHECK(game_pad_set_trigger_effect_stub(0x3400+(int)slot,&p,&t.input)==0);
 CHECK(native_set_calls==4 && t.effect_set_accepts==4 && t.effect_native_result==-9);
 for(unsigned slot=0;slot<4;slot++){packet(slot,(slot&1)?0:255,(slot&2)?0:255,0);CHECK(!query(0x12343400+(int)slot,(slot&1)?1:2,(slot&2)?1:2));}
 /* Left-only off leaves the right command intact. No input bytes change. */
 uint8_t before[120];memcpy(before,t.input.slots[0].direct_pad_data[0],120);
 p.mask=1;p.commands[0].mode=0;CHECK(game_pad_set_trigger_effect_stub(0x12343400,&p,&t.input)==0);CHECK(!query(0x3400,0,2));CHECK(!memcmp(before,t.input.slots[0].direct_pad_data[0],120));
 /* Digital-only full press and genuine analog intermediate travel. */
 p.mask=3;p.commands[0].mode=p.commands[1].mode=2;p.commands[0].data[0]=p.commands[1].data[0]=2;p.commands[0].data[1]=p.commands[1].data[1]=7;
 CHECK(game_pad_set_trigger_effect_stub(0x3400,&p,&t.input)==0);packet(0,0,0,0x300);CHECK(!query(0x3400,5,5));packet(0,80,80,0x300);CHECK(!query(0x3400,4,4));
 uint64_t accepted=t.effect_set_accepts;uint32_t sequence=t.effects[0].sequence;
 p.mask=8;CHECK(game_pad_set_trigger_effect_stub(0x3400,&p,&t.input)==-9);p.mask=1;p.commands[0].mode=99;CHECK(game_pad_set_trigger_effect_stub(0x3400,&p,&t.input)==-9);
 p.commands[0].mode=2;p.commands[0].data[0]=p.commands[0].data[1]=0;CHECK(game_pad_set_trigger_effect_stub(0x3400,&p,&t.input)==-9);
 CHECK(t.effect_set_accepts==accepted && t.effects[0].sequence==sequence);
 p.commands[0].mode=1;p.commands[0].data[0]=4;p.commands[0].data[1]=3;
 CHECK(game_pad_set_trigger_effect_stub(0x3400,0,&t.input)==-9);CHECK(game_pad_set_trigger_effect_stub(0x3400,(void*)8,&t.input)==-9);
 /* Unmatched, native DS5, simulated, inactive and expired remain native. */
 CHECK(game_pad_set_trigger_effect_stub(0x3408,&p,&t.input)==-9);
 for(unsigned mode=0;mode<5;mode++){
  t.input.slots[0].is_dualsense=mode==0;t.input.slots[0].is_simulated=mode==1;t.input.active=mode!=2;
  t.input.slots[0].active=mode!=2 && mode!=3;t.input.slots[0].direct_lease=mode==4?0:100;
  t.input.direct_active=0;t.input.direct_lease=0;
  CHECK(game_pad_set_trigger_effect_stub(0x3400,&p,&t.input)==-9);CHECK(!query(0x3400,0x42424242,0x42424242));
 }
 CHECK(t.effect_set_accepts==accepted+3); /* Desired requests retained; native returns stay native. */
 CHECK(t.effect_cached_unavailable==3 && t.effect_compat_successes==accepted);
 accepted=t.effect_set_accepts;
 t.input.active=1;t.input.slots[0].is_dualsense=t.input.slots[0].is_simulated=0;packet(0,255,255,0);
 t.effects[0].lock=1;CHECK(game_pad_set_trigger_effect_stub(0x3400,&p,&t.input)==-1);t.effects[0].lock=0;
 CHECK(t.effect_set_accepts==accepted);
 t.effects[0].sequence|=1;CHECK(!query(0x3400,0x42424242,0x42424242));t.effects[0].sequence++;
#ifdef POORDS4_EFFECT_DIAG_MAGIC
 uint64_t sum=0;for(unsigned reason=0;reason<9;reason++)sum+=t.effect_skip_counts[reason];
 CHECK(sum==t.effect_set_calls && sum==(uint64_t)native_set_calls);
 const uint64_t expected[9]={6,0,1,1,1,2,3,3,1};
 for(unsigned reason=0;reason<9;reason++)CHECK(t.effect_skip_counts[reason]==expected[reason]);
 CHECK(t.effect_last_skip==8 && t.effect_last_slot==0 && t.effect_last_call_handle==0x3400);
 CHECK(t.effect_last_mask==1 && t.effect_last_left_mode==1);
#endif
 printf("checks=%u four-player-isolation=1 native-routes=1 masks=1 bounded-contention=1\n",checks);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-effect-controls-')as td:
 td=Path(td);c=td/'test.c';exe=td/'test.exe';c.write_text(program)
 cc=os.environ.get('POORDS4_HOST_CC','gcc')
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
 result={'compiled':not build.returncode,'passed':bool(run and not run.returncode),'output':run.stdout.strip()if run else '', 'error':run.stderr if run else build.stderr[-4000:],'sources':{name:hashlib.sha256((payload/name).read_bytes()).hexdigest()for name in ['wireless_ds4.c','trigger_effect_state.h']},'physical_four_player_acceptance':False,'console_contacted':False}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
