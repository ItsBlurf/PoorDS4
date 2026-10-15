# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the production query with a pinned PS5 emulator effect-state contract.

Sony calls and valid live input are explicit host models. A backend without
adaptive actuators accepts effect requests but reports zero physical state.
This demonstrates an API compatibility gap, not an Astro Bot hardware test.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--payload',type=Path,default=ROOT/'payload')
p.add_argument('--reference',type=Path,required=True)
opts=p.parse_args();payload=opts.payload;source=(payload/'wireless_ds4.c').read_text(encoding='utf-8-sig');reference=opts.reference.read_text(encoding='utf-8-sig')
types=source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
start=source.rfind('static __attribute__',0,source.index('\ngame_bridge_find_slot('));end=source.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',start)
helpers=source[start:end]
setter='game_pad_set_trigger_effect_stub' in source
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+helpers+r'''
static int32_t model_native_get(int32_t handle,void *out){(void)handle;memset(out,0,8);return 0;}
static int32_t model_native_set(int32_t handle,const void *param){(void)handle;(void)param;return 0;}
int32_t reference_state(uint32_t mode,uint8_t first,uint8_t second,uint8_t third,int value);
int32_t reference_state_data(uint32_t mode,const uint8_t *data,int value);
int32_t '''+function(source,'game_pad_get_trigger_effect_state_stub')+'\n'
if setter:program+='int32_t '+function(source,'game_pad_set_trigger_effect_stub')+'\n'
program+=r'''
typedef struct {uint32_t mode;uint8_t reserve[4],data[48];} TestCommand;
typedef struct {uint8_t mask,reserve[7];TestCommand commands[2];} TestParam;
_Static_assert(sizeof(TestParam)==120,"PS5 command ABI");
static unsigned mismatches,cases,guards;
int main(void){
 GamePadBridgeTriggerArgs t={0};GamePadBridgeArgs *a=&t.input;
 a->magic=POORDS4_GAME_BRIDGE_MAGIC;a->active=1;a->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
 a->pad_handle=0x3400;a->direct_active=1;a->direct_lease=100;
 for(unsigned i=0;i<4;i++){a->slots[i].pad_handle=-1;a->slots[i].pad_index=-1;}
 a->slots[0].pad_handle=0x3400;a->slots[0].pad_index=0;a->slots[0].active=1;a->slots[0].direct_lease=100;
 t.magic=POORDS4_TRIGGER_ARGS_MAGIC;t.fp_get_trigger_effect_state=(intptr_t)model_native_get;
 TestParam param={0};param.mask=3;param.commands[0].mode=1;param.commands[0].data[0]=4;param.commands[0].data[1]=3;
 param.commands[1].mode=2;param.commands[1].data[0]=2;param.commands[1].data[1]=7;param.commands[1].data[2]=3;
 SETTER_SETUP
 for(unsigned alias=0;alias<2;alias++)for(unsigned i=0;i<3;i++){
  const uint8_t values[3]={0,80,255};ScePadData pad={0};pad.connected=1;pad.count=1;
  pad.analogButtons.l2=pad.analogButtons.r2=values[i];
  memcpy(a->direct_pad_data[0],&pad,sizeof pad);memcpy(a->direct_pad_data[1],&pad,sizeof pad);
  memcpy(a->slots[0].direct_pad_data[0],&pad,sizeof pad);memcpy(a->slots[0].direct_pad_data[1],&pad,sizeof pad);
  struct {uint64_t before;int32_t state[2];uint64_t after;} out={UINT64_C(0xaabbccddee112233),{-1,-1},UINT64_C(0x112233aabbccddee)};
  int h=alias?0x12343400:0x3400;int result=game_pad_get_trigger_effect_state_stub(h,out.state,a);
  cases++;
  for(unsigned side=0;side<2;side++){
   TestCommand *cmd=&param.commands[side];int want=reference_state(cmd->mode,cmd->data[0],cmd->data[1],cmd->data[2],values[i]);
   if(result || out.state[side]!=want){mismatches++;fprintf(stderr,"alias=%u pressure=%u side=%u state=%d expected=%d result=%d\n",alias,values[i],side,out.state[side],want,result);}
  }
  if(out.before!=UINT64_C(0xaabbccddee112233)||out.after!=UINT64_C(0x112233aabbccddee))guards++;
 }
 PRESSURE_SWEEP
 printf("cases=%u state_mismatches=%u guard_failures=%u\n",cases,mismatches,guards);return mismatches||guards?1:0;
}
'''
if setter:
 program=program.replace('SETTER_SETUP','t.effect_magic=POORDS4_EFFECT_ARGS_MAGIC;t.fp_set_trigger_effect=(intptr_t)model_native_set;game_pad_set_trigger_effect_stub(0x3400,&param,a);')
 program=program.replace('PRESSURE_SWEEP',r'''
 for(unsigned mode=0;mode<=6;mode++){
  for(unsigned side=0;side<2;side++){param.commands[side].mode=mode;for(unsigned b=0;b<48;b++)param.commands[side].data[b]=(uint8_t)((b*3+1)%9);}
  a->direct_lease=a->slots[0].direct_lease=100000;
  if(game_pad_set_trigger_effect_stub(0x3400,&param,a)){fprintf(stderr,"mode=%u setter failed\n",mode);return 1;}
  for(unsigned value=0;value<256;value++){
   ScePadData pad={0};pad.connected=1;pad.count=1;pad.analogButtons.l2=(uint8_t)value;pad.analogButtons.r2=(uint8_t)(255-value);
   for(unsigned b=0;b<2;b++){memcpy(a->direct_pad_data[b],&pad,sizeof pad);memcpy(a->slots[0].direct_pad_data[b],&pad,sizeof pad);}
   int32_t state[2];int result=game_pad_get_trigger_effect_state_stub(0x3400,state,a);cases++;
   for(unsigned side=0;side<2;side++){
    int want=reference_state_data(mode,param.commands[side].data,side?255-(int)value:(int)value);
    if(result || state[side]!=want){mismatches++;fprintf(stderr,"mode=%u value=%u side=%u state=%d expected=%d\n",mode,value,side,state[side],want);}
   }
  }
 }
''')
else:
 program=program.replace('SETTER_SETUP','/* No setter import/gateway in this baseline; the game calls Sony directly. */model_native_set(0x3400,&param);')
 program=program.replace('PRESSURE_SWEEP','')
