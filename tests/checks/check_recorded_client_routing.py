# SPDX-License-Identifier: GPL-3.0-or-later
"""Production selection/publication/gateways with the reported #8/#26 layout.

Table discovery, unrecorded bytes and Sony calls are explicit host models.
Changing which modeled user is the source tests attribution decisions, not
physical join-order acceptance. Native slot-zero preservation is checked by
its actual callback result, not by whether a telemetry slot was displaced.
"""
import argparse,hashlib,json,os,re,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--payload',type=Path,default=ROOT/'payload')
payload=p.parse_args().payload
s=(payload/'wireless_ds4.c').read_text();main=(payload/'game_pad_bridge_main.c').read_text()
fixture_path=ROOT/'tests/fixtures/client-layout.json'
# Git checkouts can use LF or CRLF; fingerprint normalized UTF-8 text.
fixture_text=fixture_path.read_text(encoding='utf-8')
assert hashlib.sha256(fixture_text.encode('utf-8')).hexdigest()=='23c80fd4e4e96eb3d127b7471a64a3c357805d2fba40cfc88d19880cfbf877e3'
fixture=json.loads(fixture_text)
table=fixture['table_address'];stride=fixture['stride'];memory=bytearray(stride*fixture['slot_count']);recorded=fixture['recorded_bytes']
for entry in fixture['entries']:
 for field,(offset,width) in fixture['field_offsets'].items():
  at=entry['index']*stride+offset;memory[at:at+width]=entry[field].to_bytes(width,'little')
assert recorded==1152

