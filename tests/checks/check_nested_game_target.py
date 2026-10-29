# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute production game-target discovery against a modeled process list.

A launcher eboot.bin can exit after starting the game from a nested executable
(/app0/mgs4/mgs4.elf, named "mgs4/mgs4.elf" by the kernel). Checks that such a
process is selected only when no eboot.bin exists, its main image is that file
under /app0 and its runtime modules are loaded; that eboot.bin keeps its rule
and precedence; that homebrew payloads are never inspected; and that ambiguous,
malformed or unreadable lists select nothing. The process names come from an
11.60 inventory captured while MGS4 and Peace Walker were running.
No console process or controller is touched.
"""
import argparse,hashlib,json,os,re,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--payload',type=Path,default=ROOT/'payload')
payload=p.parse_args().payload;s=(payload/'wireless_ds4.c').read_text()
def typedef(name):
    end=s.index('} '+name+';')+len('} '+name+';');return s[s.rfind('typedef struct {',0,end):end]
# Without the shared selector (v0.2.0 and earlier) only the production
# find_target cases are compiled, so the control fails for the launcher case.
selector='static int\ngame_bridge_select_target(' in s
prelude=s[s.index('#define GAME_BRIDGE_PROC_NAME_MAX'):s.index('static int\ngame_bridge_nested_name_ok(')] if selector else ''
names=['game_bridge_nested_name_ok','game_bridge_main_image_path','game_bridge_nested_image_ok',
       'game_bridge_target_modules_ready','game_bridge_nested_log','game_bridge_find_nested_target',
       'game_bridge_select_target']
code='static size_t\n'+function(s,'find_pids')+'\nstatic int\n'+function(s,'game_bridge_is_native_ps4')
if selector:code+='\n#define HAVE_SELECTOR 1'+''.join('\n'+('static void\n' if n=='game_bridge_nested_log' else 'static int\n')+function(s,n) for n in names)
code+='\nint\n'+function(s,'wireless_ds4_game_bridge_find_target')
# Installer selection is checked in source: a lost selection must not reach
# the supervisor as an error without a game pid.
installer=function(s,'wireless_ds4_game_bridge_run_passive')
installer_ok=selector and bool(re.search(r'game_bridge_select_target\(&target\);\s*if \(\w+ != 1\)\s*return \w+ == -3 \? -3 : -2;',installer)) \
    and 'find_pids("eboot.bin"' not in installer
program=r'''
#define __PROSPERO__ 1
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "wireless_ds4.h"
'''+typedef('GameBridgeDynlibObjectPrefix')+'\n'+typedef('GameBridgeAppInfo')+r'''
typedef struct{pid_t pid;char comm[20];char image[256];int readable,pad,libc,kernel;char title[10];}Proc;
static Proc procs[128];static unsigned nprocs,kernel_reads,bad;static int sysctl_fail,malformed,duplicate=-1;
static char logbuf[8192];
static void reset(void){memset(procs,0,sizeof procs);nprocs=kernel_reads=bad=0;sysctl_fail=malformed=0;duplicate=-1;logbuf[0]=0;}
static Proc *find(pid_t pid){for(unsigned i=0;i<nprocs;i++)if(procs[i].pid==pid)return &procs[i];return 0;}
static Proc *add(pid_t pid,const char *name){Proc *q=&procs[nprocs++];memset(q,0,sizeof *q);q->pid=pid;
 strncpy(q->comm,name,19);snprintf(q->image,sizeof q->image,"/system/common/lib/%s",name);q->readable=1;strcpy(q->title,"NPXS40000");return q;}
static Proc *game(pid_t pid,const char *rel){Proc *q=find(pid);if(q&&strncmp(q->comm,rel,19))bad++;if(!q)q=add(pid,rel);
 snprintf(q->image,sizeof q->image,"/app0/%s",rel);q->pad=q->libc=q->kernel=1;strcpy(q->title,"PPSA01234");return q;}
static pid_t getpid(void){return 7;}
static void poords4_log(const char *f,...){va_list a;va_start(a,f);size_t n=strlen(logbuf);vsnprintf(logbuf+n,sizeof logbuf-n,f,a);va_end(a);}
static int sysctl(const int *mib,unsigned len,void *out,size_t *size,const void *in,size_t inlen){
 (void)in;(void)inlen;if(sysctl_fail||len!=4||mib[0]!=1||mib[1]!=14||mib[2]!=8)return -1;
 unsigned rows=nprocs+(duplicate>0);size_t need=rows*1088u+(malformed?2u:0u);
 if(!out){*size=need;return 0;}if(*size<need)return -1;memset(out,0,need);uint8_t *r=out;
 for(unsigned i=0;i<rows;i++,r+=1088){const Proc *q=i<nprocs?&procs[i]:find(duplicate);int sz=1088;
  memcpy(r,&sz,4);memcpy(r+72,&q->pid,sizeof q->pid);memcpy(r+447,q->comm,strlen(q->comm)+1);}
 *size=need;return 0;}
/* proc+0x3e8 -> head -> objects; a library precedes handle 0. */
typedef struct{uint8_t proc[0x400];uint64_t head;GameBridgeDynlibObjectPrefix obj[2];char path[2][256];}KProc;
static KProc kp[128];
static intptr_t kernel_get_proc(pid_t pid){kernel_reads++;Proc *q=find(pid);if(!q||!q->readable)return 0;KProc *k=&kp[q-procs];
 memset(k,0,sizeof *k);strcpy(k->path[0],"/system/common/lib/libkernel_sys.sprx");strcpy(k->path[1],q->image);
 k->obj[0].handle=0x2001;k->obj[0].path=(uint64_t)(uintptr_t)k->path[0];k->obj[0].next=(uint64_t)(uintptr_t)&k->obj[1];
 k->obj[1].handle=0;k->obj[1].path=(uint64_t)(uintptr_t)k->path[1];k->head=(uint64_t)(uintptr_t)&k->obj[0];
 uint64_t h=(uint64_t)(uintptr_t)&k->head;memcpy(k->proc+0x3e8,&h,8);return (intptr_t)k->proc;}
static int kernel_copyout(intptr_t a,void *out,size_t n){if(!a)return -1;memcpy(out,(const void*)a,n);return 0;}
static int get_lib_quiet(pid_t pid,const char *name,uint32_t *h){Proc *q=find(pid);*h=0;if(!q)return -1;
 int ok=(!strcmp(name,"libScePad")&&q->pad)||(!strcmp(name,"libSceLibcInternal")&&q->libc)||(!strcmp(name,"libkernel_sys")&&q->kernel);
 if(ok)*h=1;
 return ok?0:-1;}
static int sceKernelGetAppInfo(pid_t pid,GameBridgeAppInfo *info){Proc *q=find(pid);if(!q)return -1;
 memcpy(info->title_id,q->title,10);return 0;}
'''+prelude+code+r'''
static const char *inventory[]={"SceSysCore","SceShellCore","SceShellUI","SceRemotePlay","SceSystemManager",
 "SceVdecProxy","SceAvCapture","SceGameLiveStreamin","ScePsNowClientDaemo","SceCdlgApp","SceSpZeroConf",
 "elfldr.elf","kstuff.elf","ps5upload.elf","shadowmountplus.elf","mini-syscore.elf","pldmgr.elf"};
static void load(void){for(unsigned i=0;i<sizeof inventory/sizeof *inventory;i++){Proc *q=add(100+(pid_t)i,inventory[i]);q->pad=q->libc=q->kernel=1;}}
static unsigned checks;
#define EXPECT(x)do{checks++;if(!(x)){fprintf(stderr,"line=%d check=%s log=%s\n",__LINE__,#x,logbuf);return 1;}}while(0)
static void fresh(void){reset();load();
#ifdef HAVE_SELECTOR
 g_nested_log_pid=-1;g_nested_log_state=0;g_nested_log_ambiguous=0;
#endif
}
int main(void){pid_t pid;int r;
 /* Peace Walker and MGS4 after the launcher exits. */
 fresh();game(169,"mgspw/mgspw.elf");r=wireless_ds4_game_bridge_find_target(&pid);
 if(r!=1)fprintf(stderr,"launcher handoff: nested game not selected result=%d pid=%d\n",r,pid);
 EXPECT(r==1&&pid==169&&kernel_reads==1&&strstr(logbuf,"name=mgspw/mgspw.elf image=/app0/mgspw/mgspw.elf state=selected"));
 fresh();game(209,"mgs4/mgs4.elf");EXPECT(wireless_ds4_game_bridge_find_target(&pid)==1&&pid==209);
 fresh();game(169,"mgspw/mgspw.elf");game(154,"eboot.bin");EXPECT(wireless_ds4_game_bridge_find_target(&pid)==1&&pid==154);
#ifdef HAVE_SELECTOR
 /* eboot.bin keeps precedence and its own readiness rule; no kernel reads. */
 fresh();game(169,"mgspw/mgspw.elf");game(154,"eboot.bin");r=game_bridge_select_target(&pid);EXPECT(r==1&&pid==154&&!kernel_reads);
 fresh();add(154,"eboot.bin");EXPECT(wireless_ds4_game_bridge_find_target(&pid)==-4&&pid==154);
 fresh();game(154,"eboot.bin");game(155,"eboot.bin");EXPECT(game_bridge_select_target(&pid)==-3&&pid==-1);
 fresh();EXPECT(wireless_ds4_game_bridge_find_target(&pid)==-2&&pid==-1&&!kernel_reads);
 /* Modules or image not ready: report the pid, never select it. */
 fresh();game(169,"mgspw/mgspw.elf")->pad=0;EXPECT(wireless_ds4_game_bridge_find_target(&pid)==-4&&pid==169);
 fresh();game(169,"mgspw/mgspw.elf")->readable=0;EXPECT(game_bridge_select_target(&pid)==-4&&pid==169&&strstr(logbuf,"state=waiting_image"));
 fresh();game(169,"mgspw/mgspw.elf")->pad=0;game(171,"tools/helper.elf")->pad=0;EXPECT(game_bridge_select_target(&pid)==-4&&pid==-1);
 /* The image must be the named file under /app0. */
 fresh();strcpy(game(169,"mgspw/mgspw.elf")->image,"/data/homebrew/mgspw/mgspw.elf");
 EXPECT(game_bridge_select_target(&pid)==-2&&strstr(logbuf,"state=not_app0"));
 fresh();strcpy(game(169,"mgspw/mgspw.elf")->image,"/app0/other/mgspw.elf");EXPECT(game_bridge_select_target(&pid)==-2);
 fresh();strcpy(game(169,"mgspw/mgspw.elf")->image,"/app0/mgspw/mgspw.elf.bak");EXPECT(game_bridge_select_target(&pid)==-2);
 fresh();strcpy(game(169,"mgspw/mgspw.elf")->image,"/mnt/sandbox/PPSA01234_000/app0/mgspw/mgspw.elf");EXPECT(game_bridge_select_target(&pid)==1);
 /* A 19-byte kernel name is a prefix of the image path. */
 fresh();{Proc *q=add(300,"metalgear/mgs2hd_x64.elf");strcpy(q->image,"/app0/metalgear/mgs2hd_x64.elf");q->pad=q->libc=q->kernel=1;
  EXPECT(strlen(q->comm)==19&&game_bridge_select_target(&pid)==1&&pid==300);
  strcpy(q->image,"/app0/metalgear/mgs2hd_x64.bin");g_nested_log_pid=-1;EXPECT(game_bridge_select_target(&pid)==-2);}
 /* Two verified candidates fail closed and log once. */
 fresh();game(169,"mgspw/mgspw.elf");game(170,"mgs4/mgs4.elf");EXPECT(wireless_ds4_game_bridge_find_target(&pid)==-3&&pid==-1);
 logbuf[0]=0;EXPECT(game_bridge_select_target(&pid)==-3&&!logbuf[0]);
 fresh();game(169,"mgspw/mgspw.elf");game(171,"tools/helper.elf")->pad=0;EXPECT(game_bridge_select_target(&pid)==1&&pid==169);
 /* List integrity. */
 fresh();game(169,"mgspw/mgspw.elf");duplicate=169;EXPECT(game_bridge_select_target(&pid)==1&&pid==169);
 fresh();game(169,"mgspw/mgspw.elf");malformed=1;EXPECT(game_bridge_select_target(&pid)==-3&&pid==-1);
 fresh();game(169,"mgspw/mgspw.elf");sysctl_fail=1;EXPECT(game_bridge_select_target(&pid)==-2&&pid==-1);
 /* A nested PS4 title keeps native passthrough. */
 fresh();strcpy(game(169,"mgspw/mgspw.elf")->title,"CUSA01234");
 EXPECT(wireless_ds4_game_bridge_find_target(&pid)==POORDS4_GAME_BRIDGE_NATIVE_PS4&&pid==169);
 /* One log line per candidate state. */
 fresh();game(169,"mgspw/mgspw.elf");for(int i=0;i<5;i++)game_bridge_select_target(&pid);
 {const char *first=strstr(logbuf,"state=selected");EXPECT(first&&!strstr(first+1,"state=selected"));}
 EXPECT(!game_bridge_nested_name_ok("eboot.bin",9)&&!game_bridge_nested_name_ok("kstuff.elf",10)&&
        !game_bridge_nested_name_ok("../x.elf",8)&&!game_bridge_nested_name_ok("/a.elf",6)&&game_bridge_nested_name_ok("a/b",3));
#endif
 EXPECT(!bad);
 printf("nested game target selection: checks=%u inventory=%u\n",checks,(unsigned)(sizeof inventory/sizeof *inventory));return 0;}
'''
with tempfile.TemporaryDirectory(prefix='poords4-nested-target-')as td:
 td=Path(td);c=td/'model.c';exe=td/'model.exe';c.write_text(program)
 build=subprocess.run([os.environ.get('POORDS4_HOST_CC','gcc'),'-std=c11','-O2','-Wall','-Wno-unused-function','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
 result={'compiled':build.returncode==0,'passed':bool(run and run.returncode==0 and installer_ok),'installer_selection_shared':installer_ok,'output':run.stdout if run else '','error':run.stderr if run else build.stderr[-4000:],'sources':{'wireless_ds4.c':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest()}}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
