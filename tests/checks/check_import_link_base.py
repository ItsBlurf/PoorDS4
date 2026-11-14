# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay ordinary imports of an executable linked above zero through the actual collector.

prospero-win's title shell (used by Playden and PC5 games) links its eboot at
0x100000000. rtld maps a module's segments at mapbase + p_vaddr, so the slots
of that eboot are at mapbase + r_offset but beyond mapbase + mapsize; they lie
in the segments rtld lists for the object. Host model of the PS5 rtld object;
the layout was observed on firmware 13.60 (eboot mapbase 0x400000, mapsize
0x1d0000, data segment at 0x100440000).
"""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tests'))
from import_scan_fixture import scanner_program
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--payload',type=Path,default=ROOT/'payload');payload=p.parse_args().payload
s=(payload/'wireless_ds4.c').read_text(encoding='utf-8-sig')
body=r'''
static GameBridgeSharedLibSection segments[2];
static const char *import_name="scePadReadState";
static uint64_t listed_count=2;
static int case_scan(uint64_t link,int listed,uint64_t offset,uint64_t segment_length,uint32_t *n,GameBridgeImportHook *h){
 prepare(import_name);slots[0]=originals[0];slots[1]=originals[0];
 symbol.st_info=STT_FUNC;symbol.st_shndx=SHN_UNDEF;
 section.plt_rela=section.plt_rela_size=0;section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
 memset(ordinary,0,sizeof ordinary);ordinary[0].r_info=ELF64_R_INFO(0,R_X86_64_GLOB_DAT);ordinary[0].r_offset=offset;
 /* The eboot's text sits at mapbase + link; its slots are in a data segment. */
 object.mapbase=(uintptr_t)slots-link;object.mapsize=0x10;
 memset(segments,0,sizeof segments);
 segments[0].address=object.mapbase+link-0x40000;segments[0].length=0x1000;
 segments[1].address=(uintptr_t)slots;segments[1].length=segment_length;
 if(listed){object.sections=(uintptr_t)segments;object.section_count=listed_count;}
 *n=0;return scan(0,n,h);
}
int main(void){unsigned failures=0,checks=0;GameBridgeImportHook h[32];uint32_t n;
 const uint64_t high=UINT64_C(0x100000000);
 /* Zero-based module: slots inside [mapbase, mapbase + mapsize) as before. */
 checks++;if(case_scan(0,0,0,0,&n,h)||n!=1||h[0].slot!=(intptr_t)&slots[0]||h[0].kind!=0||outside_reads){fprintf(stderr,"zero base first slot n=%u\n",n);failures++;}
 checks++;if(case_scan(0,0,8,0,&n,h)||n!=1||h[0].slot!=(intptr_t)&slots[1]||outside_reads){fprintf(stderr,"zero base second slot n=%u\n",n);failures++;}
 /* Linked at 0x100000000: slots past mapsize, inside a listed segment. */
 checks++;if(case_scan(high,1,high,sizeof slots,&n,h)||n!=1||h[0].slot!=(intptr_t)&slots[0]||h[0].kind!=0||outside_reads){fprintf(stderr,"high base first slot n=%u\n",n);failures++;}
 checks++;if(case_scan(high,1,high+8,sizeof slots,&n,h)||n!=1||h[0].slot!=(intptr_t)&slots[1]||outside_reads){fprintf(stderr,"high base second slot n=%u\n",n);failures++;}
 /* Never a slot outside the map and every listed segment. */
 checks++;if(!case_scan(high,0,high,sizeof slots,&n,h)||n||outside_reads||!strstr(reports,"import_slot_skipped unmapped")){fprintf(stderr,"unlisted segment n=%u\n",n);failures++;}
 checks++;if(!case_scan(high,1,high+8,8,&n,h)||n||outside_reads||!strstr(reports,"import_slot_skipped unmapped")){fprintf(stderr,"past segment n=%u\n",n);failures++;}
 checks++;if(!case_scan(high,1,high+12,sizeof slots,&n,h)||n||outside_reads){fprintf(stderr,"partial slot n=%u\n",n);failures++;}
 checks++;if(!case_scan(high,1,UINT64_MAX-4,sizeof slots,&n,h)||n||outside_reads){fprintf(stderr,"wrapping offset n=%u\n",n);failures++;}
 /* A NID-named import of the high-linked module. */
 import_name="FAKE0000000#A#B";
 checks++;if(case_scan(high,1,high+8,sizeof slots,&n,h)||n!=1||h[0].slot!=(intptr_t)&slots[1]||h[0].kind!=0||outside_reads){fprintf(stderr,"high base NID n=%u\n",n);failures++;}
 import_name="scePadReadState";
 /* An implausible segment count is not walked. */
 listed_count=33;
 checks++;if(!case_scan(high,1,high,sizeof slots,&n,h)||n||outside_reads){fprintf(stderr,"segment count n=%u\n",n);failures++;}
 listed_count=2;
 printf("link base import checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
program='#define mdbg_copyout remote_reader_copyout\n'+scanner_program(s,body)
with tempfile.TemporaryDirectory(prefix='poords4-link-base-')as td:
 td=Path(td);c=td/'test.c';exe=td/'test.exe';c.write_text(program)
 cc=os.environ.get('POORDS4_HOST_CC','gcc')
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra',str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
 result={'compiled':not build.returncode,'passed':bool(run and not run.returncode),'output':run.stdout.strip()if run else '', 'error':run.stderr if run else build.stderr[-4000:],'source_sha256':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest(),'native_import_admission_verified':False}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
