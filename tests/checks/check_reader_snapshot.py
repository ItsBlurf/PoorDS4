# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual primary/secondary readers with non-atomic copy interleavings.

The producer/transport are explicit host memory models. A bulk copy has no
read-order/atomic-snapshot contract. No controller input is injected on PS5.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from fixture_support import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--payload',type=Path,default=ROOT/'payload')
p=parser.parse_args().payload
source=(p/'wireless_ds4.c').read_text()
start=source.rfind('typedef struct {',0,source.index('} RemotePadReaderArgs;'))
types=source[start:source.index('} RemotePadReaderArgs;')+len('} RemotePadReaderArgs;')]
program=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define __PROSPERO__ 1
#include "wireless_ds4.h"
#include "pad_types.h"
#define POORDS4_REMOTE_PAD_CAPACITY 256
'''+types+r'''
static RemotePadReaderArgs fixture;
static unsigned selected,mode,bulk_copies,seq_reads;
static ScePadData old_pad,new_pad;
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){
 intptr_t base=(intptr_t)&fixture;
 if(pid!=99)return -1;
 if(address==base && size==sizeof fixture){
  bulk_copies++;
  if(mode==4)return -1;
  if((mode==1 || mode==5) && (bulk_copies==1 || mode==5)){
   /* A legal copy reads the old data first, the producer publishes a new
    * frame, then the copy reads the new header. Copy ordering is unspecified. */
   RemotePadReaderArgs before=fixture;
   if(selected==0){fixture.seq+=2;memcpy(fixture.pad_data,&new_pad,sizeof new_pad);}
   else{fixture.slot_seqs[selected]+=2;memcpy(fixture.slot_data[selected],&new_pad,sizeof new_pad);}
   memcpy(out,&fixture,sizeof fixture);
   if(selected==0)memcpy(((RemotePadReaderArgs*)out)->pad_data,before.pad_data,POORDS4_REMOTE_PAD_CAPACITY);
   else memcpy(((RemotePadReaderArgs*)out)->slot_data[selected],before.slot_data[selected],POORDS4_REMOTE_PAD_CAPACITY);
  }else memcpy(out,&fixture,sizeof fixture);
  return 0;
 }
 intptr_t seq_address=base+(selected==0?(intptr_t)offsetof(RemotePadReaderArgs,seq):
    (intptr_t)offsetof(RemotePadReaderArgs,slot_seqs)+(intptr_t)selected*sizeof(uint32_t));
 if(address==seq_address && size==sizeof(uint32_t)){
  seq_reads++;
  if(mode==3 && bulk_copies)return -1;
  memcpy(out,selected==0?(const void*)&fixture.seq:(const void*)&fixture.slot_seqs[selected],size);return 0;
 }
 return -1;
}
'''+ 'int '+function(source,'wireless_ds4_remote_reader_read')+'\n'+'int '+function(source,'wireless_ds4_remote_reader_read_slot')+r'''
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d check=%s\n",__LINE__,#x);return 1;}}while(0)
static void setup(unsigned slot,unsigned interleave){
 memset(&fixture,0,sizeof fixture);selected=slot;mode=interleave;bulk_copies=seq_reads=0;
 fixture.ready=1;fixture.pad_handle=44;fixture.seq=2;fixture.last_result=0;
 fixture.slot_handles[slot]=44+(int32_t)slot;fixture.slot_results[slot]=0;fixture.slot_seqs[slot]=2;
 memset(&old_pad,0,sizeof old_pad);old_pad.connected=1;old_pad.count=17;old_pad.buttons=0x40;old_pad.timestamp=1000;old_pad.leftStick.x=37;
 new_pad=old_pad;new_pad.count=18;new_pad.buttons=0x80;new_pad.timestamp=2000;new_pad.leftStick.x=211;
 memcpy(fixture.slot_data[slot],&old_pad,sizeof old_pad);
 memcpy(fixture.pad_data,&old_pad,sizeof old_pad);
}
static int interleaved_frame(void){
 for(unsigned slot=0;slot<4;slot++){
  setup(slot,1);ScePadData out={0};uint32_t seq=0;
  CHECK(wireless_ds4_remote_reader_read_slot(99,(intptr_t)&fixture,slot,&out,sizeof out,&seq)==0);
  if(memcmp(&out,&new_pad,sizeof out))fprintf(stderr,"slot=%u accepted_seq=%u returned_count=%u expected_count=%u returned_buttons=0x%x expected_buttons=0x%x bulk_copies=%u\n",slot,seq,out.count,new_pad.count,out.buttons,new_pad.buttons,bulk_copies);
  CHECK(seq==4 && !memcmp(&out,&new_pad,sizeof out) && bulk_copies>=2);
 }
 return 0;
}
static int controls(void){
 for(unsigned slot=0;slot<4;slot++){
  setup(slot,0);ScePadData out={0};uint32_t seq=0;
  CHECK(wireless_ds4_remote_reader_read_slot(99,(intptr_t)&fixture,slot,&out,sizeof out,&seq)==0 && seq==2 && !memcmp(&out,&old_pad,sizeof out));
  for(unsigned bad=0;bad<7;bad++){
   setup(slot,0);memset(&out,0xa5,sizeof out);
   if(bad==0){fixture.slot_seqs[slot]=3;fixture.seq=3;}
   if(bad==1){fixture.slot_seqs[slot]=0;fixture.seq=0;}
   if(bad==2)fixture.ready=2;
   if(bad==3){fixture.slot_handles[slot]=-1;if(slot==0)fixture.ready=0;}
   if(bad==4){fixture.slot_results[slot]=-1;fixture.last_result=-1;}
   if(bad==5)mode=3;
   if(bad==6)mode=4;
   CHECK(wireless_ds4_remote_reader_read_slot(99,(intptr_t)&fixture,slot,&out,sizeof out,&seq)==-1);
   CHECK(out.count==0xa5 && bulk_copies<=3); /* No failed packet is published. */
  }
  setup(slot,5);CHECK(wireless_ds4_remote_reader_read_slot(99,(intptr_t)&fixture,slot,&out,sizeof out,&seq)==-1 && bulk_copies==3);
 }
 setup(0,0);ScePadData out={0};uint32_t seq=0;
 CHECK(wireless_ds4_remote_reader_read_slot(99,(intptr_t)&fixture,0,&out,sizeof out,&seq)==0 && seq==2 && bulk_copies==1);
 CHECK(wireless_ds4_remote_reader_read_slot(99,(intptr_t)&fixture,4,&out,sizeof out,&seq)==-1);
 return 0;
}
int main(void){int failed=interleaved_frame()+controls();
 printf("actual primary and secondary readers: stale bulk-data/new-header interleave, all four sources, stable/odd/zero/stopped/absent/error/copy-failure/bounded-race controls and primary delegation; failed=%d\n",failed);return failed?1:0;}
'''
cc=os.environ.get('POORDS4_HOST_CC','gcc')
with tempfile.TemporaryDirectory(prefix='secondary-snapshot-') as tmp:
 c,exe=Path(tmp)/'test.c',Path(tmp)/'test.exe';c.write_text(program)
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(p),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True) if not build.returncode else None
 passed=run is not None and run.returncode==0
 print(json.dumps({'compiled':not build.returncode,'passed':passed,'output':run.stdout if run else '',
  'error':run.stderr if run else build.stderr[-4500:],'source':hashlib.sha256((p/'wireless_ds4.c').read_bytes()).hexdigest(),
  'scope':'Actual primary and secondary readers; explicit legal host copy interleavings, no native race/physical controller claim'},indent=2))
sys.exit(0 if passed else 1)
