# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual RC51/recovery collector bodies with an explicit host metadata model."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from import_scan_fixture import scanner_program
from fixture_support import function

parser = argparse.ArgumentParser()
parser.add_argument('source_root', type=Path)
args = parser.parse_args()
source = (args.source_root / 'payload/wireless_ds4.c').read_text(encoding='utf-8')
cc = os.environ.get('POORDS4_HOST_CC', 'gcc')

# The host model describes the published SDK metadata, including ordinary
# relocations, even when the old collector only knows its shorter prefix.
# Only this model declaration is extended; old production function bodies stay
# unchanged. This proves discovery decisions, not a native memory-read contract.
model = source
end = model.index('} GameBridgeDynlibSectionPrefix;')
start = model.rfind('typedef struct {', 0, end)
section = model[start:end]
if 'uint64_t rela;' not in section:
    extended = section.replace('    uint64_t plt_rela_size;\n',
                               '    uint64_t plt_rela_size;\n    uint64_t rela;\n    uint64_t rela_size;\n')
    model = model[:start] + extended + model[end:]

cases = {
 'rumble_imports': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    for(unsigned kind=8;kind<10;kind++){
      const char *names[]={fixture_names[kind],kind==8?"FAKE0000008#libScePad":"FAKE0000009#libScePad"};
      for(unsigned i=0;i<2;i++){
        prepare(names[i]);slots[0]=originals[kind];section.plt_rela=0;section.plt_rela_size=0;
        ordinary[0]=relocation;ordinary[0].r_info=ELF64_R_INFO(0,R_X86_64_GLOB_DAT);
        section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
        CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==kind&&!outside_reads);
      }
    }return 0;}''',
 'trigger_effect_state_import': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    const char *names[]={"scePadGetTriggerEffectState#libScePad","znaWI0gpuo8#libScePad"};
    for(unsigned i=0;i<2;i++){
      prepare(names[i]);slots[0]=originals[7];section.plt_rela=0;section.plt_rela_size=0;
      ordinary[0]=relocation;ordinary[0].r_info=ELF64_R_INFO(0,R_X86_64_GLOB_DAT);
      section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
      CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==7&&!outside_reads);
    }
    return 0;}''',
 'legacy_plt': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    for(unsigned i=0;i<7;i++){char name[64];snprintf(name,sizeof name,"FAKE%07u#module",i);
      prepare(name);slots[0]=originals[i];
      CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==i&&!outside_reads);}
    return 0;}''',
 'legacy_plt_invalid_optional_table': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    prepare("FAKE0000000#module");section.rela=1;section.rela_size=sizeof(Elf64_Rela);
    CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==0&&!outside_reads);
    prepare("FAKE0000000#module");section.rela=(uintptr_t)ordinary;section.rela_size=1;
    CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==0&&!outside_reads);
    return 0;}''',
 'mixed_plt_and_nonplt': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    prepare("FAKE0000000#module");Elf64_Sym symbols[2]={symbol,symbol};
    unsigned second=(unsigned)strlen(table)+1;strcpy(table+second,"FAKE0000002#module");
    symbols[1].st_name=second;section.symtab=(uintptr_t)symbols;section.symtab_size=sizeof symbols;
    section.strtab_size=second+strlen(table+second)+1;slots[1]=originals[2];
    ordinary[0]=(Elf64_Rela){.r_offset=sizeof(uint64_t),.r_info=ELF64_R_INFO(1,R_X86_64_GLOB_DAT)};
    section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
    CHECK(!scan(0,&n,h)&&n==2&&h[0].kind==0&&h[1].kind==2&&!outside_reads);
    return 0;}''',
 'emulator_nonplt': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    for(unsigned i=0;i<7;i++){prepare(fixture_names[i]);slots[0]=originals[i];
      section.plt_rela=section.plt_rela_size=0;
      ordinary[0]=(Elf64_Rela){.r_info=ELF64_R_INFO(0,R_X86_64_GLOB_DAT)};
      section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
      CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==i&&!outside_reads);}
    return 0;}''',
 'published_eden_input_imports': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    const char *names[]={"YndgXqQVV7c#A#B","q1cHNfGycLI#A#B"};
    const unsigned kinds[]={0,2};
    for(unsigned i=0;i<2;i++){prepare(names[i]);slots[0]=originals[kinds[i]];
      section.plt_rela=section.plt_rela_size=0;
      ordinary[0]=(Elf64_Rela){.r_info=ELF64_R_INFO(0,R_X86_64_GLOB_DAT)};
      section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
      CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==kinds[i]&&!outside_reads);}
    return 0;}''',
 'name_identity': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    for(unsigned i=0;i<7;i++){prepare(fixture_names[i]);slots[0]=0x2222;
      CHECK(!scan(1,&n,h)&&n==1&&h[0].kind==i&&!outside_reads);
      CHECK(scan(0,&n,h)==-1&&n==0&&!outside_reads);}
    prepare("FAKE0000000evil");slots[0]=0x2222;
    CHECK(scan(1,&n,h)==-1&&n==0&&!outside_reads);return 0;}''',
 'sdk_symbol_padding': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    prepare("FAKE0000000#module");section.symtab_size=sizeof symbol+16;
    CHECK(!scan(0,&n,h)&&n==1&&h[0].kind==0&&!outside_reads);return 0;}''',
 'data_and_relocation_rejection': r'''int main(void){GameBridgeImportHook h[32];uint32_t n;
    prepare("scePadReadState");section.plt_rela=section.plt_rela_size=0;
    ordinary[0]=(Elf64_Rela){.r_info=ELF64_R_INFO(0,R_X86_64_GLOB_DAT)};
    section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];
    symbol.st_info=STT_OBJECT;CHECK(scan(0,&n,h)==-1&&n==0);
    symbol.st_info=STT_FUNC;symbol.st_shndx=1;CHECK(scan(0,&n,h)==-1&&n==0);
    symbol.st_shndx=SHN_UNDEF;ordinary[0].r_info=ELF64_R_INFO(0,R_X86_64_RELATIVE);
    CHECK(scan(0,&n,h)==-1&&n==0&&!outside_reads);return 0;}'''
}
results = {}
app_info_type = '''typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} GameBridgeAppInfo;
'''
app_guard = ''
if 'game_bridge_is_native_ps4(' in source:
    end = source.index('} GameBridgeAppInfo;') + len('} GameBridgeAppInfo;')
    start = source.rfind('typedef struct {', 0, end)
    app_info_type = source[start:end] + '\n'
    app_guard = 'static int\n' + function(source, 'game_bridge_is_native_ps4') + '\n'
readiness = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int pid_t;
#define __PROSPERO__ 1
#define POORDS4_GAME_BRIDGE_NATIVE_PS4 (-5)
static const char *selected;
static const char *app_title = "PPSA99008";
static int app_result, lib_queries;
''' + app_info_type + r'''
static int sceKernelGetAppInfo(pid_t pid,GameBridgeAppInfo *info) {
    if(pid!=154)return -1;
    strncpy(info->title_id,app_title,sizeof(info->title_id));
    return app_result;
}
static size_t find_pids(const char *name,pid_t *pids,size_t capacity) {
    if(strcmp(name,"eboot.bin") || !capacity)return 0;
    pids[0]=154;return 1;
}
static int get_lib_quiet(pid_t pid,const char *name,uint32_t *handle) {
    lib_queries++;
    if(pid!=154)return -1;
    if(!strcmp(name,"libScePad") || !strcmp(name,"libSceLibcInternal") ||
       (selected && !strcmp(name,selected))){*handle=1;return 0;}
    return -1;
}
''' + app_guard + 'int\n' + function(source,'wireless_ds4_game_bridge_find_target') + r'''
int main(void) {
    const char *variants[]={"libkernel_sys","libkernel","libkernel_web"};
    for(unsigned i=0;i<3;i++){pid_t pid=-1;selected=variants[i];
      if(wireless_ds4_game_bridge_find_target(&pid)!=1 || pid!=154)return 1+i;}
    selected=0;pid_t pid=-1;
    if(wireless_ds4_game_bridge_find_target(&pid)!=-4)return 4;
    return 0;
}
'''
ps4_readiness = readiness[:readiness.index('int main(void)')] + r'''
int main(void) {
    pid_t pid=-1;selected="libkernel_sys";
    const char *native_titles[]={"CUSA08809","CUSA00001","CUSA99999"};
    for(unsigned i=0;i<3;i++) {
        app_title=native_titles[i];lib_queries=0;
        if(wireless_ds4_game_bridge_find_target(&pid)!=POORDS4_GAME_BRIDGE_NATIVE_PS4 ||
           pid!=154 || lib_queries)return 1+i;
    }
    const char *other_titles[]={"PPSA99008","CUSA","CUSA0123x","CUSA01234X","NPXS40172",""};
    for(unsigned i=0;i<6;i++) {
        app_title=other_titles[i];lib_queries=0;
        if(wireless_ds4_game_bridge_find_target(&pid)!=1 || !lib_queries)return 10+i;
    }
    app_title="CUSA08809";app_result=-1;lib_queries=0;
    if(wireless_ds4_game_bridge_find_target(&pid)!=1 || !lib_queries)return 20;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='poords4-rc51-fixtures-') as temporary:
    folder = Path(temporary)
    programs = {name: '#define mdbg_copyout remote_reader_copyout\n' + scanner_program(model,body)
                for name,body in cases.items()}
    programs['published_eden_input_imports'] = programs['published_eden_input_imports'].replace(
        'snprintf(out,12,"FAKE%07u",i);',
        'if(i==0)strcpy(out,"YndgXqQVV7c");else if(i==2)strcpy(out,"q1cHNfGycLI");'
        'else snprintf(out,12,"FAKE%07u",i);')
    programs['kernel_variants'] = readiness
    programs['native_ps4_passthrough'] = ps4_readiness
    for name, program in programs.items():
        c_file, exe = folder / (name + '.c'), folder / (name + '.exe')
        c_file.write_text(program, encoding='utf-8')
        build = subprocess.run([cc, '-std=c11', '-O2', '-Wall', str(c_file), '-o', str(exe)],
                                capture_output=True, text=True)
        if build.returncode:
            results[name] = {'compiled': False, 'passed': False, 'error': build.stderr[-1800:]}
        else:
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            results[name] = {'compiled': True, 'passed': run.returncode == 0,
                             'exit': run.returncode, 'output': run.stderr.strip()}
    print(json.dumps({'source_root': str(args.source_root), 'groups': results,
                      'native_input_verified': False}, indent=2))
sys.exit(0 if all(row['passed'] for row in results.values()) else 1)
