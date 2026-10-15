# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay LEGO's connected-buffer check against the actual queued callbacks.

Native packets and Unity's always-update flag are host controls. No console
input is generated. Native query cadence must independently confirm the cause.
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from fixture_support import function
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--payload", type=Path, required=True)
payload = p.parse_args().payload
s = (payload / "wireless_ds4.c").read_text(encoding="utf-8-sig")
types = s[s.index("#define POORDS4_GAME_BRIDGE_MAGIC"):s.index("static pid_t g_game_bridge_direct_pid")]
a = s.rfind("static __attribute__", 0, s.index("\ngame_bridge_find_slot("))
b = s.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))', a)
program = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
''' + types + s[a:b] + "\n".join("int32_t\n" + function(s, name) for name in
    ("game_pad_read_stub", "game_pad_read_ext_stub")) + r'''
static unsigned native_calls;
static int native_result=1;
static ScePadData native_pad;
static int32_t native_read(int32_t h,void *out,int32_t num,int32_t mode) {
    (void)h;(void)mode;native_calls++;
    if(!out || num<=0)return -17;
    if(native_result>=0)memcpy(out,&native_pad,sizeof native_pad);
    return native_result;
}
static int32_t native_ext(int32_t h,void *out,int32_t num) {return native_read(h,out,num,1);}
static int call(unsigned api,int32_t h,ScePadData *out,GamePadBridgeArgs *args) {
    return api?game_pad_read_ext_stub(h,out,2,args):game_pad_read_stub(h,out,2,args);
}
static void setup(GamePadBridgeArgs *args,unsigned slot,int legacy,const ScePadData *pad) {
    memset(args,0,sizeof *args);args->magic=POORDS4_GAME_BRIDGE_MAGIC;
    args->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;args->pad_size=sizeof *pad;
    args->pad_handle=0x33000300+(int)slot;args->reserved0=slot;
    for(unsigned i=0;i<4;i++)args->slots[i].pad_handle=-1;
    args->slots[slot].pad_handle=args->pad_handle;args->slots[slot].pad_index=slot;
    args->fp_read_internal=(intptr_t)native_read;args->read_ext_address=(intptr_t)native_ext;
#ifdef POORDS4_EMPTY_READ_DIAG_MAGIC
    args->legacy_receiver_port=POORDS4_EMPTY_READ_DIAG_MAGIC;
#endif
    if(legacy) {
        args->direct_active=1;args->direct_seq=7;args->direct_lease=10000;
        memcpy(args->direct_pad_data[1],pad,sizeof *pad);
    } else {
        args->slots[slot].active=1;args->slots[slot].direct_seq=7;
        args->slots[slot].direct_packets=1;args->slots[slot].direct_lease=10000;
        memcpy(args->slots[slot].direct_pad_data[1],pad,sizeof *pad);
    }
}
/* Decompiled eboot: ((manager_flags & ~2) | result) gates sample[0].connected;
 * connected==0 clears metadata and removes an existing InputSystem device. */
static unsigned consumer_removes(int result,const ScePadData *sample,uint32_t flags) {
    return (((flags & ~2u) | (uint32_t)result)!=0) && !sample->connected;
}
int main(void) {
    unsigned failures=0,guards=0,duplicates=0,removals=0;
    native_pad=(ScePadData){.connected=0,.count=0,.timestamp=0};
    ScePadData live={.buttons=0x4000,.connected=1,.count=1,.timestamp=901,
        .leftStick={128,128},.rightStick={128,128},.analogButtons={51,79}};
    for(unsigned api=0;api<2;api++)for(unsigned route=0;route<5;route++) {
        unsigned slot=route%4;int legacy=route==4;
        for(unsigned native_mode=0;native_mode<3;native_mode++) {
            GamePadBridgeArgs args;setup(&args,slot,legacy,&live);
            native_result=native_mode==0?1:native_mode==1?0:-9;
            ScePadData out[2];memset(out,0x6c,sizeof out);ScePadData guard=out[1];
            unsigned calls_before=native_calls;
            int result=call(api,args.pad_handle,out,&args);
            if(result!=1 || memcmp(out,&live,sizeof live) || memcmp(&out[1],&guard,sizeof guard))guards++;
            uint64_t emitted=api?args.read_ext_calls:args.read_calls;
            for(unsigned repeat=0;repeat<64;repeat++) {
                result=call(api,args.pad_handle,out,&args);duplicates++;
                unsigned removed=consumer_removes(result,out,1);
                removals+=removed;
                if(result!=0 || removed || memcmp(out,&live,sizeof live))failures++;
                if(memcmp(&out[1],&guard,sizeof guard) ||
                   (api?args.read_ext_calls:args.read_calls)!=emitted)guards++;
            }
            if(native_calls!=calls_before+65)guards++;
#ifdef POORDS4_EMPTY_READ_DIAG_MAGIC
            if(args.legacy_receiver_packets!=64 ||
               args.legacy_receiver_timeouts!=(native_mode<2?64u:0u))guards++;
#endif
            /* Actual source loss must expose Sony's disconnected packet. */
            args.slots[slot].active=0;args.direct_active=0;native_result=1;
            if(call(api,args.pad_handle,out,&args)!=1 || !consumer_removes(1,out,1))guards++;
            /* A live flag without a published packet must not synthesize data. */
            setup(&args,slot,legacy,&live);
            args.direct_seq=0;args.slots[slot].direct_seq=0;
            native_pad.connected=0;
            if(call(api,args.pad_handle,out,&args)!=0 || out[0].connected)guards++;
            /* Expired source leases retain real native loss as well. */
            setup(&args,slot,legacy,&live);
            args.direct_lease=0;args.slots[slot].direct_lease=0;
            if(call(api,args.pad_handle,out,&args)!=1 || out[0].connected)guards++;
            /* Native DualSense and unrelated handles retain the native buffer. */
            setup(&args,slot,legacy,&live);args.slots[slot].is_dualsense=1;
            native_pad.connected=1;native_pad.buttons=0x8000;
            if(call(api,args.pad_handle,out,&args)!=1 || memcmp(out,&native_pad,sizeof native_pad))guards++;
            if(call(api,0x41000308,out,&args)!=1 || memcmp(out,&native_pad,sizeof native_pad))guards++;
            native_pad.connected=0;native_pad.buttons=0;
        }
    }
    printf("duplicate_checks=%u false_removals=%u duplicate_failures=%u guard_failures=%u native_calls=%u\n",
        duplicates,removals,failures,guards,native_calls);
    return failures || guards;
}
'''
with tempfile.TemporaryDirectory(prefix="poords4-lego-empty-") as temporary:
    td=Path(temporary);c=td/"check.c";exe=td/"check.exe";c.write_text(program,encoding="utf-8")
    cc=os.environ.get("POORDS4_HOST_CC","gcc")
    built=subprocess.run([cc,"-std=c11","-O2","-Wall","-Wextra","-I",str(payload),str(c),"-o",str(exe)],capture_output=True,text=True)
    run=subprocess.run([str(exe)],capture_output=True,text=True) if not built.returncode else None
    result={"compiled":not built.returncode,"passed":bool(run and not run.returncode),
        "output":run.stdout.strip() if run else "","error":run.stderr if run else built.stderr[-4000:],
        "source_sha256":hashlib.sha256((payload/"wireless_ds4.c").read_bytes()).hexdigest(),
        "console_contacted":False,"unity_flag_is_control":True,"physical_gameplay_verified":False}
    print(json.dumps(result,indent=2));sys.exit(0 if result["passed"] else 1)
