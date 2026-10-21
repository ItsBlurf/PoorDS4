# SPDX-License-Identifier: GPL-3.0-or-later
"""Production table locator with synthetic Sony-shaped code and client records.

No proprietary instructions, account identifiers or game files are embedded.
Optional private captures can exercise the same locator separately.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from fixture_support import function

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload', type=Path, default=ROOT / 'payload')
parser.add_argument('--native-code', type=Path)
parser.add_argument('--native-data', type=Path)
options = parser.parse_args()
if bool(options.native_code) != bool(options.native_data):
    parser.error('Native code and data must be supplied together.')
source = (options.payload / 'wireless_ds4.c').read_text()
names = ['game_cache_validate_table_candidate', 'game_cache_find_table']
if '\ngame_cache_table_reference_stride(' in source:
    names.insert(0, 'game_cache_table_reference_stride')
constants = '\n'.join(re.findall(r'^#define POORDS4_PAD_CLIENT_.*$', source, re.M))
program = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#define __PROSPERO__ 1
#define BASE ((intptr_t)0x100000)
static unsigned char image[0x30000];
static int mdbg_copyout(int pid,intptr_t at,void *out,size_t n){
 (void)pid;if(at<BASE || (uintptr_t)(at-BASE)>sizeof image || n>sizeof image-(size_t)(at-BASE))return -1;
 memcpy(out,image+at-BASE,n);return 0;
}
#define game_bridge_process_read mdbg_copyout
static int report_printf(int fd,const char *fmt,...){(void)fd;(void)fmt;return 0;}
static void game_bridge_report_prefix(int fd,const char *name,const void *p,size_t n){(void)fd;(void)name;(void)p;(void)n;}
static uint64_t poords4_fnv1a64(const void *p,size_t n){(void)p;(void)n;return 0;}
static int game_cache_follow_read_state(int pid,intptr_t base,uint64_t n,intptr_t read,intptr_t *out){(void)pid;(void)n;(void)read;*out=base+0x1610;return 0;}
static void game_cache_report_user_hits(int pid,intptr_t base,uint64_t n,int user,int fd){(void)pid;(void)base;(void)n;(void)user;(void)fd;}
''' + constants + '\n' + '\n'.join('static int\n' + function(source, n) for n in names) + r'''
static void u32(size_t at,uint32_t value){memcpy(image+at,&value,4);}
static void lea(size_t at,unsigned reg,size_t target){
 image[at]=0x48;image[at+1]=0x8d;image[at+2]=(unsigned char)(5+(reg<<3));
 u32(at+3,(uint32_t)(target-at-7));
}
static void setup_delayed(uint32_t stride,unsigned delay,int extended){
 memset(image,0,sizeof image);
 /* A library synchronization prefix looks like one inactive client at -16. */
 u32(0x20024,0x200);u32(0x2002c,8);
 lea(0x1610,7,0x20028);
 lea(0x1630,1,0x20018);
 if(extended)image[0x1630]=0x4c; /* r9 instead of rcx */
 const unsigned char lookup[]={0x8b,0x54,0x08,0x1c,0x39,0xfa,0x48,0x05,0,0,0,0,0x48,0x3d,0,0,0,0,0x75,0xea};
 memset(image+0x1637,0x90,delay);
 size_t at=0x1637+delay;
 if(extended)image[at++]=0x42; /* index r9 in the SIB handle load */
 memcpy(image+at,lookup,sizeof lookup);
 u32(at+8,stride);u32(at+14,24*stride);
}
static void setup(uint32_t stride){setup_delayed(stride,0,0);}
static void client(unsigned slot,uint32_t stride,int user,int native){
 size_t at=0x20018+slot*stride;
 u32(at+0x18,native?1:0);u32(at+0x1c,0x03000700u+slot);u32(at+0x24,(uint32_t)user);
 if(native){image[at+0x60]=0x4c;image[at+0x61]=0x05;image[at+0x62]=0xe6;image[at+0x63]=0x0c;}
}
static unsigned cases,failed;
static void rejects_anchor(const char *name){
 cases++;
 if(game_cache_table_reference_stride(image+0x1610,0x800,0x20)!=0){
  failed++;printf("FAIL unexpected_anchor=%s\n",name);
 }
}
static void check(const char *name,int source,int expect_table,uint32_t expected_stride){
 intptr_t table=0;uint32_t stride=0;cases++;
 int rc=game_cache_find_table(7,BASE,sizeof image,BASE+0x2a80,source,0,-1,&table,&stride);
 int ok=expect_table?(rc==0 && table==BASE+0x20018 && stride==expected_stride):(rc!=0);
 if(!ok){failed++;printf("FAIL case=%s rc=%d selected_offset=0x%lx stride=0x%x\n",name,rc,(unsigned long)(table-BASE),stride);}
}
int main(int argc,char **argv){
 for(unsigned profile=0;profile<2;profile++){
  uint32_t stride=profile?0x548:0x5c8;
  setup(stride);check("before_clients_open",11,1,stride);
  setup(stride);image[0x1630]=0x4c;rejects_anchor("different_table_register");
  setup(stride);image[0x1637]=0x90;rejects_anchor("missing_handle_load");
  setup(stride);u32(0x1637+8,0x11111111);rejects_anchor("unsupported_stride");
  setup(stride);u32(0x1637+14,0);rejects_anchor("missing_span");
  setup_delayed(stride,94,0);rejects_anchor("outside_bounded_window");
  setup(stride);client(0,stride,11,0);u32(0x20018+0x18,2);
  check("invalid_connected_field",11,0,stride);
  setup(stride);memcpy(image+0x1700,image+0x1630,27);lea(0x1700,1,0x21018);
  check("ambiguous_anchored_tables",11,0,stride);
  /* Alignment and loop setup can place the first handle load farther from
   * the table LEA. Both an empty table and native-first order must work. */
  for(unsigned delay=24;delay<=49;delay++)for(int extended=0;extended<2;extended++){
   setup_delayed(stride,delay,extended);check("delayed_empty_lookup",11,1,stride);
   setup_delayed(stride,delay,extended);client(0,stride,22,1);client(8,stride,11,0);
   check("delayed_native_first_lookup",11,1,stride);
  }
  setup(stride);client(0,stride,22,1);check("native_before_ds4_open",11,1,stride);
  setup(stride);client(0,stride,22,1);client(1,stride,11,0);check("native_first_ds4_second",11,1,stride);
  setup(stride);client(0,stride,11,0);client(1,stride,22,1);check("ds4_first_native_second",11,1,stride);
  for(unsigned first=0;first<4;first++){
   setup(stride);for(unsigned i=0;i<4;i++)client(i,stride,11+11*(int)i,0);
   check("four_ds4_source_orders",11+11*(int)first,1,stride);
  }
  setup(stride);client(0,stride,22,1);client(8,stride,11,0);client(16,stride,33,0);client(23,stride,44,0);
  check("high_client_indexes",11,1,stride);
 }
 if(argc==3){
  memset(image,0,sizeof image);
  FILE *f=fopen(argv[1],"rb");if(!f)return 2;
  size_t bytes=fread(image+0x1610,1,0x800,f);fclose(f);if(bytes!=0x400)return 2;
  f=fopen(argv[2],"rb");if(!f)return 2;
  bytes=fread(image+0x20000,1,0x9000,f);fclose(f);if(bytes!=0x9000)return 2;
  int user;memcpy(&user,image+0x20018+0x24,4);
  check("native_code_and_two_controller_snapshot",user,1,0x5c8);
  /* Clear only client-routing fields; retain captured synchronization data.
   * This is a modeled startup using the captured layout, not a new native run. */
  for(unsigned i=0;i<24;i++){
   size_t at=0x20018+i*0x5c8;
   u32(at+0x18,0);u32(at+0x1c,0);u32(at+0x24,0);u32(at+0x38,0);u32(at+0x60,0);
  }
  check("captured_layout_before_clients_open",user,1,0x5c8);
 }
 printf("cases=%u failures=%u\n",cases,failed);return failed!=0;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-client-locator-') as temp:
    directory = Path(temp)
    (directory / 'test.c').write_text(program)
    compiled = subprocess.run([os.environ.get('POORDS4_HOST_CC', 'gcc'), '-std=c11', '-O2', str(directory / 'test.c'), '-o', str(directory / 'test.exe')], capture_output=True, text=True)
    captures = [str(options.native_code), str(options.native_data)] if options.native_code else []
    result = subprocess.run([str(directory / 'test.exe'), *captures], capture_output=True, text=True) if compiled.returncode == 0 else compiled
    report = {'compiled': compiled.returncode == 0, 'passed': compiled.returncode == 0 and result.returncode == 0, 'output': result.stdout, 'error': result.stderr}
    print(json.dumps(report, indent=2))
    raise SystemExit(not report['passed'])
