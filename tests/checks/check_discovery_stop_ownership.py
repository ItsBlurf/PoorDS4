# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production reader status/stop against bounded host transport memory.

Checks that uncertain probe closes or unavailable discovery state prevent a
replacement, while legacy and normally closed readers still stop successfully.
No console process or controller is touched.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--payload',type=Path,default=ROOT/'payload')
payload=p.parse_args().payload;s=(payload/'wireless_ds4.c').read_text()
end=s.index('} RemotePadReaderArgs;')+len('} RemotePadReaderArgs;');types=s[s.rfind('typedef struct {',0,end):end]
program=r'''
#define __PROSPERO__ 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
#define POORDS4_REMOTE_PAD_CAPACITY 256
#define MSG_DONTWAIT 0x80
#define PT_ATTACH 10
#define PT_DETACH 11
typedef char *caddr_t;
'''+types+r'''
#include "reader_rumble_channel.h"
static RemotePadReaderTransport t;
static unsigned bad,closed,checks;
static int copy_fault;
static int g_reader_rumble_fd;static pid_t g_reader_rumble_pid;static intptr_t g_reader_rumble_args;
static uint64_t g_reader_rumble_sent[4];
static void klog_printf(const char *format,...){(void)format;}
static void usleep(unsigned us){(void)us;}
static int sys_ptrace(int kind,pid_t pid,caddr_t a,int b){(void)kind;(void)pid;(void)a;(void)b;bad++;return -1;}
static pid_t waitpid(pid_t pid,void *out,int flags){(void)pid;(void)out;(void)flags;bad++;return -1;}
static int pt_io_write(pid_t pid,intptr_t addr,const void *src,size_t size){(void)pid;(void)addr;(void)src;(void)size;bad++;return -1;}
static int model_close(int fd){if(fd!=17)bad++;closed++;return 0;}
#define close model_close
static int remote_reader_copyout(pid_t pid,intptr_t addr,void *out,size_t size){
 intptr_t base=(intptr_t)&t;
 if(pid!=99 || addr<base || (uintptr_t)(addr-base)+size>sizeof t){bad++;return -1;}
 intptr_t discovery=base+offsetof(RemotePadReaderTransport,discovery);
 if(t.discovery.magic==POORDS4_DISCOVERY_MAGIC_V1 && addr>=discovery &&
    (uintptr_t)(addr-discovery)+size>offsetof(PoorDS4SourceDiscovery,probe_handle)){bad++;return -1;}
 if(copy_fault==1 && addr==discovery && size==sizeof(uint64_t))return -1;
 if(copy_fault==2 && addr==discovery && size==sizeof(PoorDS4SourceDiscovery))return -1;
 if(copy_fault==3 && addr==discovery && size==offsetof(PoorDS4SourceDiscovery,probe_handle))return -1;
 memcpy(out,(void*)addr,size);return 0;
}
static int remote_reader_copyin(pid_t pid,const void *src,intptr_t addr,size_t size){
 if(pid!=99 || addr!=(intptr_t)&t.input.stop || size!=4){bad++;return -1;}
 memcpy((void*)addr,src,size);return 0;
}
int '''+function(s,'wireless_ds4_remote_reader_status')+'\nint '+function(s,'wireless_ds4_remote_reader_stop')+r'''
#define EXPECT(x)do{checks++;if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static int exercise(int scenario){
 memset(&t,0,sizeof t);bad=closed=0;copy_fault=0;
 t.input.ready=2;t.input.owner_pid=76;t.input.owner_check_interval=120;
 t.input.fp_readstate=t.input.fp_usleep=t.input.fp_kill=1;
 t.discovery.magic=POORDS4_DISCOVERY_MAGIC;
 if(scenario==1)t.discovery.close_errors=1;
 if(scenario==2)copy_fault=1;
 if(scenario==3)copy_fault=2;
 if(scenario==4)t.discovery.magic=0;
 if(scenario==5 || scenario==6 || scenario==8){
  t.discovery.magic=POORDS4_DISCOVERY_MAGIC_V1;
  t.discovery.probe_held=99;t.discovery.probe_waits=99;
 }
 if(scenario==6)t.discovery.close_errors=1;
 if(scenario==7)t.discovery.probe_held=1;
 if(scenario==8)copy_fault=3;
 if(scenario==9){t.discovery.probe_waits=7;t.discovery.probe_timeouts=3;t.discovery.last_state_result=-9;}
 PoorDS4RemoteReaderStatus status;
 int snapshot=wireless_ds4_remote_reader_status(99,(intptr_t)&t,&status);
 if(scenario==2 || scenario==3 || scenario==8)EXPECT(snapshot==-1);
 else {
  EXPECT(snapshot==0 && status.discovery_available==(scenario!=4));
  if(scenario==5 || scenario==6)EXPECT(!status.discovery_probe_held && !status.discovery_probe_waits);
  if(scenario==9)EXPECT(status.discovery_probe_waits==7 && status.discovery_probe_timeouts==3 && status.discovery_last_state_result==-9);
 }
 g_reader_rumble_fd=17;g_reader_rumble_pid=99;g_reader_rumble_args=(intptr_t)&t;
 int result=wireless_ds4_remote_reader_stop(99,(intptr_t)&t);
 EXPECT(t.input.stop==1 && closed==1 && !bad && g_reader_rumble_fd==-1);
 EXPECT(result==((scenario==0 || scenario==4 || scenario==5 || scenario==9)?0:-1));
 return 0;
}
int main(void){for(int s=0;s<10;s++)if(exercise(s))return 1;printf("stop ownership and V1/V2 bounded status: checks=%u cases=10\n",checks);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='poords4-discovery-stop-')as td:
 td=Path(td);c=td/'model.c';exe=td/'model.exe';c.write_text(program)
 build=subprocess.run([os.environ.get('POORDS4_HOST_CC','gcc'),'-std=c11','-O2','-Wall','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
 result={'compiled':build.returncode==0,'passed':bool(run and run.returncode==0),'output':run.stdout if run else '', 'error':run.stderr if run else build.stderr[-4000:],'sources':{n:hashlib.sha256((payload/n).read_bytes()).hexdigest()for n in ['wireless_ds4.c','source_device_discovery.h']},'native_close_error_injected':False}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
