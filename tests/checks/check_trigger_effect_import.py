# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay name/NID setter imports through the actual collector in a host model."""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tests'))
from import_scan_fixture import scanner_program
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--payload',type=Path,default=ROOT/'payload');payload=p.parse_args().payload
s=(payload/'wireless_ds4.c').read_text(encoding='utf-8-sig')
body=r'''
int main(void){unsigned failures=0,checks=0;GameBridgeImportHook h[32];uint32_t n;
 const char *names[]={"scePadSetTriggerEffect","2JgFB2n9oUM","scePadSetTriggerEffect#Pad#1","2JgFB2n9oUM#Pad#1"};
 for(unsigned k=0;k<4;k++)for(unsigned table_kind=0;table_kind<2;table_kind++){
  prepare(names[k]);slots[0]=originals[10];symbol.st_info=STT_FUNC;symbol.st_shndx=SHN_UNDEF;
  relocation.r_info=ELF64_R_INFO(0,table_kind?R_X86_64_GLOB_DAT:R_X86_64_JMP_SLOT);
  if(table_kind){section.plt_rela=section.plt_rela_size=0;section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];ordinary[0]=relocation;}
  checks++;if(scan(0,&n,h)||n!=1||h[0].kind!=10||outside_reads){fprintf(stderr,"name=%s ordinary=%u count=%u\n",names[k],table_kind,n);failures++;}
 }
 printf("setter import checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
'''
program='#define mdbg_copyout remote_reader_copyout\n'+scanner_program(s,body)
with tempfile.TemporaryDirectory(prefix='poords4-effect-import-')as td:
 td=Path(td);c=td/'test.c';exe=td/'test.exe';c.write_text(program)
 cc=os.environ.get('POORDS4_HOST_CC','gcc')
 build=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra',str(c),'-o',str(exe)],capture_output=True,text=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True)if not build.returncode else None
 result={'compiled':not build.returncode,'passed':bool(run and not run.returncode),'output':run.stdout.strip()if run else '', 'error':run.stderr if run else build.stderr[-4000:],'source_sha256':hashlib.sha256((payload/'wireless_ds4.c').read_bytes()).hexdigest(),'native_import_admission_verified':False}
 print(json.dumps(result,indent=2));sys.exit(0 if result['passed']else 1)
