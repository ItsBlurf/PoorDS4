# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay the game's device-class query imports through actual collector code."""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from import_scan_fixture import scanner_program
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--payload", type=Path, required=True)
payload = parser.parse_args().payload
source = (payload / "wireless_ds4.c").read_text(encoding="utf-8-sig")
body = r'''
int main(void) {
    unsigned failures=0, checks=0; GameBridgeImportHook hooks[32]; uint32_t count;
    const char *names[]={"scePadDeviceClassGetExtendedInformation","AcslpN1jHR8",
        "scePadDeviceClassGetExtendedInformation#K#K","AcslpN1jHR8#K#K"};
    for(unsigned i=0;i<4;i++)for(unsigned ordinary_table=0;ordinary_table<2;ordinary_table++) {
        prepare(names[i]);slots[0]=0x100b00;symbol.st_info=STT_FUNC;symbol.st_shndx=SHN_UNDEF;
        relocation.r_info=ELF64_R_INFO(0,ordinary_table?R_X86_64_GLOB_DAT:R_X86_64_JMP_SLOT);
        if(ordinary_table) {
            section.plt_rela=section.plt_rela_size=0;
            section.rela=(uintptr_t)ordinary;section.rela_size=sizeof ordinary[0];ordinary[0]=relocation;
        }
        checks++;
        if(scan(0,&count,hooks)||count!=1||hooks[0].kind!=11||outside_reads)failures++;
    }
    printf("device_class_import_checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
'''
program = "#define mdbg_copyout remote_reader_copyout\n" + scanner_program(source, body)
with tempfile.TemporaryDirectory(prefix="poords4-device-info-import-") as temporary:
    temp = Path(temporary); c = temp / "test.c"; exe = temp / "test.exe"
    c.write_text(program, encoding="utf-8")
    cc = os.environ.get("POORDS4_HOST_CC", "gcc")
    build = subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", str(c), "-o", str(exe)],
                           capture_output=True, text=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True) if build.returncode == 0 else None
    result = {"compiled": build.returncode == 0, "passed": run is not None and run.returncode == 0,
              "output": run.stdout.strip() if run else "", "error": run.stderr if run else build.stderr[-4000:],
              "source_sha256": hashlib.sha256((payload / "wireless_ds4.c").read_bytes()).hexdigest(),
              "console_contacted": False, "gameplay_verified": False}
    print(json.dumps(result, indent=2))
    sys.exit(0 if result["passed"] else 1)
