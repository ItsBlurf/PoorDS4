# SPDX-License-Identifier: GPL-3.0-or-later
"""Stress the actual per-player cache publication/snapshot with host threads.

No Sony calls or console contact. Accepted snapshots must contain one complete
two-trigger request; failed bounded snapshots/publications are counted.
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--payload',type=Path,default=ROOT/'payload');payload=p.parse_args().payload
program=r'''
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include "trigger_effect_state.h"
static PoorDS4TriggerCache cache[4];
static unsigned done[4],accepted[4],bad[4],refused[4],published[4];
static void *writer(void *arg){
 unsigned slot=(unsigned)(uintptr_t)arg;
 for(unsigned i=0;i<30000;i++){
  PoorDS4TriggerParam p={0};p.mask=3;uint8_t value=(uint8_t)(i+slot*37);
  for(unsigned side=0;side<2;side++){
   p.commands[side].mode=4;
   for(unsigned byte=0;byte<48;byte++)p.commands[side].data[byte]=side?(uint8_t)(value^0xaa):value;
  }
  if(poords4_trigger_publish(&cache[slot],&p))refused[slot]++;else published[slot]++;
 }
 __atomic_store_n(&done[slot],1,__ATOMIC_RELEASE);return 0;
}
static void *reader(void *arg){
 unsigned slot=(unsigned)(uintptr_t)arg;
 do{
  PoorDS4TriggerParam p;
  if(poords4_trigger_snapshot(&cache[slot],&p)){
   accepted[slot]++;uint8_t value=p.commands[0].data[0];
   if(p.mask!=3 || p.commands[0].mode!=4 || p.commands[1].mode!=4)bad[slot]++;
   for(unsigned byte=0;byte<48;byte++)
    if(p.commands[0].data[byte]!=value || p.commands[1].data[byte]!=(uint8_t)(value^0xaa))bad[slot]++;
  }
 }while(!__atomic_load_n(&done[slot],__ATOMIC_ACQUIRE));return 0;
}
int main(void){
 pthread_t w[4],r[4];
 for(unsigned s=0;s<4;s++){
  PoorDS4TriggerParam empty;
  if(poords4_trigger_snapshot(&cache[s],&empty))return 3;
  /* An unpublished cache has no request. Seed a
   * complete request before testing concurrent request consistency. */
  PoorDS4TriggerParam seed={0};seed.mask=3;
  for(unsigned side=0;side<2;side++){
   seed.commands[side].mode=4;
   for(unsigned b=0;b<48;b++)seed.commands[side].data[b]=side?0xaa:0;
  }
  if(poords4_trigger_publish(&cache[s],&seed))return 4;
 }
 for(unsigned s=0;s<4;s++){if(pthread_create(&r[s],0,reader,(void*)(uintptr_t)s)||pthread_create(&w[s],0,writer,(void*)(uintptr_t)s))return 2;}
 unsigned failed=0;
 for(unsigned s=0;s<4;s++){pthread_join(w[s],0);pthread_join(r[s],0);if(!accepted[s]||bad[s]||!published[s])failed=1;}
 unsigned a=0,b=0,p=0,f=0;for(unsigned s=0;s<4;s++){a+=accepted[s];b+=bad[s];p+=published[s];f+=refused[s];}
 printf("players=4 publications=%u snapshots=%u torn=%u bounded_write_refusals=%u\n",p,a,b,f);return failed;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-effect-concurrency-')as td:
 td=Path(td);c=td/'test.c';exe=td/'test.exe';c.write_text(program)
 cc=os.environ.get('POORDS4_HOST_CC','gcc')
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-pthread','-I',str(payload),str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)if not build.returncode else None
 result={'compiled':not build.returncode,'passed':bool(run and not run.returncode),'output':run.stdout.strip()if run else '', 'error':run.stderr if run else build.stderr[-3000:],'header_sha256':hashlib.sha256((payload/'trigger_effect_state.h').read_bytes()).hexdigest(),'scope':'Actual independent cache helpers, four concurrent publisher/reader pairs; host scheduling only.'}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