types=s[s.index('#define POORDS4_GAME_BRIDGE_MAGIC'):s.index('static pid_t g_game_bridge_direct_pid')]
helper_start=s.rfind('static __attribute__',0,s.index('\ngame_bridge_find_slot('))
helper_end=s.index('__attribute__((noinline, used, section(".text.ds4gamebridge")))',helper_start)
init_start=s.index('    GamePadBridgeArgs args;\n    memset(&args, 0, sizeof(args));',s.index('\nwireless_ds4_game_bridge_run_passive('))
init_end=s.index('    args.fp_state_internal =',init_start)
initialization=s[init_start:init_end]
decision_start=main.index('    unsigned primary_slot = 0;')
decision_end=main.index('    last_bridge_status = initial_status;',decision_start)
decision=main[decision_start:decision_end]
callbacks=('game_pad_read_state_stub','game_pad_read_state_ext_stub','game_pad_read_stub','game_pad_read_ext_stub','game_pad_get_data_internal_stub')
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define __PROSPERO__ 1
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+'\n'+ '\n'.join(re.findall(r'^#define POORDS4_PAD_CLIENT_(?:STRIDE|CONNECTED|HANDLE|USER_ID|VENDOR|PRODUCT)_1160.*$',s,re.M))+r'''
static GamePadBridgeArgs game;
static ScePadData native,primary,secondary,tertiary,quaternary;
static pid_t g_game_bridge_direct_pid=42;
static intptr_t g_game_bridge_direct_args;
static uint32_t g_game_bridge_direct_seq,g_game_bridge_direct_packets;
static uint32_t g_game_bridge_slot_seq[4],g_game_bridge_slot_packets[4];
static uint64_t g_game_bridge_direct_cr3,g_game_bridge_direct_dmap;
static int32_t g_reader_users[4]={22,11,-1,-1};
static int g_game_reader_slots[4];
static PoorDS4PadSource g_pad_source;
static int g_sim_mask,native_present=1;
static unsigned reader_available_mask=14;
static uint32_t g_sim_buttons[4];
static uint8_t g_sim_lx[4],g_sim_ly[4],g_sim_rx[4],g_sim_ry[4],g_sim_l2[4],g_sim_r2[4];
static void refresh_simulated_pads_config(void){}
static uint64_t monotonic_milliseconds(void){return 1000;}
static uint8_t table_memory[] = {'''+','.join(map(str,memory))+r'''};
static intptr_t table_address='''+str(table)+r''';
typedef struct {uint64_t mapbase,mapsize,section_count;unsigned refcount;} GameBridgeDynlibObjectPrefix;
static int game_cache_find_libpad_object(int p,intptr_t b,GameBridgeDynlibObjectPrefix *o){(void)p;o->mapbase=b;o->mapsize=0x30000;return 0;}
static int game_cache_find_table(int p,intptr_t b,uint64_t n,intptr_t r,int32_t u,int32_t i,int fd,intptr_t *t,uint32_t *stride){(void)p;(void)b;(void)n;(void)r;(void)u;(void)i;(void)fd;*t=table_address;*stride=0x5c8;return 0;}
static int report_printf(int fd,const char *fmt,...){(void)fd;(void)fmt;return 0;}
static void klog_printf(const char *fmt,...){(void)fmt;}
static void game_bridge_report_prefix(int fd,const char *n,const void *b,size_t s){(void)fd;(void)n;(void)b;(void)s;}
#define game_bridge_dump_client_table_report(...) ((void)0)
static int game_bridge_process_read(pid_t pid,intptr_t address,void *out,size_t size){
 (void)pid;uintptr_t off=(uintptr_t)address-(uintptr_t)table_address;
 if(off>sizeof table_memory || size>sizeof table_memory-off)return -1;
 memcpy(out,table_memory+off,size);return 0;
}
static int game_bridge_process_write(pid_t pid,intptr_t address,const void *in,size_t size){
 uintptr_t off=(uintptr_t)address-(uintptr_t)&game;
 if(pid!=42 || off>sizeof game || size>sizeof game-off)return -1;
 memcpy((uint8_t*)&game+off,in,size);return 0;
}
static int native_state(int h,void *out,int mode){(void)mode;ScePadData d={0};if((h&255)==0 && native_present)d=native;memcpy(out,&d,sizeof d);return 0;}
static int native_state_ext(int h,void *out){return native_state(h,out,1);}
static int native_read(int h,void *out,int n,int mode){if((h&255)!=0 || !native_present)return 0;if(n>0)native_state(h,out,mode);return n>0?1:0;}
static int native_read_ext(int h,void *out,int n){return native_read(h,out,n,1);}
static int game_bridge_copy_args(pid_t pid,intptr_t address,GamePadBridgeArgs *out){if(pid!=42 || address!=(intptr_t)&game)return -1;*out=game;return 0;}
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){uintptr_t off=(uintptr_t)address-(uintptr_t)&game;if(pid!=42 || off>sizeof game || size>sizeof game-off)return -1;memcpy(out,(uint8_t*)&game+off,size);return 0;}
int wireless_ds4_game_bridge_image_state(pid_t pid){(void)pid;return 0;}
int wireless_ds4_remote_reader_read_slot(pid_t pid,intptr_t args,unsigned slot,void *out,uint32_t size,uint32_t *seq){(void)args;if(pid!=99 || slot<1 || slot>3 || !(reader_available_mask&(1u<<slot)) || size!=sizeof secondary)return -1;const ScePadData *pad=slot==1?&secondary:slot==2?&tertiary:&quaternary;memcpy(out,pad,size);if(seq)*seq=2;return 0;}
'''
program+='static int '+function(s,'remote_pad_is_known_ds4')+'\n'
program+='static void '+function(s,'game_bridge_map_reader_slots')+'\n'
if '\ngame_bridge_collect_reader_clients(' in s:
 start=s.rfind('typedef struct {',0,s.index('} GameBridgeReaderClient;'))
 program+=s[start:s.index('} GameBridgeReaderClient;')+len('} GameBridgeReaderClient;')]+'\n'
 program+='static void '+function(s,'game_bridge_collect_reader_clients')+'\n'
 program+='#define HAS_READER_COLLECTOR 1\n'
