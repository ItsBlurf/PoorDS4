# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual recovery loop and routing APIs with explicit host discovery/IO mocks."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, sys, tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload',type=Path,default=ROOT/'payload')
p=parser.parse_args().payload;source=(p/'wireless_ds4.c').read_text();main=(p/'game_pad_bridge_main.c').read_text()
types=source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
start=source.rfind('typedef struct {',0,source.index('} GameBridgeDynlibObjectPrefix;'))
obj=source[start:source.index('} GameBridgeDynlibObjectPrefix;')+len('} GameBridgeDynlibObjectPrefix;')]
# Locate the actual reset loop, independently of where lifetime-safe discovery
# locals are declared. Moving those locals must not silently make this empty.
reader_start=function(source,'wireless_ds4_remote_reader_start')
reset_statement=reader_start.index('        g_reader_users[s] = -1;')
reset_begin=reader_start.rfind('    for (unsigned s = 0;',0,reset_statement)
reset_end=reader_start.index('    }',reset_statement)+len('    }')
reset=reader_start[reset_begin:reset_end]
assert reset_begin>=0 and 'g_game_reader_slots[s] = -1;' in reset
installer_fp=next(line.strip() for line in source.splitlines() if line.strip().startswith('args.fp_state_internal ='))
rebind='int '+function(source,'wireless_ds4_game_bridge_rebind_readers') if '\nwireless_ds4_game_bridge_rebind_readers(' in source else ''
record=''
if '\nrecord_source_user_candidates(' in main:
 start=main.index('static int32_t g_source_candidate_users')
 record=main[start:main.index('/* Remember',start)]+'static void '+function(main,'record_source_user_candidates')+'\n'
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "wireless_ds4.h"
#include "pad_types.h"
#define __PROSPERO__ 1
'''+types+obj+r'''
static int32_t g_reader_users[4],game_users[4];
static uint32_t eligible[4],table_native[4];
static int32_t table_handles[4];
static int g_game_reader_slots[4];
static pid_t g_game_bridge_direct_pid=94,g_reader_pid;
static intptr_t g_game_bridge_direct_args=0x3300,g_reader_args;
static uint32_t g_game_bridge_direct_seq,g_game_bridge_direct_packets,g_game_bridge_slot_seq[4],g_game_bridge_slot_packets[4];
static uint64_t g_game_bridge_direct_cr3,g_game_bridge_direct_dmap;
static PoorDS4PadSource g_pad_source;
static GamePadBridgeTriggerArgs complete_args;
static GamePadBridgeArgs args;
static int reader_restarted;
static int fail_observation,change_game_handle,change_native_class,change_internal,change_primary,change_user,select_calls;
static int reversed_users,source_index_changed,reader_case;
static void game_bridge_map_reader_slots(const int32_t *,const uint32_t *,int32_t);
static int game_bridge_copy_args(pid_t pid,intptr_t address,GamePadBridgeArgs *out){
 if((fail_observation && reader_restarted) || pid!=94 || address!=0x3300)return -1;*out=args;return 0;
}
int wireless_ds4_game_bridge_image_state(pid_t pid){(void)pid;return 0;}
static int game_bridge_process_write(pid_t pid,intptr_t address,const void *data,size_t size){
 if(pid!=94 || address<0x3300 || (size_t)(address-0x3300)>sizeof complete_args || size>sizeof complete_args-(size_t)(address-0x3300))return -1;
 size_t off=(size_t)(address-0x3300);
 if(off<sizeof args){if(size>sizeof args-off)return -1;memcpy((uint8_t*)&args+off,data,size);}
 else memcpy((uint8_t*)&complete_args+off,data,size);return 0;
}
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){
 if(pid!=94 || address<0x3300 || (size_t)(address-0x3300)>sizeof complete_args || size>sizeof complete_args-(size_t)(address-0x3300))return -1;
 size_t off=(size_t)(address-0x3300);
 if(off<sizeof args){if(size>sizeof args-off)return -1;memcpy(out,(const uint8_t*)&args+off,size);}
 else memcpy(out,(const uint8_t*)&complete_args+off,size);return 0;
}
static int get_lib_quiet(pid_t pid,const char *name,uint32_t *handle){
 if(pid!=94 || strcmp(name,"libScePad"))return -1;*handle=1;return 0;
}
static intptr_t kernel_dynlib_mapbase_addr(pid_t pid,uint32_t handle){return pid==94 && handle==1?0x100000:0;}
static intptr_t resolve_sym(pid_t pid,uint32_t handle,const char *name){return pid==94 && handle==1 && !strcmp(name,"scePadReadState")?(change_internal?0x120000:0x110000):0;}
static int game_cache_find_libpad_object(pid_t pid,intptr_t base,GameBridgeDynlibObjectPrefix *out){
 if(pid!=94 || base!=0x100000)return -1;memset(out,0,sizeof *out);out->mapbase=base;out->mapsize=0x100000;return 0;
}
static int game_cache_follow_read_state(pid_t pid,intptr_t base,uint64_t size,intptr_t state,intptr_t *out){
 if(pid!=94 || base!=0x100000 || size!=0x100000 || state!=0x110000)return -1;*out=change_internal?0x190000:0x180000;return 0;
}
static int game_bridge_select_pad_handle(pid_t pid,intptr_t base,intptr_t state,int32_t user,int32_t index,int fd,
 int32_t *handle,int32_t *selected,int32_t *handles,uint32_t *native,uint32_t *matches){
 select_calls++;if(pid!=94 || base!=0x100000 || state!=0x110000 || user!=11 || index!=0 || fd!=-1)return -1;
 *selected=change_primary?2:1;*handle=args.slots[*selected].pad_handle;
 for(unsigned i=0;i<4;i++){handles[i]=table_handles[i];native[i]=table_native[i];matches[i]=game_users[i]==user;}
 if(change_game_handle)handles[2]=0x6602;if(change_native_class){native[2]=1;eligible[2]=0;}
 game_bridge_map_reader_slots(game_users,eligible,*selected);return 0;
}
static int lifecycle_should_stop(void){return 0;}
static int process_alive(pid_t pid){return pid==94;}
int wireless_ds4_game_bridge_status(pid_t pid,intptr_t address,PoorDS4GameBridgeStatus *out){
 if(pid!=94 || address!=0x3300)return -1;memset(out,0,sizeof *out);out->bridge_ready=1;out->active=1;return 0;
}
static uint64_t monotonic_milliseconds(void){return 1000;}
static uint32_t collect_user_candidates(int32_t *out){out[0]=reversed_users?22:11;out[1]=reversed_users?11:22;return 2;}
int wireless_ds4_remote_reader_start(const int32_t *users,uint32_t count,PoorDS4PadSource *out,pid_t *pid,intptr_t *address){
 if(!users || count!=2 || users[0]!=11)return -1;
'''+reset+r'''
 /* Discovery reordered a secondary physical reader: source user 22 is now
  * ordinal two. The actual production reset fragment above clears old routes. */
 g_reader_users[0]=change_user?12:11;g_reader_users[1]=reader_case==1?-1:reader_case==2?22:33;g_reader_users[2]=reader_case==1?-1:22;
 *out=(PoorDS4PadSource){g_reader_users[0],source_index_changed?2:0,44,1};*pid=63;*address=0x1234;reader_restarted=1;return 0;
}
static void poords4_log(const char *format,...){(void)format;}
static void write_supervisor_state(const char *name,pid_t pid,unsigned sessions,unsigned long long frames,int force){(void)name;(void)pid;(void)sessions;(void)frames;(void)force;}
static int sleep_interruptible(unsigned usec){(void)usec;return -1;}
'''+record+ 'int '+function(source,'wireless_ds4_game_bridge_update_slot')+'\n'+'int '+function(source,'wireless_ds4_game_bridge_deactivate_slot')+'\n'+'int '+function(source,'wireless_ds4_game_bridge_update')+'\n'+ 'static void '+function(source,'game_bridge_map_reader_slots')+'\n'+rebind+'\n'+'int '+function(source,'wireless_ds4_game_bridge_ack_reset')+'\n'+'static void '+function(main,'make_neutral_connected_pad')+'\n'+'static int '+function(main,'recover_wireless_reader_in_place')+r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static void setup(void){
 memset(&complete_args,0,sizeof complete_args);memset(&args,0,sizeof args);args.magic=POORDS4_GAME_BRIDGE_MAGIC;args.reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
 args.pad_size=POORDS4_GAME_BRIDGE_PAD_SIZE;reader_restarted=0;
 memset(g_game_bridge_slot_seq,0,sizeof g_game_bridge_slot_seq);memset(g_game_bridge_slot_packets,0,sizeof g_game_bridge_slot_packets);
 /* RC51's status reports direct_active, while the legacy active field stays
  * zero. Keep the actual installer/producer contract, not a synthetic flag. */
 args.direct_active=1;args.reserved0=1;args.pad_handle=0x3301;intptr_t originals[10]={0x110000};
'''+installer_fp+r'''
 for(unsigned i=0;i<4;i++){args.slots[i].pad_handle=-1;game_users[i]=-1;eligible[i]=0;g_reader_users[i]=-1;g_game_reader_slots[i]=-1;}
 args.slots[0].pad_handle=0x4400;args.slots[0].is_dualsense=1;args.slots[1].pad_handle=0x3301;args.slots[2].pad_handle=0x5502;
 args.slots[1].reserved[0]=1;
 for(unsigned i=0;i<4;i++){table_handles[i]=args.slots[i].pad_handle;table_native[i]=args.slots[i].is_dualsense;}
 game_users[0]=9;game_users[1]=11;game_users[2]=22;eligible[1]=eligible[2]=1;
 g_reader_users[0]=11;g_reader_users[1]=22;game_bridge_map_reader_slots(game_users,eligible,1);
 fail_observation=change_game_handle=change_native_class=change_internal=change_primary=change_user=select_calls=0;
 reversed_users=source_index_changed=reader_case=0;
}
static void setup_live(void){
 setup();args.slots[0].reserved[2]=POORDS4_CLIENT_BINDING_MAGIC;
 complete_args.client_binding_magic=POORDS4_CLIENT_BINDING_MAGIC;
 complete_args.client_primary_slot=1;
 for(unsigned s=0;s<4;s++)complete_args.client_users[s]=-1;
 complete_args.client_users[1]=11;complete_args.client_users[2]=22;
}
static int recover(void){PoorDS4PadSource source={11,0,44,1};pid_t reader=63;intptr_t address=0x1000;
 return recover_wireless_reader_in_place(&source,&reader,&address,94,0x3300,1,600);}
int main(void){
 setup();CHECK(g_game_reader_slots[2]==1);CHECK(recover()==0);
 CHECK(args.slots[0].is_dualsense==1 && args.slots[0].active==0 && args.slots[1].active==1);
 CHECK(g_game_reader_slots[2]==2 && g_game_reader_slots[0]==-1 && g_game_reader_slots[1]==-1);
 setup();change_game_handle=1;CHECK(recover()==1 && g_game_reader_slots[2]==-1);
 setup();change_native_class=1;CHECK(recover()==1 && g_game_reader_slots[2]==-1);
 setup();change_internal=1;CHECK(recover()==1 && g_game_reader_slots[2]==-1 && !select_calls);
 setup();change_primary=1;CHECK(recover()==1 && g_game_reader_slots[2]==-1);
 setup();fail_observation=1;CHECK(recover()==1 && g_game_reader_slots[2]==-1);
 setup();change_user=1;CHECK(recover()==1 && g_game_reader_slots[2]==-1 && !select_calls);
 /* Live handle ownership is intentionally not a frozen table topology. A
  * new reader and Sony native clients must not trigger hook removal. */
 setup_live();reversed_users=source_index_changed=1;
 CHECK(recover()==0 && !select_calls);
 CHECK(complete_args.client_users[0]==33 && complete_args.client_users[1]==11 && complete_args.client_users[2]==22);
 CHECK(g_game_reader_slots[0]==1 && g_game_reader_slots[1]==-1 && g_game_reader_slots[2]==2);
 CHECK(args.slots[0].active==0 && !args.slots[0].is_dualsense);
 CHECK(args.slots[1].pad_handle==0x3301 && args.slots[2].pad_handle==0x5502);
 args.reset_requested=1;args.reset_combo_ticks=45;memset((void*)args.reset_ticks,45,sizeof args.reset_ticks);
 CHECK(wireless_ds4_game_bridge_ack_reset(94,0x3300)==0 && !args.reset_requested && !args.reset_combo_ticks);
 for(unsigned s=0;s<4;s++)CHECK(!args.reset_ticks[s]);
 CHECK(wireless_ds4_game_bridge_ack_reset(95,0x3300)==-1);
 setup_live();reader_case=1;CHECK(recover()==0 && !select_calls);
 CHECK(complete_args.client_users[0]==-1 && complete_args.client_users[1]==11 && complete_args.client_users[2]==-1);
 CHECK(g_game_reader_slots[0]==-1 && g_game_reader_slots[2]==-1);
 setup_live();reader_case=2;CHECK(recover()==0 && !select_calls);
 CHECK(complete_args.client_users[0]==-1 && complete_args.client_users[2]==-1 && g_game_reader_slots[2]==-1);
 setup_live();complete_args.client_users[1]=99;CHECK(recover()==1 && !select_calls);
 for(unsigned primary=0;primary<4;primary++){
  setup_live();memset(args.slots,0,sizeof args.slots);
  args.direct_active=args.direct_seq=args.direct_packets=0;
  args.slots[0].reserved[2]=POORDS4_CLIENT_BINDING_MAGIC;
  args.pad_handle=0x03000708+(int)primary;args.reserved0=8+primary;
  args.slots[primary].pad_handle=args.pad_handle;args.slots[primary].active=1;
  for(unsigned s=0;s<4;s++)complete_args.client_users[s]=-1;
  complete_args.client_primary_slot=primary;complete_args.client_users[primary]=11;
  CHECK(recover()==0 && !select_calls && complete_args.client_users[primary]==11);
  CHECK(args.slots[primary].pad_handle==args.pad_handle && args.slots[primary].active);
  for(unsigned s=0;s<4;s++)if(s!=primary && complete_args.client_users[s]>=0){
   int reader=g_game_reader_slots[s];CHECK(reader>0 && reader<4);
   CHECK(complete_args.client_users[s]==g_reader_users[reader]);
  }
 }
 puts("production neutral publisher and recovery restore reordered secondary reader by user; native slot untouched; changed game handle/class/primary/library and failed observation retain no old route; changed source user requests existing reinstall");return 0;
}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='reader-route-recovery-') as tmp:
 c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(p),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True) if not build.returncode else None
 passed=run is not None and run.returncode==0
 print(json.dumps({'compiled':not build.returncode,'passed':passed,'exit':run.returncode if run else None,'output':run.stdout.strip() if run else '',
 'error':run.stderr if run else build.stderr[-3500:],'sources':{n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in ('wireless_ds4.c','wireless_ds4.h','game_pad_bridge_main.c')},'native_console_verified':False},indent=2))
sys.exit(0 if passed else 1)
