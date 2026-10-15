# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the real import collector with explicitly modeled metadata transports.

No console access, physical pad evidence or proof of kernel pointer safety.
"""
from fixture_support import function
import re

def scanner_program(source, body):
    first=source.rfind('typedef struct {',0,source.index('} GameBridgeDynlibObjectPrefix;'))
    last=source.index('} GameBridgeImportHook;')+len('} GameBridgeImportHook;')
    types=source[first:last]
    helper=''
    if '\ngame_bridge_metadata_span_valid(' in source:
        helper+='static int\n'+function(source,'game_bridge_metadata_span_valid')+'\n'
    if '\ngame_bridge_read_import_name(' in source:
        helper+='static int\n'+function(source,'game_bridge_read_import_name')
    program = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
/* Explicit host ELF64 model; native compilation uses actual SDK definitions. */
typedef struct {uint32_t st_name;uint8_t st_info,st_other;uint16_t st_shndx;uint64_t st_value,st_size;} Elf64_Sym;
typedef struct {uint64_t r_offset,r_info;int64_t r_addend;} Elf64_Rela;
#define ELF64_R_SYM(value) ((value)>>32)
#define ELF64_R_TYPE(value) ((uint32_t)(value))
#define ELF64_ST_TYPE(value) ((value)&15)
#define STT_NOTYPE 0
#define STT_FUNC 2
#define STT_OBJECT 1
#define SHN_UNDEF 0
#define ELF64_R_INFO(symbol,type) (((uint64_t)(symbol)<<32)|(type))
#define R_X86_64_JMP_SLOT 7
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_RELATIVE 8
typedef int pid_t;
#define __PROSPERO__ 1
#define POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS 32
#define POORDS4_GAME_BRIDGE_API_COUNT 11u
'''+types+r'''
static GameBridgeDynlibObjectPrefix object;
static GameBridgeDynlibSectionPrefix section;
static Elf64_Sym symbol;
static Elf64_Rela relocation;
static Elf64_Rela ordinary[130];
static char table[256],path[192];
static uint64_t slots[2];
static intptr_t originals[11];
static unsigned name_reads,process_reads,outside_reads,rela_reads,symbol_reads;
typedef struct {uint64_t address;const void *bytes;size_t length;unsigned kind;} FixtureKernelRange;
static FixtureKernelRange fixture_kernel_ranges[4];
static unsigned fixture_kernel_range_count;
static int name_fault,process_fault;
static const char *const fixture_names[11]={"scePadReadState","scePadReadStateExt","scePadRead",
    "scePadReadExt","scePadGetDataInternal","scePadGetControllerInformation",
    "scePadGetExtControllerInformation","scePadGetTriggerEffectState","scePadSetVibration","scePadSetVibrationMode","scePadSetTriggerEffect"};
static void nid_encode(const char *name,char *out){
    if(!strcmp(name,"scePadGetTriggerEffectState")){strcpy(out,"znaWI0gpuo8");return;}
    if(!strcmp(name,"scePadSetTriggerEffect")){strcpy(out,"2JgFB2n9oUM");return;}
    for(unsigned i=0;i<11;i++)if(!strcmp(name,fixture_names[i])){snprintf(out,12,"FAKE%07u",i);return;}
}
static intptr_t kernel_get_proc(pid_t pid){return pid==59?0x10000:0;}
static int kernel_copyout(intptr_t address,void *out,size_t size){
    for(unsigned i=0;i<fixture_kernel_range_count;i++){
        const FixtureKernelRange *range=&fixture_kernel_ranges[i];
        uint64_t raw=(uint64_t)address;
        if(raw>=range->address && raw-range->address<range->length){
            size_t offset=(size_t)(raw-range->address);
            if(size>range->length-offset){outside_reads++;return -1;}
            if(range->kind==0)symbol_reads++;
            if(range->kind==1)name_reads++;
            if(range->kind==2)rela_reads++;
            memcpy(out,(const uint8_t *)range->bytes+offset,size);return 0;
        }
    }
    if((uint64_t)address>=UINT64_C(0xffff800000000000)){outside_reads++;return -1;}
    if(address==0x103e8 && size==8){*(uint64_t*)out=0x20000;return 0;}
    if(address==0x20000 && size==8){*(uint64_t*)out=(uintptr_t)&object;return 0;}
    if(size==sizeof(Elf64_Sym) && (uintptr_t)address>=section.symtab &&
       (uintptr_t)address<=section.symtab+section.symtab_size){
        symbol_reads++;
        if(size>section.symtab_size || (uintptr_t)address-section.symtab>section.symtab_size-size){
            outside_reads++;return -1;
        }
    }
    if((uintptr_t)address>=(uintptr_t)ordinary && (uintptr_t)address<(uintptr_t)ordinary+sizeof ordinary){
        rela_reads++;
        if((uintptr_t)address+size>(uintptr_t)ordinary+sizeof ordinary){outside_reads++;return -1;}
    }
    if((uintptr_t)address>=(uintptr_t)table && (uintptr_t)address<(uintptr_t)table+sizeof table){
        name_reads++;
        if((uintptr_t)address+size>(uintptr_t)table+section.strtab_size){outside_reads++;return -1;}
        if(name_fault==2 || (name_fault==1 && size>1))return -1;
    }
    memcpy(out,(const void*)(uintptr_t)address,size);return 0;
}
static int game_bridge_process_read(pid_t pid,intptr_t address,void *out,size_t size){
    (void)pid;process_reads++;
    if(process_fault)return -1;
    if((uintptr_t)address<(uintptr_t)table || (uintptr_t)address+size>(uintptr_t)table+section.strtab_size){outside_reads++;return -1;}
    memcpy(out,(const void*)(uintptr_t)address,size);return 0;
}
static int remote_reader_copyout(pid_t pid,intptr_t address,void *out,size_t size){
    (void)pid;if((address!=(intptr_t)slots && address!=(intptr_t)&slots[1]) || size!=8){outside_reads++;return -1;}
    memcpy(out,(const void*)(uintptr_t)address,size);return 0;
}
static int game_bridge_pointer_in_loaded_object(pid_t pid,uint64_t address){(void)pid;(void)address;return 1;}
static int kernel_get_vmem_protection(pid_t pid,intptr_t address,size_t length){(void)pid;(void)address;(void)length;return 3;}
static char reports[16384];
static void report_printf(int fd,const char *format,...){
    (void)fd;size_t used=strlen(reports);va_list args;va_start(args,format);
    if(used<sizeof reports-1)vsnprintf(reports+used,sizeof reports-used,format,args);
    va_end(args);
}
static void game_bridge_dump_modules_report(pid_t pid,intptr_t base,const GameBridgeImportHook *hooks,uint32_t count){(void)pid;(void)base;(void)hooks;(void)count;}
static void prepare(const char *name){
    reports[0]=0;
    memset(&object,0,sizeof object);memset(&section,0,sizeof section);memset(&symbol,0,sizeof symbol);memset(&relocation,0,sizeof relocation);
    memset(table,0,sizeof table);strcpy(table,name);memset(path,0,sizeof path);strcpy(path,"/app0/eboot.bin");
    for(unsigned i=0;i<11;i++)originals[i]=0x100000+i*0x100;
    slots[0]=originals[0];slots[1]=0;
    object.path=(uintptr_t)path;object.mapbase=(uintptr_t)slots;object.mapsize=sizeof slots;object.dynsec=(uintptr_t)&section;
    section.symtab=(uintptr_t)&symbol;section.symtab_size=sizeof symbol;
    section.strtab=(uintptr_t)table;section.strtab_size=strlen(name)+1;
    section.plt_rela=(uintptr_t)&relocation;section.plt_rela_size=sizeof relocation;
    relocation.r_info=ELF64_R_INFO(0,R_X86_64_JMP_SLOT);
    name_reads=process_reads=outside_reads=rela_reads=symbol_reads=0;name_fault=process_fault=0;
    fixture_kernel_range_count=0;
}
'''+helper+'\nstatic int\n'+function(source,'game_bridge_symbol_kind')+'\nstatic int\n'+function(source,'game_bridge_collect_import_hooks')+r'''
static int scan(int allow,uint32_t *count,GameBridgeImportHook *hooks){return game_bridge_collect_import_hooks(59,0x90000,originals,hooks,count,-1,allow,0);}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
'''+body
    count = int(re.search(r'#define POORDS4_GAME_BRIDGE_API_COUNT (\d+)u', source).group(1))
    if count not in (11, 12):
        raise ValueError('Import fixture needs explicit names for this API count')
    program = program.replace('#define POORDS4_GAME_BRIDGE_API_COUNT 11u',
                              f'#define POORDS4_GAME_BRIDGE_API_COUNT {count}u')
    program = program.replace('originals[11]', f'originals[{count}]')
    program = program.replace('fixture_names[11]', f'fixture_names[{count}]')
    program = program.replace('i<11;', f'i<{count};')
    program = program.replace('static void nid_encode(const char *name,char *out){',
                              'static void nid_encode(const char *name,char *out){\n'
                              '    if(!strcmp(name,"scePadDeviceClassGetExtendedInformation"))'
                              '{strcpy(out,"AcslpN1jHR8");return;}')
    if count == 12:
        program = program.replace('"scePadSetTriggerEffect"};',
                                  '"scePadSetTriggerEffect","scePadDeviceClassGetExtendedInformation"};')
    return program
