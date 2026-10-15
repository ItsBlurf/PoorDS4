# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the optional device-class observer preserves native query behavior."""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from fixture_support import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--payload",type=Path,required=True)
payload=parser.parse_args().payload
source=(payload/"wireless_ds4.c").read_text(encoding="utf-8-sig")
types=source[source.index("#define POORDS4_GAME_BRIDGE_MAGIC"):source.index("static pid_t g_game_bridge_direct_pid")]
start=source.rfind("static __attribute__",0,source.index("\ngame_bridge_find_slot("))
end=source.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',start)
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+source[start:end]+r'''
static unsigned native_calls;
static int32_t native_result;
static int32_t native_query(int32_t handle,void *out) {
    (void)handle;native_calls++;
    if(!native_result && out) {
        unsigned char *bytes=out;
        for(unsigned i=0;i<20;i++)bytes[i]=(unsigned char)(0x31+i);
    }
    return native_result;
}
int32_t '''+function(source,"game_pad_device_class_info_stub")+r'''
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"line=%d condition=%s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    unsigned checks=0;
    GamePadBridgeTriggerArgs t;memset(&t,0,sizeof t);
    t.input.magic=POORDS4_GAME_BRIDGE_MAGIC;t.input.reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
    t.device_class_diag_magic=POORDS4_DEVICE_CLASS_DIAG_MAGIC;
    t.fp_device_class_info=(intptr_t)native_query;
    t.input.slots[0].pad_handle=0x33000300;t.input.slots[0].active=1;
    unsigned char buffer[36],expected[36];
    const int32_t handles[]={0x33000300,0x33000400,0x33000303};
    for(unsigned route=0;route<3;route++)for(unsigned error=0;error<2;error++) {
        memset(buffer,0xa5,sizeof buffer);memcpy(expected,buffer,sizeof buffer);
        native_result=error?-91:0;
        if(!error)for(unsigned i=0;i<20;i++)expected[8+i]=(unsigned char)(0x31+i);
        unsigned before=native_calls;
        CHECK(game_pad_device_class_info_stub(handles[route],buffer+8,&t.input)==native_result);
        CHECK(native_calls==before+1);CHECK(!memcmp(buffer,expected,sizeof buffer));
    }
    t.input.slots[0].is_dualsense=1;
    unsigned before=native_calls;native_result=0;
    CHECK(game_pad_device_class_info_stub(0x33000300,buffer+8,&t.input)==0);
    CHECK(native_calls==before+1);
    native_result=-3;before=native_calls;
    CHECK(game_pad_device_class_info_stub(0x33000300,NULL,&t.input)==-3);
    CHECK(native_calls==before+1);
    CHECK(t.device_class_calls==8);CHECK(t.device_class_errors==4);
    CHECK(t.input.slots[0].active==1);CHECK(t.input.active==0);
    CHECK(t.input.direct_lease==0);CHECK(t.input.slots[0].direct_lease==0);
    printf("device_class_observer_controls=%u native_calls=%u no_overrides=1\n",checks,native_calls);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="poords4-device-info-controls-") as temporary:
    temp=Path(temporary);c=temp/"test.c";exe=temp/"test.exe";c.write_text(program,encoding="utf-8")
    cc=os.environ.get("POORDS4_HOST_CC","gcc")
    build=subprocess.run([cc,"-std=c11","-O2","-Wall","-Wextra","-I",str(payload),str(c),"-o",str(exe)],capture_output=True,text=True)
    run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
    result={"compiled":not build.returncode,"passed":bool(run and not run.returncode),
            "output":run.stdout.strip()if run else"","error":run.stderr if run else build.stderr[-4000:],
            "source_sha256":hashlib.sha256((payload/"wireless_ds4.c").read_bytes()).hexdigest(),
            "console_contacted":False,"native_abi_verified":False}
    print(json.dumps(result,indent=2));sys.exit(0 if result["passed"]else 1)
