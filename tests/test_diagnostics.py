# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise bounded diagnostic failures without Sony calls or console input."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("POORDS4_TEST_ROOT", ROOT)).resolve()

PROGRAM = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x100000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x200000
#endif
static unsigned exists, opens, closes, unlinks, polls, read_calls, bad_flags;
static unsigned write_limit, zero_write, fail_read, fail_close, interruptions;
static unsigned source_length, destination_length, flag_mode;
static char source[70000], destination[66000];
static int model_fstat(int fd, struct stat *out) {
    if (fd != 10) return -1;
    memset(out, 0, sizeof(*out)); out->st_mode = S_IFREG; out->st_size = source_length; return 0;
}
static int model_open(const char *path, int flags, ...) {
    (void)path; opens++;
    if ((flags & (O_EXCL|O_NOFOLLOW|O_NONBLOCK)) != (O_EXCL|O_NOFOLLOW|O_NONBLOCK)) bad_flags++;
    if (exists) { errno=EEXIST; return -1; }
    exists=1; return 20;
}
static ssize_t model_pread(int fd, void *out, size_t amount, off_t offset) {
    read_calls++;
    if (fd!=10 || fail_read) { errno=EIO; return -1; }
    if (offset < 0 || (uint64_t)offset+amount > source_length) return -1;
    memcpy(out,source+(size_t)offset,amount); return (ssize_t)amount;
}
static ssize_t model_write(int fd, const void *bytes, size_t amount) {
    if (fd!=20) return -1;
    if (interruptions) { interruptions--; errno=EINTR; return -1; }
    if (zero_write) return 0;
    if (write_limit && amount>write_limit) amount=write_limit;
    if (destination_length+amount>=sizeof(destination)) return -1;
    memcpy(destination+destination_length,bytes,amount); destination_length+=(unsigned)amount;
    destination[destination_length]=0; return (ssize_t)amount;
}
static int model_close(int fd) { if(fd!=20) return -1; closes++; return fail_close ? -1 : 0; }
static int model_unlink(const char *path) { (void)path; unlinks++; exists=0; destination_length=0; return 0; }
static int model_lstat(const char *path, struct stat *out) {
    (void)path; polls++; memset(out,0,sizeof(*out));
    if (!flag_mode) return -1;
    out->st_mode = flag_mode==1 ? S_IFREG : S_IFDIR; return 0;
}
#define fstat model_fstat
#define open model_open
#define pread model_pread
#define write model_write
#define close model_close
#define unlink model_unlink
#define lstat model_lstat
#include "diagnostic_files.h"
static unsigned checks;
#define CHECK(c) do { checks++; if(!(c)) { fprintf(stderr,"check failed line=%u: %s\n",__LINE__,#c); return 1; } } while(0)
static void reset(void) {
    exists=opens=closes=unlinks=polls=read_calls=bad_flags=0;
    write_limit=zero_write=fail_read=fail_close=interruptions=0;
    source_length=27; destination_length=flag_mode=0;
    memset(source,'x',sizeof(source)); source[0]='e'; memset(destination,0,sizeof(destination));
}
int main(void) {
    reset(); errno=EDOM;
    CHECK(poords4_capture_first_failure(10,-1)==1);
    CHECK(errno==EDOM && !bad_flags && exists && closes==1);
    CHECK(!memcmp(destination,source,source_length));
    CHECK(strstr(destination,"first_failure_capture_complete=1")!=NULL);
    CHECK(strstr(destination,"first_failure_truncated=0")!=NULL);
    unsigned old_length=destination_length;
    CHECK(poords4_capture_first_failure(10,0)==0 && destination_length==old_length && read_calls==1);
    for(int result=-6;result<=2;result++) {
        if(result==-1 || result==0)continue;
        reset(); CHECK(poords4_capture_first_failure(10,result)==0 && opens==0);
    }
    reset(); CHECK(poords4_capture_first_failure(-1,-1)==0 && opens==0);
    reset(); source_length=0; CHECK(poords4_capture_first_failure(10,-1)==0 && opens==0);
    reset(); write_limit=3; CHECK(poords4_capture_first_failure(10,-1)==1 && !memcmp(destination,source,source_length));
    reset(); zero_write=1; errno=EDOM; CHECK(poords4_capture_first_failure(10,-1)==0 && !exists && unlinks==1 && errno==EDOM);
    reset(); fail_read=1; CHECK(poords4_capture_first_failure(10,-1)==0 && !exists && closes==1);
    reset(); fail_close=1; CHECK(poords4_capture_first_failure(10,-1)==0 && !exists);
    reset(); interruptions=2; CHECK(poords4_capture_first_failure(10,-1)==1);
    reset(); interruptions=50; CHECK(poords4_capture_first_failure(10,-1)==0 && !exists && interruptions>=40);
    reset(); source_length=sizeof(source); CHECK(poords4_capture_first_failure(10,-1)==1);
    CHECK(!memcmp(destination,source,65536));
    CHECK(destination_length<65700 && strstr(destination+65536,"first_failure_truncated=1")!=NULL);
    PoorDS4DiagnosticLogging state={0};
    reset(); errno=EDOM;
    CHECK(poords4_diagnostic_logging(&state,0)==0 && polls==0);
    CHECK(poords4_diagnostic_logging(&state,100)==0 && polls==1 && errno==EDOM);
    flag_mode=1;
    CHECK(poords4_diagnostic_logging(&state,5099)==0 && polls==1);
    CHECK(poords4_diagnostic_logging(&state,5100)==1 && polls==2);
    flag_mode=0;
    CHECK(poords4_diagnostic_logging(&state,10099)==1 && polls==2);
    CHECK(poords4_diagnostic_logging(&state,10100)==0 && polls==3);
    flag_mode=2; CHECK(poords4_diagnostic_logging(&state,15100)==0 && polls==4);
    flag_mode=1; CHECK(poords4_diagnostic_logging(&state,UINT64_MAX-10)==1 && state.next_poll_ms==UINT64_MAX);
    printf("diagnostic_checks=%u passed=1\n",checks); return 0;
}
'''


class DiagnosticFiles(unittest.TestCase):
    def test_bounded_capture_and_optional_logging(self):
        with tempfile.TemporaryDirectory(prefix="poords4-diagnostics-") as work:
            cfile = Path(work) / "diagnostics.c"
            exe = Path(work) / ("diagnostics.exe" if os.name == "nt" else "diagnostics")
            cfile.write_text(PROGRAM, encoding="utf-8")
            result = subprocess.run(
                [os.environ.get("POORDS4_HOST_CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(SOURCE / "payload"), str(cfile), "-o", str(exe)],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("passed=1", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
