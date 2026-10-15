# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual neutral packet builder/wrapper/slot publisher with bounded host IO.

The fixture is supervisor-owned host memory, not console input. --payload tests
an authoritative frozen previous build without changing its sources.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload',type=Path,default=ROOT/'payload')
parser.add_argument('--connection-continuity',action='store_true',
                    help='Also verify defined neutral construction and retained connection metadata.')
opts=parser.parse_args();p=opts.payload
source=(p/'wireless_ds4.c').read_text();main=(p/'game_pad_bridge_main.c').read_text()
types=source[source.index('#define POORDS4_GAME_BRIDGE_MAGIC'):source.index('static pid_t g_game_bridge_direct_pid')]
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define __PROSPERO__ 1
#include "wireless_ds4.h"
#include "pad_types.h"
'''+types+r'''
static GamePadBridgeArgs fixture;
static pid_t g_game_bridge_direct_pid=94;
static intptr_t g_game_bridge_direct_args;
static uint32_t g_game_bridge_direct_seq,g_game_bridge_direct_packets;
static uint32_t g_game_bridge_slot_seq[4],g_game_bridge_slot_packets[4];
static uint64_t g_game_bridge_direct_cr3,g_game_bridge_direct_dmap;
static int writes,copy_fail,image_state;
int wireless_ds4_game_bridge_image_state(pid_t pid){(void)pid;return image_state;}
static int transfer(pid_t pid,intptr_t address,void *data,size_t size,int write){
 intptr_t base=(intptr_t)&fixture;
 if(pid!=94 || address<base || (uintptr_t)(address-base)>sizeof fixture || size>sizeof fixture-(size_t)(address-base))return -1;
 if(write){writes++;memcpy((uint8_t*)&fixture+(address-base),data,size);}else memcpy(data,(uint8_t*)&fixture+(address-base),size);
 return 0;
}
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){if(copy_fail)return -1;return transfer(pid,address,out,size,0);}
static int game_bridge_process_write(pid_t pid,intptr_t address,const void *data,size_t size){return transfer(pid,address,(void*)data,size,1);}
'''
program+='static int '+function(source,'game_bridge_copy_args')+'\n'
program+='int '+function(source,'wireless_ds4_game_bridge_update_slot')+'\n'
program+='int '+function(source,'wireless_ds4_game_bridge_update')+'\n'
program+='static void '+function(main,'make_neutral_connected_pad')+'\n'
program+=r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static void setup(unsigned primary){
 memset(&fixture,0,sizeof fixture);memset(g_game_bridge_slot_seq,0,sizeof g_game_bridge_slot_seq);memset(g_game_bridge_slot_packets,0,sizeof g_game_bridge_slot_packets);
 fixture.magic=POORDS4_GAME_BRIDGE_MAGIC;fixture.reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;fixture.pad_size=POORDS4_GAME_BRIDGE_PAD_SIZE;
 fixture.reserved0=primary;fixture.pad_handle=0x5000+(int32_t)primary;
 for(unsigned i=0;i<4;i++){
  fixture.slots[i].pad_handle=0x5000+(int32_t)i;fixture.slots[i].pad_index=(int32_t)i;
  fixture.slots[i].is_dualsense=i!=primary;fixture.slots[i].active=1;fixture.slots[i].reserved[0]=1;
  memset((void*)fixture.slots[i].direct_pad_data,(int)(0xa0+i),sizeof fixture.slots[i].direct_pad_data);
 }
 g_game_bridge_direct_pid=94;g_game_bridge_direct_args=(intptr_t)&fixture;
 writes=copy_fail=image_state=0;
}
static int all_primary_indices(void){
 for(unsigned primary=0;primary<4;primary++){
  setup(primary);GamePadBridgeSlot old[4];memcpy(old,fixture.slots,sizeof old);
  fixture.slots[primary].active=0; /* First publication is also legitimate. */
  ScePadData neutral;make_neutral_connected_pad(&neutral);
  CHECK(wireless_ds4_game_bridge_update(94,(intptr_t)&fixture,&neutral,sizeof neutral)==0);
  for(unsigned i=0;i<4;i++)if(i!=primary)CHECK(!memcmp(&old[i],&fixture.slots[i],sizeof old[i]));
  CHECK(fixture.slots[primary].active && !fixture.slots[primary].is_dualsense && !fixture.slots[primary].is_simulated);
  CHECK(fixture.slots[primary].direct_seq==1 && fixture.slots[primary].direct_packets==1);
  CHECK(!memcmp((const void*)fixture.slots[primary].direct_pad_data[1],&neutral,sizeof neutral));
  CHECK(writes==(primary==0?4:2));
 }
 return 0;
}
static int invalid_context(void){
 for(unsigned mode=0;mode<14;mode++){
  setup(1);ScePadData neutral;make_neutral_connected_pad(&neutral);
  if(mode==0)fixture.reserved0=4;if(mode==1)fixture.magic=0;if(mode==2)fixture.reserved1=0;
  if(mode==3)fixture.slots[1].is_dualsense=1;if(mode==4)fixture.slots[1].is_simulated=1;
  if(mode==5)fixture.slots[1].pad_handle=77;if(mode==6)copy_fail=1;if(mode==7)image_state=1;
  if(mode==8)g_game_bridge_direct_args=0;
  if(mode==9)fixture.pad_size=1;
  GamePadBridgeArgs old=fixture;
  CHECK(wireless_ds4_game_bridge_update(mode==10?93:94,mode==11?0:(intptr_t)&fixture,mode==13?NULL:&neutral,mode==12?119:sizeof neutral)==-1);
  CHECK(!writes && !memcmp(&old,&fixture,sizeof fixture));
 }
 return 0;
}
static int live_client_indices(void){
 for(unsigned primary=0;primary<4;primary++){
  setup(primary);fixture.slots[0].reserved[2]=POORDS4_CLIENT_BINDING_MAGIC;
  fixture.reserved0=8+primary;fixture.slots[primary].pad_index=(int32_t)fixture.reserved0;
  fixture.pad_handle=fixture.slots[primary].pad_handle=0x03000708+(int32_t)primary;
  GamePadBridgeSlot old[4];memcpy(old,fixture.slots,sizeof old);
  ScePadData neutral;make_neutral_connected_pad(&neutral);
  CHECK(wireless_ds4_game_bridge_update(94,(intptr_t)&fixture,&neutral,sizeof neutral)==0);
  for(unsigned i=0;i<4;i++)if(i!=primary)CHECK(!memcmp(&old[i],&fixture.slots[i],sizeof old[i]));
  CHECK(fixture.slots[primary].direct_packets==1);
 }
 return 0;
}
'''
if opts.connection_continuity:
 program+=r'''
static int initialized_neutral(void){
 ScePadData pad;memset(&pad,0xa5,sizeof pad);make_neutral_connected_pad(&pad);
 CHECK(pad.timestamp==0 && pad.count==0);
 CHECK(pad.connected==1 && pad.buttons==0 && pad.leftStick.x==128 && pad.rightStick.y==128);
 CHECK(!pad.analogButtons.l2 && !pad.analogButtons.r2 && pad.touchData.fingers==0);
 return 0;
}
static int retained_generation(void){
 unsigned cases=0;
 for(unsigned primary=0;primary<4;primary++)for(unsigned sample=0;sample<256;sample++)for(unsigned parity=1;parity<=2;parity++){
  unsigned generation=(sample+2)&255; /* Demonstrate count loss before the zero wrap control. */
  setup(primary);ScePadData previous={0},neutral={0};
  previous.connected=1;previous.count=(uint8_t)generation;previous.timestamp=UINT64_C(987654321);
  previous.buttons=0x40;previous.analogButtons.l2=255;previous.leftStick.x=17;
  fixture.slots[primary].direct_seq=parity;
  memcpy((void*)fixture.slots[primary].direct_pad_data[parity&1],&previous,sizeof previous);
  GamePadBridgeSlot old[4];memcpy(old,fixture.slots,sizeof old);
  make_neutral_connected_pad(&neutral);
  CHECK(wireless_ds4_game_bridge_update(94,(intptr_t)&fixture,&neutral,sizeof neutral)==0);
  ScePadData actual;memcpy(&actual,(const void*)fixture.slots[primary].direct_pad_data[fixture.slots[primary].direct_seq&1],sizeof actual);
  if(actual.count!=generation || actual.timestamp!=previous.timestamp)
   fprintf(stderr,"retained primary=%u generation=%u old_timestamp=%llu new_count=%u new_timestamp=%llu\n",primary,generation,(unsigned long long)previous.timestamp,actual.count,(unsigned long long)actual.timestamp);
  CHECK(actual.count==generation && actual.timestamp==previous.timestamp && actual.connected==1);
  CHECK(!actual.buttons && !actual.analogButtons.l2 && actual.leftStick.x==128 && actual.quat.w==1.0f);
  for(unsigned i=0;i<4;i++)if(i!=primary)CHECK(!memcmp(&old[i],&fixture.slots[i],sizeof old[i]));
  CHECK(neutral.count==0 && neutral.timestamp==0); /* No caller buffer mutation. */
  ScePadData reconnected=previous;reconnected.count=(uint8_t)(generation+1);reconnected.timestamp+=1000;reconnected.buttons=0x80;
  CHECK(wireless_ds4_game_bridge_update_slot(94,(intptr_t)&fixture,primary,&reconnected,sizeof reconnected,0,0)==0);
  memcpy(&actual,(const void*)fixture.slots[primary].direct_pad_data[fixture.slots[primary].direct_seq&1],sizeof actual);
  CHECK(!memcmp(&actual,&reconnected,sizeof actual)); /* Real reconnect adopts the new generation, including wrap to zero. */
  cases++;
 }
 printf("retained connection metadata: %u cases, both buffers, every count including zero, all primary indices; held controls neutralized\n",cases);
 return 0;
}
int main(void){int failed=all_primary_indices()+invalid_context()+live_client_indices()+initialized_neutral()+retained_generation();
 printf("neutral continuity and context checks failed=%d\n",failed);return failed?1:0;}
'''
else:
 program+=r'''
int main(void){int failed=all_primary_indices()+invalid_context()+live_client_indices();
 printf("actual neutral builder and producer: all four primary indices preserve other slots; invalid identity/native/simulated/context refuses without writes; failed=%d\n",failed);return failed?1:0;}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='primary-neutral-route-') as tmp:
 c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(p),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True) if not build.returncode else None
 passed=run is not None and run.returncode==0
 print(json.dumps({'compiled':not build.returncode,'passed':passed,'exit':run.returncode if run else None,'output':run.stdout.strip() if run else '',
 'error':run.stderr if run else build.stderr[:3500],'sources':{n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in ('wireless_ds4.c','wireless_ds4.h','game_pad_bridge_main.c')},
 'native_console_verified':False,'connection_continuity_cases':opts.connection_continuity,
 'transport':'bounded supervisor-owned host memory'},indent=2))
sys.exit(0 if passed else 1)
