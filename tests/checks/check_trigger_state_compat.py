# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual trigger-state callback with Sony mocks and guarded caller memory."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys,tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('source_root',type=Path)
args=p.parse_args();payload=args.source_root.resolve()/'payload'
source=(payload/'wireless_ds4.c').read_text()
types=source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
start=source.rfind('static __attribute__',0,source.index('\ngame_bridge_find_slot('))
end=source.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',start)
helpers=source[start:end]
callback='int32_t\n'+function(source,'game_pad_get_trigger_effect_state_stub')
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+helpers+callback+r'''
static int native_result=-9,native_calls;
static int32_t native_state(int32_t handle,void *out){
    (void)handle;native_calls++;
    if(native_result==0 && (uintptr_t)out>=0x10000)memset(out,0x42,8);
    return native_result;
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
    GamePadBridgeTriggerArgs t={0};GamePadBridgeArgs *a=&t.input;
    a->magic=POORDS4_GAME_BRIDGE_MAGIC;a->active=1;a->pad_handle=0x3400;
    a->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
    a->direct_active=1;a->direct_lease=100;
    for(unsigned i=0;i<4;i++){a->slots[i].pad_handle=-1;a->slots[i].pad_index=-1;}
    a->slots[0].pad_handle=0x3400;a->slots[0].pad_index=0;
    a->slots[0].active=1;a->slots[0].direct_lease=100;
    t.magic=POORDS4_TRIGGER_ARGS_MAGIC;t.fp_get_trigger_effect_state=(intptr_t)native_state;
    uint8_t bytes[24];memset(bytes,0xEE,sizeof bytes);
    CHECK(game_pad_get_trigger_effect_state_stub(0x3400,bytes+8,a)==0);
    for(unsigned i=0;i<24;i++)CHECK(bytes[i]==(i>=8 && i<16?0:0xEE));
    CHECK(native_calls==1 && t.neutral_overrides==1);
    native_result=0;CHECK(game_pad_get_trigger_effect_state_stub(0x3400,bytes+8,a)==0);
    for(unsigned i=8;i<16;i++)CHECK(bytes[i]==0x42);
    CHECK(t.neutral_overrides==1);
    native_result=-9;
    CHECK(game_pad_get_trigger_effect_state_stub(0x3401,bytes+8,a)==-9);
    a->slots[0].is_dualsense=1;CHECK(game_pad_get_trigger_effect_state_stub(0x3400,bytes+8,a)==-9);
    a->slots[0].is_dualsense=0;a->slots[0].is_simulated=1;
    CHECK(game_pad_get_trigger_effect_state_stub(0x3400,bytes+8,a)==-9);
    a->slots[0].is_simulated=0;a->active=0;
    CHECK(game_pad_get_trigger_effect_state_stub(0x3400,bytes+8,a)==0);
    CHECK(t.neutral_overrides==2); /* Queued input is live despite legacy active=0. */
    a->active=1;a->direct_active=0;a->direct_lease=0;
    a->slots[0].active=0;a->slots[0].direct_lease=0;
    CHECK(game_pad_get_trigger_effect_state_stub(0x3400,bytes+8,a)==-9);
    a->direct_active=1;a->direct_lease=100;
    CHECK(game_pad_get_trigger_effect_state_stub(0x3400,0,a)==-9);
    CHECK(game_pad_get_trigger_effect_state_stub(0x3400,(void*)8,a)==-9);
    for(unsigned i=0;i<24;i++)CHECK(bytes[i]==(i>=8 && i<16?0:0xEE));
    CHECK(t.neutral_overrides==2 && native_calls==9);
    puts("DS4 neutral fallback writes exactly eight bytes; native success/DS5/unmatched/simulated/inactive/expired/invalid outputs preserved");
    return 0;
}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='trigger-state-') as tmp:
    c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
    build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
    run=subprocess.run([str(exe)],capture_output=True,text=True) if not build.returncode else None
    passed=run is not None and run.returncode==0
    print(json.dumps({'compiled':not build.returncode,'passed':passed,'exit':run.returncode if run else None,'output':run.stdout.strip() if run else '',
        'error':run.stderr if run else build.stderr[-2400:],'source_sha256':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest(),
        'native_gameplay_verified':False},indent=2))
sys.exit(0 if passed else 1)