program+='static int '+function(s,'game_bridge_select_pad_handle')+'\n'
program+=s[helper_start:helper_end]+'\n'+'\n'.join('int32_t '+function(s,n) for n in callbacks)+'\n'
program+='int '+function(s,'wireless_ds4_game_bridge_update_slot')+'\n'
program+='int '+function(s,'wireless_ds4_game_bridge_deactivate_slot')+'\n'
program+='int '+function(s,'wireless_ds4_remote_reader_read_game_slot')+'\n'
program+='int '+function(s,'wireless_ds4_game_bridge_status')+'\n'
program+='static void '+function(main,'feed_multi_controller_slots')+'\n'
program+='static void initialize(int32_t game_pad_handle,int32_t game_pad_index,const int32_t *slot_handles,const uint32_t *slot_dualsense,const uint32_t *slot_user_matches){\n'+initialization+r'''
 game=args;game.fp_state_internal=(intptr_t)native_state;game.read_state_ext_address=(intptr_t)native_state_ext;
 game.fp_read_internal=(intptr_t)native_read;game.read_ext_address=(intptr_t)native_read_ext;game.fp_data_internal=(intptr_t)native_state;
 g_game_bridge_direct_args=(intptr_t)&game;
 game.remote_block=(intptr_t)&game;game.remote_block_size=sizeof game;
}
static unsigned selected_primary(void){
 int game_pid=42;intptr_t bridge_args=(intptr_t)&game;PoorDS4GameBridgeStatus initial_status={0};
'''+decision+r'''
 return primary_slot;
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static int read(int kind,int h,ScePadData *out){
 memset(out,0,sizeof *out);
 switch(kind){case 0:return game_pad_read_state_stub(h,out,&game);case 1:return game_pad_read_state_ext_stub(h,out,&game);case 2:return game_pad_read_stub(h,out,1,&game);case 3:return game_pad_read_ext_stub(h,out,1,&game);default:return game_pad_get_data_internal_stub(h,out,&game);}
}
static int recorded_selection(void){
 for(int user=0;user<2;user++)for(int source_index=0;source_index<8;source_index++){
  int32_t h=-1,index=-1,handles[4];uint32_t sony[4],matches[4];
  int32_t uid=user?22:11;
  int base=user?4:2;int expected=(source_index==base+1)?base+1:base;
  uint32_t expected_handle=user?(expected==4?0x03280704:0x03290705):(expected==2?0x03240702:0x03250703);
  CHECK(game_bridge_select_pad_handle(42,table_address-0x20018,1,uid,source_index,-1,&h,&index,handles,sony,matches)==0);
  if(h!=(int32_t)expected_handle || index!=expected)fprintf(stdout,"recorded user=0x%08x modeled source_index=%d selected=0x%08x raw_index=%d expected_index=%d\n",(unsigned)uid,source_index,(unsigned)h,index,expected);
  CHECK(h==(int32_t)expected_handle && index==expected);
 }
 return 0;
}
static int initialized_high_primary(int source_index){
 int32_t h=-1,index=-1,handles[4];uint32_t sony[4],matches[4];
 /* Source index zero selects recorded raw index four in the existing selector.
  * Source index one is then used independently to test actual publication
  * once selection is corrected; this is explicitly a modeled source order. */
 CHECK(game_bridge_select_pad_handle(42,table_address-0x20018,1,22,0,-1,&h,&index,handles,sony,matches)==0);
 CHECK(h==0x03280704 && index==4);
 initialize(h,index,handles,sony,matches);g_pad_source.pad_index=source_index;
 unsigned chosen=selected_primary();CHECK(chosen<4);
 CHECK(wireless_ds4_game_bridge_update_slot(42,(intptr_t)&game,chosen,&primary,sizeof primary,0,0)==0);
 ScePadData out;
 for(int kind=0;kind<5;kind++){
  CHECK(read(kind,0x03200700,&out)>=0 && !memcmp(&out,&native,sizeof out));
  CHECK(wireless_ds4_game_bridge_update_slot(42,(intptr_t)&game,chosen,&primary,sizeof primary,0,0)==0);
  CHECK(read(kind,h,&out)>=0 && !memcmp(&out,&primary,sizeof out));
 }
 return 0;
}
static int recorded_secondary_feed(int reverse){
 g_reader_users[0]=reverse?22:11;g_reader_users[1]=reverse?11:22;
 g_reader_users[2]=g_reader_users[3]=-1;reader_available_mask=14;
 int32_t h=-1,index=-1,handles[4];uint32_t sony[4],matches[4];
 CHECK(game_bridge_select_pad_handle(42,table_address-0x20018,1,g_reader_users[0],0,-1,&h,&index,handles,sony,matches)==0);
 CHECK(h==(reverse?0x03280704:0x03240702) && index==(reverse?4:2));
 int32_t other=reverse?0x03240702:0x03280704,alias=reverse?0x05580702:0x05580704;
 initialize(h,index,handles,sony,matches);g_pad_source.pad_index=0;
 unsigned chosen=selected_primary();CHECK(chosen<4);
 CHECK(wireless_ds4_game_bridge_update_slot(42,(intptr_t)&game,chosen,&primary,sizeof primary,0,0)==0);
 PoorDS4GameBridgeStatus status={0};CHECK(wireless_ds4_game_bridge_status(42,(intptr_t)&game,&status)==0);
 feed_multi_controller_slots(99,1,42,(intptr_t)&game,1,chosen,&status,&primary);
 ScePadData out;
 for(int kind=0;kind<5;kind++){
  CHECK(read(kind,0x03200700,&out)>=0 && !memcmp(&out,&native,sizeof out));
  CHECK(read(kind,other,&out)>=0 && !memcmp(&out,&secondary,sizeof out));
  /* Publish a new independently attributed packet before each queued read. */
  feed_multi_controller_slots(99,1,42,(intptr_t)&game,2,chosen,&status,&primary);
  CHECK(read(kind,alias,&out)>=0 && !memcmp(&out,&secondary,sizeof out));
  feed_multi_controller_slots(99,1,42,(intptr_t)&game,3,chosen,&status,&primary);
 }
 reader_available_mask=12;feed_multi_controller_slots(99,1,42,(intptr_t)&game,4,chosen,&status,&primary);
 CHECK(read(0,other,&out)==0 && !out.connected);
 CHECK(read(0,h,&out)==0 && !memcmp(&out,&primary,sizeof out));
 return 0;
}
#if defined(HAS_READER_COLLECTOR)
static int modeled_capacity_and_ambiguity(void){
 for(int mode=0;mode<2;mode++){
  /* Additional entries are explicit models, not new recorded console bytes. */
  GameBridgeReaderClient clients[8];int32_t handles[4],users[4];uint32_t sony[4]={0},matches[4]={0},eligible[4];
  for(int c=0;c<8;c++){
   int user=mode?2+c/2:(c<2?9:1+c/2);
   clients[c]=(GameBridgeReaderClient){0x04000700|c,c,user,(mode || c!=0)?1u:0u};
   if(c<4){handles[c]=clients[c].handle;users[c]=user;eligible[c]=clients[c].eligible;matches[c]=user==2;}
  }
  if(!mode)sony[0]=1;
  g_reader_users[0]=2;g_reader_users[1]=3;g_reader_users[2]=4;g_reader_users[3]=mode?5:-1;
  int raw_primary=mode?0:2;
  game_bridge_collect_reader_clients(clients,8,clients[raw_primary].handle,raw_primary,2,handles,sony,matches,users,eligible);
  initialize(clients[raw_primary].handle,raw_primary,handles,sony,matches);
  native_present=!mode;reader_available_mask=14;g_pad_source.pad_index=0;
  unsigned chosen=selected_primary();CHECK(chosen==(unsigned)raw_primary);
  CHECK(wireless_ds4_game_bridge_update_slot(42,(intptr_t)&game,chosen,&primary,sizeof primary,0,0)==0);
  PoorDS4GameBridgeStatus status={0};CHECK(wireless_ds4_game_bridge_status(42,(intptr_t)&game,&status)==0);
  for(int kind=0;kind<5;kind++){
   feed_multi_controller_slots(99,1,42,(intptr_t)&game,1,chosen,&status,&primary);
   ScePadData out;
   if(!mode)CHECK(read(kind,clients[0].handle,&out)>=0 && !memcmp(&out,&native,sizeof out));
   CHECK(wireless_ds4_game_bridge_update_slot(42,(intptr_t)&game,chosen,&primary,sizeof primary,0,0)==0);
   CHECK(read(kind,clients[raw_primary].handle,&out)>=0 && !memcmp(&out,&primary,sizeof out));
   for(int reader=1;reader<(mode?4:3);reader++){
    int raw=mode?reader*2:(reader+1)*2;
    const ScePadData *expected=reader==1?&secondary:reader==2?&tertiary:&quaternary;
    CHECK(read(kind,clients[raw].handle,&out)>=0 && !memcmp(&out,expected,sizeof out));
   }
  }
  /* Removing reader one cannot borrow another reader's data or stop others. */
  reader_available_mask=12;feed_multi_controller_slots(99,1,42,(intptr_t)&game,2,chosen,&status,&primary);
  ScePadData out;int lost=mode?2:4;int retained=mode?4:6;
  CHECK(read(0,clients[lost].handle,&out)==0 && !out.connected);
  CHECK(read(0,clients[retained].handle,&out)==0 && !memcmp(&out,&tertiary,sizeof out));
 }
 /* Two readers with the same user are ambiguous and must remain unassigned. */
 GameBridgeReaderClient clients[2]={{0x04000700,0,2,1},{0x04000704,4,3,1}};
 int32_t handles[4]={clients[0].handle,-1,-1,-1},users[4]={2,-1,-1,-1};
 uint32_t sony[4]={0},matches[4]={1,0,0,0},eligible[4]={1,0,0,0};
 g_reader_users[0]=2;g_reader_users[1]=3;g_reader_users[2]=3;g_reader_users[3]=-1;
 game_bridge_collect_reader_clients(clients,2,clients[0].handle,0,2,handles,sony,matches,users,eligible);
 for(int slot=0;slot<4;slot++)CHECK(g_game_reader_slots[slot]<0);
 return 0;
}
#endif
int main(void){
 memset(&native,0,sizeof native);native.connected=1;native.buttons=0x20;native.leftStick.x=17;native.rightStick.y=19;native.timestamp=1234;native.count=1;
 memset(&primary,0,sizeof primary);primary.connected=1;primary.buttons=0x4000;primary.leftStick.x=199;primary.rightStick.y=211;primary.analogButtons.l2=117;primary.timestamp=5678;primary.count=1;
 secondary=primary;secondary.buttons=0x8000;secondary.leftStick.x=31;secondary.rightStick.y=47;secondary.analogButtons.l2=231;secondary.timestamp=9012;
 tertiary=secondary;tertiary.buttons=0x1000;tertiary.leftStick.x=91;tertiary.analogButtons.l2=15;tertiary.timestamp=11001;
 quaternary=secondary;quaternary.buttons=0x2000;quaternary.leftStick.x=131;quaternary.analogButtons.l2=47;quaternary.timestamp=12001;
 int failed=recorded_selection();
 for(int source_index=0;source_index<8;source_index++)failed+=initialized_high_primary(source_index);
 failed+=recorded_secondary_feed(0)+recorded_secondary_feed(1);
#if defined(HAS_READER_COLLECTOR)
 failed+=modeled_capacity_and_ambiguity();
#endif
 printf("production selection/init/full-status/primary and secondary publication with reported layout, generation aliases and five gateways; native DS5 bytes preserved; failed=%d\n",failed);
 return failed!=0;
}
'''
compiler=Path(os.environ.get('POORDS4_HOST_CC','gcc'))
with tempfile.TemporaryDirectory(prefix='poords4-recorded-client-') as t:
 path=Path(t);(path/'test.c').write_text(program)
 compiled=subprocess.run([str(compiler),'-std=c11','-O2','-I',str(payload),str(path/'test.c'),'-o',str(path/'test.exe')],capture_output=True,text=True)
 result=subprocess.run([str(path/'test.exe')],capture_output=True,text=True) if compiled.returncode==0 else compiled
 print(json.dumps({'compiled':compiled.returncode==0,'passed':compiled.returncode==0 and result.returncode==0,'output':result.stdout,'error':result.stderr,
 'sources':{n:hashlib.sha256((payload/n).read_bytes()).hexdigest() for n in ('wireless_ds4.c','game_pad_bridge_main.c')},'recorded_bytes':recorded,
 'model':'Reported 1152-byte record layout; synthetic table rebuilt from five routing fields with sample user identifiers and zero padding; discovery/Sony callbacks mocked; source ordering modeled; mixed 1-DS5/3-DS4 and four-DS4 capacity entries are explicit additional host models',
 'physical_join_orders_verified':False},indent=2))
 raise SystemExit(0 if compiled.returncode==0 and result.returncode==0 else 1)
