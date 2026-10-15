# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real copied read callbacks with host packets; never sends console input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from fixture_support import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source_root', type=Path)
args = p.parse_args()
payload = args.source_root.resolve() / 'payload'
source = (payload / 'wireless_ds4.c').read_text(encoding='utf-8')
cc = os.environ.get('POORDS4_HOST_CC', 'gcc')
types = source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
start = source.rfind('static __attribute__', 0, source.index('\ngame_bridge_find_slot('))
end = source.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))', start)
helpers = source[start:end]
names = ('game_pad_read_state_stub', 'game_pad_read_state_ext_stub',
         'game_pad_read_stub', 'game_pad_read_ext_stub', 'game_pad_get_data_internal_stub')
callbacks = '\n'.join('int32_t\n' + function(source, name) for name in names)
program = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
''' + types + helpers + callbacks + r'''
static ScePadData native_pad;
static int32_t native_state(int32_t h,void *out,int32_t flag) {
    (void)h;(void)flag;memcpy(out,&native_pad,sizeof native_pad);return 0;
}
static int32_t native_state_ext(int32_t h,void *out) {return native_state(h,out,1);}
static int32_t native_read(int32_t h,void *out,int32_t num,int32_t flag) {
    if(num<=0)return -1;
    native_state(h,out,flag);return 1;
}
static int32_t native_read_ext(int32_t h,void *out,int32_t num) {return native_read(h,out,num,1);}
static int call(unsigned api,int32_t handle,void *out,GamePadBridgeArgs *a) {
    switch(api) {
    case 0:return game_pad_read_state_stub(handle,out,a);
    case 1:return game_pad_read_state_ext_stub(handle,out,a);
    case 2:return game_pad_read_stub(handle,out,1,a);
    case 3:return game_pad_read_ext_stub(handle,out,1,a);
    default:return game_pad_get_data_internal_stub(handle,out,a);
    }
}
static void setup(GamePadBridgeArgs *a) {
    memset(a,0,sizeof *a);a->magic=POORDS4_GAME_BRIDGE_MAGIC;
    a->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;a->pad_size=sizeof(ScePadData);
    a->pad_handle=0x33000300;a->reserved0=0;
    a->slots[0].pad_handle=a->pad_handle;a->slots[0].pad_index=0;
    a->slots[0].active=1;a->slots[0].direct_packets=1;
    a->fp_state_internal=(intptr_t)native_state;a->fp_data_internal=(intptr_t)native_state;
    a->fp_read_internal=(intptr_t)native_read;
    a->read_state_ext_address=(intptr_t)native_state_ext;
    a->read_ext_address=(intptr_t)native_read_ext;
}
int main(void) {
    GamePadBridgeArgs a;
    struct {uint8_t before[16];ScePadData pad;uint8_t after[16];} out;
    ScePadData input={0};input.connected=1;input.count=1;
    input.leftStick.x=20;input.leftStick.y=40;input.rightStick.x=128;input.rightStick.y=128;
    input.timestamp=123456789;input.extension[7]=0x5a;
    native_pad=input;native_pad.analogButtons.l2=37;native_pad.analogButtons.r2=91;
    native_pad.buttons=0x200;
    for(unsigned api=0;api<5;api++) {
        setup(&a);
        for(unsigned l2=0;l2<256;l2++)for(unsigned r2=0;r2<256;r2++) {
            input.analogButtons.l2=(uint8_t)l2;input.analogButtons.r2=(uint8_t)r2;
            input.buttons=(l2>=128?0x100:0)|(r2>=128?0x200:0)|0x4000;
            memcpy(a.slots[0].direct_pad_data[0],&input,sizeof input);
            memcpy(a.slots[0].direct_pad_data[1],&input,sizeof input);
            a.slots[0].direct_seq++;
            a.slots[0].direct_lease=2048;
            memset(&out,0xa5,sizeof out);
            int rc=call(api,a.pad_handle,&out.pad,&a);
            int expected_rc=(api==2 || api==3)?1:0;
            if(rc!=expected_rc || memcmp(&out.pad,&input,sizeof input)) {
                fprintf(stderr,"packet mismatch api=%u l2=%u r2=%u result=%d\n",api,l2,r2,rc);return 1;
            }
            for(unsigned i=0;i<16;i++)if(out.before[i]!=0xa5 || out.after[i]!=0xa5)return 2;
            if((api==2 || api==3) && call(api,a.pad_handle,&out.pad,&a)!=0)return 5;
        }
        /* Native DualSense and unmatched handles retain native trigger data. */
        a.slots[0].is_dualsense=1;a.slots[0].direct_lease=2048;
        if(call(api,a.pad_handle,&out.pad,&a)!=((api==2 || api==3)?1:0) ||
           memcmp(&out.pad,&native_pad,sizeof native_pad))return 3;
        a.slots[0].is_dualsense=0;
        if(call(api,0x33000304,&out.pad,&a)!=((api==2 || api==3)?1:0) ||
           memcmp(&out.pad,&native_pad,sizeof native_pad))return 4;
    }
    puts("pressure_pairs=65536 callbacks=5 packet_checks=327680 no_new_frame_checks=131072 native_controls=10");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-triggers-') as temporary:
    c_file, exe = Path(temporary) / 'trigger.c', Path(temporary) / 'trigger.exe'
    c_file.write_text(program, encoding='utf-8')
    build = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-I', str(payload),
                            str(c_file), '-o', str(exe)], capture_output=True, text=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True) if not build.returncode else None
    passed = run is not None and run.returncode == 0
    print(json.dumps({'source_root': str(args.source_root),
                      'sources': {name: hashlib.sha256((payload / name).read_bytes()).hexdigest()
                                  for name in ('wireless_ds4.c', 'wireless_ds4.h', 'pad_types.h')},
                      'compiled': build.returncode == 0, 'passed': passed,
                      'exit': run.returncode if run else None,
                      'output': run.stdout.strip() if run else '',
                      'error': run.stderr if run else build.stderr[-2500:],
                      'console_contacted': False, 'physical_trigger_verified': False,
                      'astro_bot_symptom_fixed': False}, indent=2))
sys.exit(0 if passed else 1)
