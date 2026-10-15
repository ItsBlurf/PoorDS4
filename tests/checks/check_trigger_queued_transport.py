# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce trigger rejection with the actual constructor's zero legacy flag.

Sony endpoints, assigned handles and packet publications are host models. This
is an actual callback/constructor-fragment test, not physical controller input.
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from fixture_support import function
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--payload', required=True, type=Path)
payload = p.parse_args().payload
s = (payload / 'wireless_ds4.c').read_text(encoding='utf-8-sig')
types = s[s.index('#define POORDS4_GAME_BRIDGE_MAGIC'):s.index('static pid_t g_game_bridge_direct_pid')]
a = s.rfind('static __attribute__', 0, s.index('\ngame_bridge_find_slot('))
b = s.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))', a)
end = s.index('    args.last_caller_handle = -1;')
begin = s.rfind('    GamePadBridgeArgs args;\n', 0, end)
if begin < 0:
    raise SystemExit('Constructor fragment not found')
constructor = s[begin:end]
program = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+s[a:b]+r'''
static int set_calls,get_calls;
static int32_t native_set(int32_t h,const void *p){(void)h;(void)p;set_calls++;return -9;}
static int32_t native_get(int32_t h,void *p){(void)h;get_calls++;memset(p,0x42,8);return 0;}
int32_t '''+function(s,'game_pad_set_trigger_effect_stub')+'\nint32_t '+function(s,'game_pad_get_trigger_effect_state_stub')+r'''
static GamePadBridgeTriggerArgs make_args(void){
 int32_t game_pad_handle=0x3400,game_pad_index=0;
'''+constructor+r'''
 GamePadBridgeTriggerArgs t={0};t.input=args;t.magic=POORDS4_TRIGGER_ARGS_MAGIC;
 t.effect_magic=POORDS4_EFFECT_ARGS_MAGIC;t.fp_set_trigger_effect=(intptr_t)native_set;
 t.fp_get_trigger_effect_state=(intptr_t)native_get;
 for(unsigned slot=0;slot<4;slot++){t.input.slots[slot].pad_handle=0x3400+(int)slot;t.input.slots[slot].pad_index=(int)slot;}
 return t;
}
static void publish(GamePadBridgeTriggerArgs *t,unsigned slot){
 ScePadData pad={0};pad.connected=1;pad.count=1;pad.analogButtons.l2=255;pad.analogButtons.r2=255;
 t->input.slots[slot].active=1;t->input.slots[slot].direct_lease=100000;
 t->input.slots[slot].direct_packets=1;t->input.slots[slot].direct_seq=2;
 for(unsigned copy=0;copy<2;copy++)memcpy(t->input.slots[slot].direct_pad_data[copy],&pad,sizeof pad);
}
int main(void){
 unsigned startup_failures=0,queued_failures=0,guards=0;
 PoorDS4TriggerParam param={0};param.mask=3;
 for(unsigned side=0;side<2;side++){param.commands[side].mode=1;param.commands[side].data[0]=4;param.commands[side].data[1]=3;}
 for(unsigned slot=0;slot<4;slot++){
  GamePadBridgeTriggerArgs t=make_args();int32_t h=0x3400+(int)slot;
  if(t.input.active!=0)guards++;
  int set=game_pad_set_trigger_effect_stub(h,&param,&t.input);
  struct{uint64_t before;int32_t state[2];uint64_t after;} out={17,{-1,-1},19};
  int get=game_pad_get_trigger_effect_state_stub(h,out.state,&t.input);
  if(set!=-9 || get!=0 || out.state[0]!=0x42424242 || out.state[1]!=0x42424242)guards++;
  if(t.effects[slot].mask!=3)startup_failures++;
  publish(&t,slot);
  uint8_t original[120];memcpy(original,t.input.slots[slot].direct_pad_data[0],120);
  get=game_pad_get_trigger_effect_state_stub(h,out.state,&t.input);
  if(get!=0 || out.state[0]!=2 || out.state[1]!=2)startup_failures++;
  if(out.before!=17 || out.after!=19 || memcmp(original,t.input.slots[slot].direct_pad_data[0],120))guards++;
  /* A valid queued packet is sufficient even though legacy active stays zero. */
  t=make_args();publish(&t,slot);
  set=game_pad_set_trigger_effect_stub(h,&param,&t.input);
  get=game_pad_get_trigger_effect_state_stub(h,out.state,&t.input);
  if(set!=0 || get!=0 || out.state[0]!=2 || out.state[1]!=2)queued_failures++;
 }
 printf("startup_failures=%u queued_failures=%u guard_failures=%u native_set_calls=%d native_get_calls=%d\n",startup_failures,queued_failures,guards,set_calls,get_calls);
 return startup_failures || queued_failures || guards || set_calls!=8 || get_calls!=12;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-queued-trigger-') as temporary:
 td=Path(temporary);c=td/'check.c';exe=td/'check.exe';c.write_text(program,encoding='utf-8')
 cc=os.environ.get('POORDS4_HOST_CC','gcc')
 compile=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True) if not compile.returncode else None
 r={'compiled':not compile.returncode,'passed':bool(run and not run.returncode),
    'output':run.stdout.strip() if run else '', 'error':run.stderr if run else compile.stderr[-4000:],
    'source_sha256':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest(),
    'constructor_fragment':constructor,'physical_controller_input':False,'console_contacted':False}
 print(json.dumps(r,indent=2));sys.exit(0 if r['passed'] else 1)
