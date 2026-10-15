# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute actual reader shutdown/bootstrap blocks with explicit Sony/ptrace models.

No target execution, native controller input or hardware ownership claim.
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
channel=(payload/'reader_rumble_channel.h').read_text()
end=source.index('} RemotePadReaderArgs;')+len('} RemotePadReaderArgs;')
types=source[source.rfind('typedef struct {',0,end):end]
start=function(source,'wireless_ds4_remote_reader_start')
begin=start.index('    RemotePadReaderTransport transport;')
factory=start[begin:start.index('    /* Optional output setup',begin)]
cleanup=start[start.index('    if(!launched)',start.index('\ncleanup:')):start.index('    if (trap_saved',start.index('\ncleanup:'))]
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
#define POORDS4_REMOTE_PAD_CAPACITY 256
#define MSG_DONTWAIT 0x80
#define POORDS4_GAME_RESET_HOLD_READER_TICKS 180
'''+types+r'''
#include "reader_rumble_channel.h"
static unsigned calls[4],bad_handles,failed,checks;
static RemotePadReaderTransport *active;
static int watchdog;
static void close_record(int h){if(h<44 || h>47)bad_handles++;else calls[h-44]++;}
static int32_t native_close(int32_t h){close_record(h);return 0;}
static int32_t native_read(int32_t h,void *out){(void)h;memset(out,0,sizeof(ScePadData));((ScePadData*)out)->connected=1;return 0;}
static void native_sleep(unsigned us){(void)us;if(!watchdog)active->input.stop=1;}
static int32_t native_owner(int32_t pid,int32_t signal){(void)pid;(void)signal;return -1;}
static void model_close(int fd){(void)fd;bad_handles++;}
#define close model_close
static int g_reader_rumble_fd; static pid_t g_reader_rumble_pid; static intptr_t g_reader_rumble_args;
static uint64_t g_reader_rumble_sent[4];
static int64_t pt_call(pid_t pid,intptr_t fn,intptr_t trap,uint64_t h,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 (void)pid;(void)trap;(void)b;(void)c;(void)d;(void)e;(void)f;
 if(fn!=99)bad_handles++;else close_record((int)h);return 0;
}
void *'''+function(source,'remote_pad_reader_stub')+r'''
static RemotePadReaderTransport actual_factory(unsigned ownership){
 intptr_t fn_readstate=1,fn_read=2,fn_usleep=3,fn_kill=4,fn_closepad=99;
 int32_t handle=44;int selected_opened_here=(ownership&1u)!=0;int owner_pid=77;
 unsigned ds4_count=4;int32_t multi_handles[4]={44,45,46,47};
 int multi_opened[4];for(unsigned s=0;s<4;s++)multi_opened[s]=(ownership>>s)&1u;
'''+factory+r'''
 transport.input=args;return transport;
}
static void actual_failed_bootstrap(unsigned ownership,int launched,unsigned admitted){
 int output_remote=-1,output_local=-1;intptr_t output_close=0,trap_mem=55,args_addr=66;
 pid_t target=99;intptr_t probe_device_addr=0,probe_data_addr=0,remote_block=0,fn_free=0,fn_closepad=99;
 int selected_opened_here=(ownership&1u)!=0;int32_t handle=44;
 unsigned ds4_count=admitted;int32_t multi_handles[4]={44,45,46,47};
 int multi_opened[4];for(unsigned s=0;s<4;s++)multi_opened[s]=(ownership>>s)&1u;
'''+cleanup+r'''
}
static void expect(const char *phase,unsigned ownership){
 for(unsigned s=0;s<4;s++){checks++;unsigned expected=(ownership>>s)&1u;
  if(calls[s]!=expected){if(failed<8)fprintf(stderr,"%s ownership=0x%x slot=%u closes=%u expected=%u\n",phase,ownership,s,calls[s],expected);failed++;}}
 if(bad_handles){failed++;fprintf(stderr,"unexpected handle operation\n");}
}
static void reset_calls(void){memset(calls,0,sizeof calls);bad_handles=0;}
int main(void){
 for(unsigned ownership=0;ownership<16;ownership++){
  RemotePadReaderTransport t=actual_factory(ownership);
  if(t.input.close_pad_on_exit!=(int)(ownership&1u)){failed++;fprintf(stderr,"primary bool changed\n");}
  for(unsigned s=0;s<4;s++){t.output.channels[s].state.handle=-1;t.output.channels[s].state.user=-1;}
  t.input.fp_readstate=(intptr_t)native_read;t.input.fp_read=0;t.input.fp_usleep=(intptr_t)native_sleep;
  t.input.fp_closepad=(intptr_t)native_close;t.input.fp_kill=(intptr_t)native_owner;t.input.owner_check_interval=1;
  for(unsigned reason=0;reason<2;reason++){
   reset_calls();active=&t;watchdog=(int)reason;t.input.stop=reason?0:1;t.input.owner_miss_count=0;
   remote_pad_reader_stub(&t);expect(reason?"watchdog":"stop",ownership);
   if(t.input.ready!=2 || (reason && t.input.owner_watchdog_exits!=1)){failed++;fprintf(stderr,"reader completion failed\n");}
  }
  reset_calls();actual_failed_bootstrap(ownership,0,4);expect("failed-bootstrap",ownership);
  reset_calls();actual_failed_bootstrap(ownership,1,4);expect("live-bootstrap-no-close",0);
 }
 /* Impossible/alias metadata must never authorize a borrowed-primary close
  * or duplicate Sony close. Use the actual full reader with stopped input. */
 RemotePadReaderTransport alias=actual_factory(15);active=&alias;alias.input.stop=1;
 alias.input.fp_closepad=(intptr_t)native_close;
 alias.input.slot_handles[1]=44;
 reset_calls();remote_pad_reader_stub(&alias);expect("primary-alias-once",13);
 alias=actual_factory(12);active=&alias;alias.input.stop=1;alias.input.fp_closepad=(intptr_t)native_close;
 alias.input.slot_handles[2]=alias.input.slot_handles[3]=45;
 reset_calls();remote_pad_reader_stub(&alias);expect("secondary-alias-once",2);
 alias=actual_factory(2);active=&alias;alias.input.stop=1;alias.input.fp_closepad=(intptr_t)native_close;
 alias.input.slot_handles[1]=44;
 reset_calls();remote_pad_reader_stub(&alias);expect("borrowed-primary-alias-preserved",0);
 alias=actual_factory(15);active=&alias;alias.input.stop=1;alias.input.fp_closepad=0;
 reset_calls();remote_pad_reader_stub(&alias);expect("missing-close-api-no-call",0);
 reset_calls();actual_failed_bootstrap(0,0,0);expect("early-failure-no-admission",0);
 printf("actual constructor, stopped/watchdog reader and failed bootstrap: 16 ownership masks, checks=%u failed=%u\n",checks,failed);
 return failed?1:0;
}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='reader-owned-cleanup-') as tmp:
    c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
    build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
    run=subprocess.run([str(exe)],capture_output=True,text=True) if not build.returncode else None
    passed=run is not None and run.returncode==0
    print(json.dumps({'compiled':not build.returncode,'passed':passed,'output':run.stdout.strip() if run else '',
      'error':run.stderr if run else build.stderr[-4000:],'sources':{n:hashlib.sha256((payload/n).read_bytes()).hexdigest()for n in('wireless_ds4.c','reader_rumble_channel.h')},
      'scope':'Actual constructor and lifecycle code; Sony handles and ptrace are explicit host models, no physical/native cleanup acceptance'},indent=2))
sys.exit(0 if passed else 1)