enum_start=reference.index('enum TriggerState : int32_t {');enum_end=reference.index('\n};',enum_start)+len('\n};')
reference_program=r'''
#include <algorithm>
#include <cstdint>
struct PadTriggerEffectCommand {uint32_t mode;uint8_t reserve[4],data[48];};
'''+reference[enum_start:enum_end]+'\nconstexpr int TRIGGER_PRESSED_MIN=8;\nint32_t '+function(reference.replace('int32_t TriggerEffectState(', 'int32_t\nTriggerEffectState('),'TriggerEffectState')+r'''
extern "C" int32_t reference_state(uint32_t mode,uint8_t first,uint8_t second,uint8_t third,int value){
 PadTriggerEffectCommand c={};c.mode=mode;c.data[0]=first;c.data[1]=second;c.data[2]=third;return TriggerEffectState(c,value);
}
extern "C" int32_t reference_state_data(uint32_t mode,const uint8_t *data,int value){
 PadTriggerEffectCommand c={};c.mode=mode;for(unsigned b=0;b<48;b++)c.data[b]=data[b];return TriggerEffectState(c,value);
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-effect-state-')as td:
 td=Path(td);c=td/'product.c';cpp=td/'reference.cpp';obj=td/'product.o';exe=td/'compare.exe';c.write_text(program);cpp.write_text(reference_program)
 cc=os.environ.get('POORDS4_HOST_CC','gcc');cxx=os.environ.get('POORDS4_HOST_CXX',str(Path(cc).with_name('g++.exe'))if os.name=='nt'else'g++')
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),'-c',str(c),'-o',str(obj)],capture_output=True,text=True)
 link=subprocess.run([cxx,'-std=c++17','-O2',str(cpp),str(obj),'-o',str(exe)],capture_output=True,text=True)if not build.returncode else None
 run=subprocess.run([str(exe)],capture_output=True,text=True)if link and not link.returncode else None
 result={'compiled':bool(link and not link.returncode),'passed':bool(run and not run.returncode),'output':run.stdout.strip()if run else '', 'error':run.stderr if run else (build.stderr if build.returncode else link.stderr[-4000:]),'source_sha256':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest(),'reference_sha256':hashlib.sha256(opts.reference.read_bytes()).hexdigest(),'setter_gateway_present':setter,'native_controller_test':False,'scope':'Actual product query with zero-state Sony backend; independently compiled pinned PS5 effect-state reference; requested feedback/weapon states and real-shaped pressure values, canonical and same-slot alias.'}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
