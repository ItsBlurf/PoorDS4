/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional wireless DualShock 4 bridge for native PS5 games.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <machine/reg.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <elf.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>

#include "wireless_ds4.h"

#ifndef POORDS4_RC_VERSION
#define POORDS4_RC_VERSION 0
#endif
#include "pad_types.h"

#ifndef GC_FORCE_STRUCTURAL_FIRMWARE
#define GC_FORCE_STRUCTURAL_FIRMWARE 0
#endif

extern void poords4_log(const char *format, ...);
#define klog_printf poords4_log
#define POORDS4_REMOTE_PAD_CAPACITY 256u
#define POORDS4_GAME_BRIDGE_FW_0860 UINT32_C(0x08600004)
#define POORDS4_GAME_BRIDGE_FW_1160 UINT32_C(0x11600005)
#define POORDS4_GAME_BRIDGE_FW_1240 UINT32_C(0x12400009)
#define POORDS4_KEKCALL_REMOTE_SYSCALL UINT64_C(0x500000027)
#define POORDS4_KEKCALL_CHECK UINT64_C(0xffffffff00000027)
#define POORDS4_TARGET_PAGE_SIZE UINT64_C(0x4000)

static int remote_reader_copyin(
    pid_t pid, const void *buf, intptr_t addr, size_t len);
static int game_bridge_process_write(
    pid_t pid, intptr_t address, const void *source, size_t length);

void *p_poords4_kekcall_bridge;
__asm__(
    ".text\n"
    ".global poords4_kekcall_bridge\n"
    ".type poords4_kekcall_bridge,@function\n"
    "poords4_kekcall_bridge:\n"
    "mov 8(%rsp), %rax\n"
    "jmp *p_poords4_kekcall_bridge(%rip)\n");
extern uint64_t poords4_kekcall_bridge(
    uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t);

static int
poords4_kekcall_available(void)
{
    p_poords4_kekcall_bridge = (uint8_t *)(uintptr_t)getpid + 7;
    return poords4_kekcall_bridge(
        0, 0, 0, 0, 0, 0, POORDS4_KEKCALL_CHECK) == 0;
}

static int64_t
poords4_remote_syscall(pid_t pid, uint64_t number,
                           const uint64_t arguments[6])
{
    return (int64_t)poords4_kekcall_bridge(
        (uint64_t)(uint32_t)pid, number,
        (uint64_t)(uintptr_t)arguments, 0, 0, 0,
        POORDS4_KEKCALL_REMOTE_SYSCALL);
}

static int
poords4_remote_map(pid_t pid, size_t length, intptr_t *out_address)
{
    if (pid <= 0 || length == 0 || !out_address)
        return -1;
    uint64_t arguments[6] = {
        0, (uint64_t)length, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, UINT64_MAX, 0};
    int64_t mapped = poords4_remote_syscall(pid, SYS_mmap, arguments);
    if (mapped <= 0 || (uint64_t)mapped > (uint64_t)INTPTR_MAX)
        return -1;

    uint64_t lock_arguments[6] = {
        (uint64_t)mapped, (uint64_t)length, 0, 0, 0, 0};
    if (poords4_remote_syscall(pid, SYS_mlock, lock_arguments) != 0) {
        uint64_t unmap_arguments[6] = {
            (uint64_t)mapped, (uint64_t)length, 0, 0, 0, 0};
        (void)poords4_remote_syscall(
            pid, SYS_munmap, unmap_arguments);
        return -1;
    }
    *out_address = (intptr_t)mapped;
    return 0;
}

static int
poords4_remote_unmap(pid_t pid, intptr_t address, size_t length)
{
    if (pid <= 0 || address <= 0 || length == 0)
        return -1;
    uint64_t arguments[6] = {
        (uint64_t)address, (uint64_t)length, 0, 0, 0, 0};
    return poords4_remote_syscall(pid, SYS_munmap, arguments) == 0
        ? 0 : -1;
}

static int
poords4_remote_cow_write(pid_t pid, intptr_t address,
                             const void *bytes, size_t length,
                             int final_protection,
                             int64_t stages[9])
{
    if (pid <= 0 || address <= 0 || !bytes || length == 0 || length > 256u)
        return -1;
    for (unsigned index = 0; index < 9u; ++index)
        stages[index] = -1;

    int result = -1;
    stages[0] = 0;
    stages[1] = 0;

    uint64_t target_page = (uint64_t)address &
        ~(POORDS4_TARGET_PAGE_SIZE - 1u);
    uint64_t writable_args[6] = {
        target_page, POORDS4_TARGET_PAGE_SIZE,
        PROT_READ | PROT_WRITE, 0, 0, 0};
    stages[4] = poords4_remote_syscall(
        pid, SYS_mprotect, writable_args);
    if (stages[4] != 0)
        goto done;

    stages[5] = game_bridge_process_write(
        pid, address, bytes, length);
    if (stages[5] == 0)
        result = 0;

done:
    if (stages[4] == 0) {
        uint64_t restore_args[6] = {
            target_page, POORDS4_TARGET_PAGE_SIZE,
            (uint64_t)(uint32_t)final_protection, 0, 0, 0};
        stages[6] = poords4_remote_syscall(
            pid, SYS_mprotect, restore_args);
        if (stages[6] != 0)
            result = -1;
    }
    return result;
}

/* libScePad's private client-table layout verified from firmware 11.60. */
#define POORDS4_PAD_CLIENT_TABLE_1160 UINT32_C(0x00020018)
#define POORDS4_PAD_CLIENT_STRIDE_1160 UINT32_C(0x000005c8)
#define POORDS4_PAD_CLIENT_CONNECTED_1160 UINT32_C(0x18)
#define POORDS4_PAD_CLIENT_HANDLE_1160 UINT32_C(0x1c)
#define POORDS4_PAD_CLIENT_USER_ID_1160 UINT32_C(0x24)
#define POORDS4_PAD_CLIENT_VENDOR_1160 UINT32_C(0x60)
#define POORDS4_PAD_CLIENT_PRODUCT_1160 UINT32_C(0x62)

static void report_printf(int fd, const char *format, ...);
static int remote_pad_identity_1160(
    pid_t pid, intptr_t libpad_base, int32_t pad_handle,
    int32_t *out_connected, uint16_t *out_vendor, uint16_t *out_product);
static int remote_pad_is_known_ds4(
    int32_t connected, uint16_t vendor, uint16_t product);
static int g_report_archive_fd = -1;

/* Firmware admission evidence captured from the already-safe RemotePlay
 * source process. A game on the same firmware must expose the identical
 * controller-information implementation before the structural fallback can
 * replace the old compiler-prologue lock. */
enum { POORDS4_PAD_FINGERPRINT_COUNT = 6 };
static uint32_t g_source_pad_firmware;
static uint64_t
    g_source_pad_fingerprints[POORDS4_PAD_FINGERPRINT_COUNT];
static uint32_t g_source_pad_fingerprint_mask;
static int g_source_controller_info_runtime_abi;

static uint64_t
poords4_fnv1a64(const uint8_t *buf, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    if (!buf) return 0;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)buf[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* sys_ptrace - elevate credentials for ptrace, then restore */
static int
sys_ptrace(int request, pid_t pid, caddr_t addr, int data)
{
    uint8_t privcaps[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    pid_t   mypid = getpid();
    uint8_t caps[16];
    uint64_t authid;
    int ret;

    if (!(authid = kernel_get_ucred_authid(mypid))) return -1;
    if (kernel_get_ucred_caps(mypid, caps))          return -1;
    if (kernel_set_ucred_authid(mypid, 0x4800000000010003l)) return -1;
    if (kernel_set_ucred_caps(mypid, privcaps)) {
        (void)kernel_set_ucred_authid(mypid, authid);
        return -1;
    }

    ret = (int)__syscall(SYS_ptrace, request, pid, addr, data);

    (void)kernel_set_ucred_caps(mypid, caps);
    (void)kernel_set_ucred_authid(mypid, authid);
    return ret;
}

/* A timed-out RemotePlay call may still be executing. Restoring registers or
 * detaching a running tracee in that state is unsafe, so first prove that the
 * process stopped or exited. */
static int
sys_kill_elevated(pid_t pid, int signal_number)
{
    uint8_t privcaps[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    pid_t mypid = getpid();
    uint8_t caps[16];
    uint64_t authid = kernel_get_ucred_authid(mypid);
    if (!authid || kernel_get_ucred_caps(mypid, caps) != 0)
        return -1;
    if (kernel_set_ucred_authid(
            mypid, UINT64_C(0x4800000000010003)) != 0)
        return -1;
    if (kernel_set_ucred_caps(mypid, privcaps) != 0) {
        (void)kernel_set_ucred_authid(mypid, authid);
        return -1;
    }
    int result = (int)__syscall(SYS_kill, pid, signal_number);
    int saved_errno = errno;
    (void)kernel_set_ucred_caps(mypid, caps);
    (void)kernel_set_ucred_authid(mypid, authid);
    errno = saved_errno;
    return result;
}

/* Returns 1 with the tracee stopped, 0 if it exited/was killed, and -1 only
 * when neither state could be proved. */
static int
stop_timed_out_tracee(pid_t pid, const char *stage)
{
    errno = 0;
    int stop_result = sys_kill_elevated(pid, SIGSTOP);
    int stop_errno = errno;
    klog_printf(
        "[WirelessDS4] pt_call recovery stage=%s SIGSTOP=%d errno=%d "
        "pid=%d\n", stage ? stage : "timeout", stop_result,
        stop_errno, pid);
    if (stop_result == 0) {
        for (unsigned attempt = 0; attempt < 2000; ++attempt) {
            int status = 0;
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                if (WIFSTOPPED(status))
                    return 1;
                if (WIFEXITED(status) || WIFSIGNALED(status))
                    return 0;
            } else if (waited < 0 && errno == ESRCH) {
                return 0;
            }
            usleep(1000);
        }
    }

    /* Never use PT_KILL against SceRemotePlay. SIGSTOP is unmaskable; keep
     * waiting for a provable stop/exit and periodically reassert it rather
     * than detaching unknown registers. */
    for (unsigned attempt = 0;; ++attempt) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            if (WIFSTOPPED(status))
                return 1;
            if (WIFEXITED(status) || WIFSIGNALED(status))
                return 0;
        } else if (waited < 0 &&
                   (errno == ESRCH || errno == ECHILD)) {
            return 0;
        }
        if ((attempt % 5000u) == 4999u) {
            klog_printf(
                "[WirelessDS4] pt_call recovery still waiting for "
                "system tracee pid=%d\n", pid);
            (void)sys_kill_elevated(pid, SIGSTOP);
        }
        usleep(1000);
    }
}

/* find_pids - locate processes by thread name via sysctl (ki_pid@72, ki_tdname@447) */
static size_t
find_pids(const char *name, pid_t *pids, size_t max_pids)
{
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid();
    size_t buf_size;
    uint8_t *buf;
    size_t count = 0;

    if (!pids || max_pids == 0) return 0;
    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0)) return 0;
    if (!(buf = malloc(buf_size)))                 return 0;
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) { free(buf); return 0; }

    const uint8_t *end = buf + buf_size;
    for (uint8_t *ptr = buf; ptr < end;) {
        if ((size_t)(end - ptr) < sizeof(int))
            break;
        int ki_structsize = 0;
        memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
        if (ki_structsize < 448 ||
            (size_t)ki_structsize > (size_t)(end - ptr))
            break;
        pid_t ki_pid = 0;
        memcpy(&ki_pid, ptr + 72, sizeof(ki_pid));
        const char *ki_tdname = (const char *)&ptr[447];
        size_t tdname_limit = (size_t)ki_structsize - 447u;
        size_t tdname_length = 0;
        while (tdname_length < tdname_limit &&
               ki_tdname[tdname_length] != '\0')
            tdname_length++;
        size_t pi;
        int seen = 0;

        ptr += (size_t)ki_structsize;
        if (tdname_length == tdname_limit ||
            strcmp(name, ki_tdname) || ki_pid == mypid) {
            continue;
        }
        for (pi = 0; pi < count; pi++) {
            if (pids[pi] == ki_pid) {
                seen = 1;
                break;
            }
        }
        if (seen || count >= max_pids) {
            continue;
        }
        pids[count++] = ki_pid;
    }

    for (size_t i = 1; i < count; i++) {
        pid_t pid = pids[i];
        size_t j = i;
        while (j > 0 && pids[j - 1] > pid) {
            pids[j] = pids[j - 1];
            j--;
        }
        pids[j] = pid;
    }

    free(buf);
    return count;
}

/* resolve_sym - look up a symbol in a remote process library */
static intptr_t
resolve_sym(pid_t pid, uint32_t lib_handle, const char *sym)
{
    intptr_t addr = kernel_dynlib_dlsym(pid, lib_handle, sym);
    if (addr) return addr;

#ifdef __PROSPERO__
    char nid[12];
    nid_encode(sym, nid);
    addr = kernel_dynlib_resolve(pid, lib_handle, nid);
    return addr;
#else
    return 0;
#endif
}

/* get_lib - wrapper around kernel_dynlib_handle with logging */
static int
get_lib(pid_t pid, const char *name, uint32_t *handle)
{
    *handle = 0;
    int ret = kernel_dynlib_handle(pid, name, handle);
    if (ret != 0 || *handle == 0) {
        char sprx[64];
        snprintf(sprx, sizeof(sprx), "%s.sprx", name);
        ret = kernel_dynlib_handle(pid, sprx, handle);
    }
    klog_printf("[WirelessDS4] dynlib_handle(%s) -> ret=%d handle=0x%x\n",
                name, ret, *handle);
    return (*handle != 0) ? 0 : -1;
}

/* Read-only launch readiness check. Unlike get_lib(), this is called once per
 * supervisor poll and deliberately stays quiet until the game has finished
 * loading the modules required by the bridge. */
static int
get_lib_quiet(pid_t pid, const char *name, uint32_t *handle)
{
    if (!name || !handle)
        return -1;
    *handle = 0;
    int result = kernel_dynlib_handle(pid, name, handle);
    if (result != 0 || *handle == 0) {
        char sprx[64];
        int length = snprintf(sprx, sizeof(sprx), "%s.sprx", name);
        if (length <= 0 || (size_t)length >= sizeof(sprx))
            return -1;
        result = kernel_dynlib_handle(pid, sprx, handle);
    }
    return result == 0 && *handle != 0 ? 0 : -1;
}

/* pt_io_write - write process memory via PT_IO (process must be stopped) */
static int
pt_io_write(pid_t pid, intptr_t dst, const void *src, size_t len)
{
    struct ptrace_io_desc iod;
    iod.piod_op    = PIOD_WRITE_D;
    iod.piod_offs  = (void *)dst;
    iod.piod_addr  = (void *)src;
    iod.piod_len   = len;
    return sys_ptrace(PT_IO, pid, (caddr_t)&iod, 0);
}

static int64_t
pt_call(pid_t pid, intptr_t fn, intptr_t trap_rip,
        uint64_t a1, uint64_t a2, uint64_t a3,
        uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct reg regs = {0};
    struct reg saved = {0};
    int status;

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) return -1;
    memcpy(&saved, &regs, sizeof(regs));

    /*
     * Skip the x86-64 red zone, then build the stack exactly as a real CALL
     * would. The AMD64 ABI requires 16-byte alignment before CALL, so a
     * callee observes RSP % 16 == 8 after the return address is pushed.
     * Entering with RSP % 16 == 0 can fault on aligned SIMD stack accesses.
     */
    intptr_t new_rsp = ((regs.r_rsp - 256) & ~(intptr_t)0xf) - 8;

    if (pt_io_write(pid, new_rsp, &trap_rip, 8)) return -1;

    regs.r_rsp = new_rsp;
    regs.r_rip = fn;
    regs.r_rdi = a1;
    regs.r_rsi = a2;
    regs.r_rdx = a3;
    regs.r_rcx = a4;
    regs.r_r8  = a5;
    regs.r_r9  = a6;

    if (sys_ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0)) return -1;
    if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, 0)) {
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    /* Wait for our SIGTRAP; forward other signals so the process stays healthy.
     * Use WNOHANG + 1 ms sleep so we never block forever if the INT3 doesn't fire
     * (e.g. if the write failed silently or the process runs past trap_rip).
     * SIGCHLD (17) is suppressed rather than forwarded: forwarding it while the
     * main thread is executing injected code (pthread_create, pad IPC calls) has
     * been observed to cause a kernel panic when SceShellUI's SIGCHLD handler
     * runs concurrently with thread-creation internals. */
    int got_trap = 0;
    for (int total_ms = 0; total_ms < 5000; ) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r < 0) {
            klog_printf("[WirelessDS4] pt_call: waitpid error errno=%d\n", errno);
            break;
        }
        if (r == 0) {
            usleep(1000);   /* 1 ms - process has not stopped yet */
            total_ms++;
            continue;
        }
        /* Process stopped */
        if (!WIFSTOPPED(status)) {
            klog_printf("[WirelessDS4] pt_call: process exited status=0x%x\n", status);
            sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -1;
        }
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) { got_trap = 1; break; }
        if (sig == SIGBUS || sig == SIGSEGV) {
            klog_printf("[WirelessDS4] pt_call: suppressing fatal target sig=%d\n",
                        sig);
            (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -2;
        }
        /* Suppress SIGCHLD; forwarding it during injected execution causes panics. */
        int fwd = (sig == 17) ? 0 : sig;
        if (fwd != sig)
            klog_printf("[WirelessDS4] pt_call: suppressing SIGCHLD\n");
        else
            klog_printf("[WirelessDS4] pt_call: forwarding sig=%d\n", sig);
        if (sys_ptrace(PT_CONTINUE, pid, (caddr_t)1, fwd) != 0) {
            (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
            return -1;
        }
    }
    if (!got_trap) {
        klog_printf("[WirelessDS4] pt_call: timed out waiting for SIGTRAP fn=0x%lx\n", fn);
        int recovery = stop_timed_out_tracee(pid, "remote-call-timeout");
        if (recovery == 1)
            (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }

    if (sys_ptrace(PT_GETREGS, pid, (caddr_t)&regs, 0)) {
        (void)sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        return -1;
    }
    int64_t retval = (int64_t)regs.r_rax;
    klog_printf("[WirelessDS4] pt_call: fn=0x%lx rip=0x%lx rax=0x%lx\n",
                fn, (uint64_t)regs.r_rip, (uint64_t)retval);

    sys_ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
    return retval;
}


typedef struct {
    intptr_t fp_readstate;
    intptr_t fp_usleep;
    int32_t  pad_handle;
    uint32_t interval_us;
    volatile int32_t  ready;       /* 0=starting, 1=running, 2=stopped */
    volatile int32_t  stop;
    volatile int32_t  last_result;
    volatile uint32_t seq;
    uint8_t pad_data[POORDS4_REMOTE_PAD_CAPACITY];
    intptr_t fp_kill;
    intptr_t fp_closepad;
    int32_t owner_pid;
    int32_t close_pad_on_exit;
    uint32_t owner_check_interval;
    volatile uint32_t owner_miss_count;
    volatile uint32_t owner_watchdog_exits;
    uint32_t reserved;
} RemotePadReaderArgs;

_Static_assert(sizeof(ScePadData) == 120,
               "ScePadData ABI changed");
_Static_assert(sizeof(RemotePadReaderArgs) == 336,
               "RemotePadReaderArgs ABI changed");

extern void *remote_pad_reader_stub(void *arg);
extern void remote_pad_reader_stub_end(void);

/*
 * Position-independent target thread. Do not call local symbols or touch
 * globals: every target-side call is made through a resolved function pointer
 * stored in RemotePadReaderArgs.
 */
__attribute__((noinline, used, section(".text.ds4reader")))
void *
remote_pad_reader_stub(void *arg)
{
    RemotePadReaderArgs *a = (RemotePadReaderArgs *)arg;
    typedef int32_t (*read_fn_t)(int32_t, void *);
    typedef void (*usleep_fn_t)(unsigned int);
    typedef int32_t (*kill_fn_t)(int32_t, int32_t);
    typedef int32_t (*close_pad_fn_t)(int32_t);
    read_fn_t readstate = (read_fn_t)(uintptr_t)a->fp_readstate;
    usleep_fn_t sleep_us = (usleep_fn_t)(uintptr_t)a->fp_usleep;
    kill_fn_t check_owner = (kill_fn_t)(uintptr_t)a->fp_kill;
    close_pad_fn_t close_pad =
        (close_pad_fn_t)(uintptr_t)a->fp_closepad;
    uint32_t owner_countdown = a->owner_check_interval;

    __atomic_store_n(&a->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&a->stop, __ATOMIC_ACQUIRE)) {
        uint32_t odd =
            (__atomic_load_n(&a->seq, __ATOMIC_RELAXED) + 1u) | 1u;
        __atomic_store_n(&a->seq, odd, __ATOMIC_RELEASE);
        int32_t result = readstate(a->pad_handle, a->pad_data);
        __atomic_store_n(&a->last_result, result, __ATOMIC_RELAXED);
        __atomic_store_n(&a->seq, odd + 1u, __ATOMIC_RELEASE);
        sleep_us(a->interval_us);
        if (check_owner && a->owner_pid > 1 &&
            a->owner_check_interval != 0) {
            if (owner_countdown > 1u) {
                owner_countdown--;
            } else {
                owner_countdown = a->owner_check_interval;
                if (check_owner(a->owner_pid, 0) == 0) {
                    __atomic_store_n(
                        &a->owner_miss_count, 0, __ATOMIC_RELAXED);
                } else {
                    uint32_t misses = (uint32_t)__atomic_add_fetch(
                        &a->owner_miss_count, 1, __ATOMIC_RELAXED);
                    if (misses >= 3u) {
                        (void)__atomic_fetch_add(
                            &a->owner_watchdog_exits, 1,
                            __ATOMIC_RELAXED);
                        break;
                    }
                }
            }
        }
    }
    if (a->close_pad_on_exit && close_pad && a->pad_handle >= 0)
        (void)close_pad(a->pad_handle);
    __atomic_store_n(&a->ready, 2, __ATOMIC_RELEASE);
    return (void *)0;
}

__attribute__((noinline, used, section(".text.ds4reader")))
void
remote_pad_reader_stub_end(void)
{
}

/*
 * Native-game pad-read bridge. These functions are copied into the target
 * game and must remain position independent: no local calls, global accesses,
 * or libc helpers are permitted in the copied range.
 */
#define POORDS4_GAME_BRIDGE_MAGIC UINT32_C(0x34424744) /* "DGB4" */
#define POORDS4_GAME_BRIDGE_PAD_SIZE 120u
#define POORDS4_GAME_BRIDGE_LAYOUT_V1 UINT32_C(0x52433433) /* legacy ABI v1 */
#define POORDS4_GAME_BRIDGE_SNAPSHOT_RETRIES 64u
#define POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS 64u
#define POORDS4_GAME_BRIDGE_DIRECT_LEASE 2048u

typedef struct {
    uint32_t magic;
    volatile uint32_t active;
    volatile uint32_t seq;
    uint32_t pad_size;
    int32_t pad_handle;
    uint32_t reserved0;
    intptr_t fp_state_internal;
    intptr_t fp_read_internal;
    intptr_t fp_data_internal;
    intptr_t fp_get_controller_info_trampoline;
    intptr_t fp_socket;
    intptr_t fp_bind;
    intptr_t fp_recvfrom;
    intptr_t fp_close;
    intptr_t remote_block;
    uint32_t remote_block_size;
    uint32_t original_protection;
    uint32_t original_data_protection;
    uint32_t original_info_function_protection;
    uint32_t reserved1;
    intptr_t read_state_address;
    intptr_t read_state_ext_address;
    intptr_t read_address;
    intptr_t read_ext_address;
    intptr_t data_internal_address;
    intptr_t controller_info_address;
    intptr_t controller_info_trampoline;
    intptr_t controller_info_gateway;
    uint8_t original_read_state[16];
    uint8_t original_read_state_ext[16];
    uint8_t original_read[16];
    uint8_t original_read_ext[16];
    uint8_t original_data_internal[16];
    uint8_t original_controller_info[32];
    volatile uint64_t read_state_calls;
    volatile uint64_t read_state_ext_calls;
    volatile uint64_t read_calls;
    volatile uint64_t read_ext_calls;
    volatile uint64_t data_internal_calls;
    volatile uint64_t controller_info_calls;
    volatile uint64_t controller_info_spoofs;
    /* Legacy-compatible bridge ABI v1 prefix retained byte-for-byte so current builds can
     * identify and recover a bridge left by RC33. These fields are inactive. */
    volatile int32_t legacy_receiver_ready;
    volatile int32_t legacy_receiver_stop;
    volatile int32_t legacy_receiver_last_result;
    uint32_t legacy_receiver_port;
    volatile uint64_t legacy_receiver_packets;
    uint8_t legacy_pad_data[POORDS4_GAME_BRIDGE_PAD_SIZE];
    intptr_t legacy_fp_setsockopt;
    volatile uint64_t legacy_receiver_timeouts;
    volatile uint64_t lease_expirations;
    volatile uint32_t legacy_receiver_timeout_streak;
    uint32_t reserved2;
    /* RC31 diagnostics for one-frame continuity protection. */
    volatile uint64_t snapshot_contention_fallbacks;
    volatile uint64_t controller_info_result_overrides;
    volatile uint64_t native_backing_calls;
    volatile uint64_t native_backing_errors;
    /* RC33 redirects caller-owned PLT/GOT slots instead of modifying
     * libScePad text.  The saved entries make removal transactional. */
    uint32_t import_hook_count;
    uint32_t import_hook_reserved;
    intptr_t import_hook_slots[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS];
    intptr_t import_hook_originals[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS];
    intptr_t import_hook_gateways[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS];
    uint32_t import_hook_protections[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS];
    uint32_t import_hook_kinds[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS];
    /* The supervisor fills the inactive buffer, then publishes the new
     * sequence and lease together. Readers retry if publication changes
     * mid-copy. */
    volatile uint32_t direct_seq;
    volatile uint32_t direct_lease;
    volatile uint32_t direct_active;
    volatile uint32_t direct_packets;
    uint8_t direct_pad_data[2][POORDS4_GAME_BRIDGE_PAD_SIZE];
} GamePadBridgeArgs;

_Static_assert(offsetof(GamePadBridgeArgs, legacy_fp_setsockopt) == 496,
               "PoorDS4 bridge ABI v1 prefix changed");
_Static_assert(sizeof(GamePadBridgeArgs) == 2872,
               "GamePadBridgeArgs ABI changed");

static pid_t g_game_bridge_direct_pid = -1;
static intptr_t g_game_bridge_direct_args = 0;
static uint32_t g_game_bridge_direct_seq = 0;
static uint32_t g_game_bridge_direct_packets = 0;
static uint64_t g_game_bridge_direct_cr3 = 0;
static uint64_t g_game_bridge_direct_dmap = 0;

extern int32_t game_pad_read_state_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_read_state_ext_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_read_stub(
    int32_t handle, void *out, int32_t num, GamePadBridgeArgs *args);
extern int32_t game_pad_read_ext_stub(
    int32_t handle, void *out, int32_t num, GamePadBridgeArgs *args);
extern int32_t game_pad_get_data_internal_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_get_controller_info_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern void game_pad_bridge_stub_end(void);

static __attribute__((always_inline)) inline int
game_bridge_direct_available(GamePadBridgeArgs *args)
{
    if (!args || args->reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1 ||
        !__atomic_load_n(&args->direct_active, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t lease = __atomic_load_n(
        &args->direct_lease, __ATOMIC_RELAXED);
    while (lease != 0) {
        if (__atomic_compare_exchange_n(
                &args->direct_lease, &lease, lease - 1u, 0,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    if (__atomic_exchange_n(
            &args->direct_active, 0, __ATOMIC_ACQ_REL) != 0)
        (void)__atomic_fetch_add(
            &args->lease_expirations, 1,
            __ATOMIC_RELAXED);
    return 0;
}

static __attribute__((always_inline)) inline int
game_bridge_handle_matches(GamePadBridgeArgs *args, int32_t handle)
{
    if (!args || handle < 0)
        return 0;
    if (handle == args->pad_handle)
        return 1;
    /* libScePad regenerates the upper handle bits when a controller/client
     * is reopened, while the low byte remains the global player slot.  The
     * installer derives and validates reserved0 from the selected client
     * table entry.  Following that slot keeps P2 alive across a handle
     * generation change without ever matching the connected P1 entry. */
    int32_t index = (int32_t)args->reserved0;
    return index >= 0 && index < 24 && (handle & 0xff) == index;
}

static __attribute__((always_inline)) inline int
game_bridge_copy_direct(GamePadBridgeArgs *args, void *out,
                        volatile uint64_t *call_counter)
{
    volatile uint8_t *destination = (volatile uint8_t *)out;
    for (unsigned attempt = 0;
         attempt < POORDS4_GAME_BRIDGE_SNAPSHOT_RETRIES; ++attempt) {
        uint32_t before = __atomic_load_n(
            &args->direct_seq, __ATOMIC_ACQUIRE);
        for (unsigned byte = 0;
             byte < POORDS4_GAME_BRIDGE_PAD_SIZE; ++byte)
            destination[byte] =
                args->direct_pad_data[before & 1u][byte];
        if (before == __atomic_load_n(
                &args->direct_seq, __ATOMIC_ACQUIRE)) {
            (void)__atomic_fetch_add(
                call_counter, 1, __ATOMIC_RELAXED);
            return 1;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    if (!__atomic_load_n(
            &args->direct_active, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t current = __atomic_load_n(
        &args->direct_seq, __ATOMIC_ACQUIRE);
    for (unsigned byte = 0;
         byte < POORDS4_GAME_BRIDGE_PAD_SIZE; ++byte)
        destination[byte] =
            args->direct_pad_data[current & 1u][byte];
    destination[offsetof(ScePadData, connected)] = 1;
    (void)__atomic_fetch_add(
        &args->snapshot_contention_fallbacks, 1, __ATOMIC_RELAXED);
    (void)__atomic_fetch_add(call_counter, 1, __ATOMIC_RELAXED);
    return 1;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_state_stub(int32_t handle, void *out,
                         GamePadBridgeArgs *args)
{
    typedef int32_t (*state_internal_fn)(int32_t, void *, int32_t);
    state_internal_fn original =
        (state_internal_fn)(uintptr_t)(
            args ? args->fp_state_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        game_bridge_handle_matches(args, handle) && out &&
        game_bridge_direct_available(args) &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (original) {
            native_result = original(handle, out, 0);
            native_called = 1;
            (void)__atomic_fetch_add(
                &args->native_backing_calls, 1, __ATOMIC_RELAXED);
            if (native_result < 0)
                (void)__atomic_fetch_add(
                    &args->native_backing_errors, 1,
                    __ATOMIC_RELAXED);
        }
        if (game_bridge_copy_direct(
                args, out, &args->read_state_calls))
            return 0;
        return native_called ? native_result :
            (original ? original(handle, out, 0) : native_result);
    }
    if (native_called)
        return native_result;
    return original ? original(handle, out, 0) : native_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_state_ext_stub(int32_t handle, void *out,
                             GamePadBridgeArgs *args)
{
    typedef int32_t (*state_internal_fn)(int32_t, void *, int32_t);
    state_internal_fn original =
        (state_internal_fn)(uintptr_t)(
            args ? args->fp_state_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        game_bridge_handle_matches(args, handle) && out &&
        game_bridge_direct_available(args) &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (original) {
            native_result = original(handle, out, 1);
            native_called = 1;
            (void)__atomic_fetch_add(
                &args->native_backing_calls, 1, __ATOMIC_RELAXED);
            if (native_result < 0)
                (void)__atomic_fetch_add(
                    &args->native_backing_errors, 1,
                    __ATOMIC_RELAXED);
        }
        if (game_bridge_copy_direct(
                args, out, &args->read_state_ext_calls))
            return 0;
        return native_called ? native_result :
            (original ? original(handle, out, 1) : native_result);
    }
    if (native_called)
        return native_result;
    return original ? original(handle, out, 1) : native_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_stub(int32_t handle, void *out, int32_t num,
                   GamePadBridgeArgs *args)
{
    typedef int32_t (*read_internal_fn)(int32_t, void *, int32_t, int32_t);
    read_internal_fn original =
        (read_internal_fn)(uintptr_t)(
            args ? args->fp_read_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        game_bridge_handle_matches(args, handle) && out && num > 0 &&
        game_bridge_direct_available(args) &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (original) {
            native_result = original(handle, out, num, 0);
            native_called = 1;
            (void)__atomic_fetch_add(
                &args->native_backing_calls, 1, __ATOMIC_RELAXED);
            if (native_result < 0)
                (void)__atomic_fetch_add(
                    &args->native_backing_errors, 1,
                    __ATOMIC_RELAXED);
        }
        if (game_bridge_copy_direct(
                args, out, &args->read_calls))
            return 1;
        return native_called ? native_result :
            (original ? original(handle, out, num, 0) : native_result);
    }
    if (native_called)
        return native_result;
    return original ? original(handle, out, num, 0) : native_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_ext_stub(int32_t handle, void *out, int32_t num,
                       GamePadBridgeArgs *args)
{
    typedef int32_t (*read_internal_fn)(int32_t, void *, int32_t, int32_t);
    read_internal_fn original =
        (read_internal_fn)(uintptr_t)(
            args ? args->fp_read_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        game_bridge_handle_matches(args, handle) && out && num > 0 &&
        game_bridge_direct_available(args) &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (original) {
            native_result = original(handle, out, num, 1);
            native_called = 1;
            (void)__atomic_fetch_add(
                &args->native_backing_calls, 1, __ATOMIC_RELAXED);
            if (native_result < 0)
                (void)__atomic_fetch_add(
                    &args->native_backing_errors, 1,
                    __ATOMIC_RELAXED);
        }
        if (game_bridge_copy_direct(
                args, out, &args->read_ext_calls))
            return 1;
        return native_called ? native_result :
            (original ? original(handle, out, num, 1) : native_result);
    }
    if (native_called)
        return native_result;
    return original ? original(handle, out, num, 1) : native_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_get_data_internal_stub(int32_t handle, void *out,
                                GamePadBridgeArgs *args)
{
    typedef int32_t (*data_internal_fn)(int32_t, void *, int32_t);
    data_internal_fn original =
        (data_internal_fn)(uintptr_t)(
            args ? args->fp_data_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        game_bridge_handle_matches(args, handle) && out &&
        game_bridge_direct_available(args) &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (original) {
            native_result = original(handle, out, 1);
            native_called = 1;
            (void)__atomic_fetch_add(
                &args->native_backing_calls, 1, __ATOMIC_RELAXED);
            if (native_result < 0)
                (void)__atomic_fetch_add(
                    &args->native_backing_errors, 1,
                    __ATOMIC_RELAXED);
        }
        if (game_bridge_copy_direct(
                args, out, &args->data_internal_calls))
            return 0;
        return native_called ? native_result :
            (original ? original(handle, out, 1) : native_result);
    }
    if (native_called)
        return native_result;
    return original ? original(handle, out, 1) : native_result;
}

/*
 * ScePadControllerInformation (firmware 11.60, standard controller):
 *   +0x00 float touchpadDensity
 *   +0x04 uint16_t touchResolutionX
 *   +0x06 uint16_t touchResolutionY
 *   +0x08 uint8_t stickDeadzoneL
 *   +0x09 uint8_t stickDeadzoneR
 *   +0x0a uint8_t connectionType
 *   +0x0b uint8_t connectedCount
 *   +0x0c int32_t connected
 *   +0x10 int32_t deviceClass
 *   +0x14 uint8_t reserved[8]
 *
 * The game-local import hook calls Sony first and changes only the standard
 * metadata prefix. Once bridge publication is active, its state is the source
 * of truth: a transient result from the disconnected backing handle must not
 * leak through and make the title raise a controller dialog.
 */
__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_get_controller_info_stub(int32_t handle, void *out,
                                  GamePadBridgeArgs *args)
{
    typedef int32_t (*get_info_fn)(int32_t, void *);
    get_info_fn original = (get_info_fn)(uintptr_t)(
        args ? args->fp_get_controller_info_trampoline : 0);
    int32_t result = original
        ? original(handle, out) : (int32_t)0x80920001u;
    if (args)
        (void)__atomic_fetch_add(
            &args->controller_info_calls, 1, __ATOMIC_RELAXED);
    if (args &&
        args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        game_bridge_handle_matches(args, handle) && out &&
        game_bridge_direct_available(args)) {
        volatile uint8_t *info = (volatile uint8_t *)out;
        /* 1.0f pixels/mm-ish density; DS4 native 1920x943 touch range. */
        info[0] = 0x00;
        info[1] = 0x00;
        info[2] = 0x80;
        info[3] = 0x3f;
        info[4] = 0x80;
        info[5] = 0x07;
        info[6] = 0xaf;
        info[7] = 0x03;
        info[8] = 2;
        info[9] = 2;
        info[10] = 0; /* local controller */
        info[11] = 1;
        info[12] = 1;
        info[13] = 0;
        info[14] = 0;
        info[15] = 0;
        info[16] = 0; /* standard controller class */
        info[17] = 0;
        info[18] = 0;
        info[19] = 0;
        for (unsigned byte = 20; byte < 28; ++byte)
            info[byte] = 0;
        (void)__atomic_fetch_add(
            &args->controller_info_spoofs, 1, __ATOMIC_RELAXED);
        if (result != 0)
            (void)__atomic_fetch_add(
                &args->controller_info_result_overrides, 1,
                __ATOMIC_RELAXED);
        return 0;
    }
    return result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
void
game_pad_bridge_stub_end(void)
{
}

static int
remote_reader_copyout(pid_t pid, intptr_t addr, void *buf, size_t len)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)addr; (void)buf; (void)len;
    return -1;
#else
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid &&
        kernel_set_ucred_authid(self, 0x4800000000010003l) != 0)
        return -1;
    int result = mdbg_copyout(pid, addr, buf, len);
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    return result;
#endif
}

static int
remote_reader_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)buf; (void)addr; (void)len;
    return -1;
#else
    pid_t self = getpid();
    uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (saved_authid &&
        kernel_set_ucred_authid(self, 0x4800000000010003l) != 0)
        return -1;
    int result = mdbg_copyin(pid, buf, addr, len);
    if (saved_authid)
        kernel_set_ucred_authid(self, saved_authid);
    return result;
#endif
}

#define POORDS4_PG_FRAME UINT64_C(0x000ffffffffFF000)
#define POORDS4_X86_PG_VALID UINT64_C(0x1)
#define POORDS4_X86_PG_LARGE UINT64_C(0x80)

static int
game_bridge_translate_with_paging(
    uint64_t cr3, uint64_t dmap, uint64_t address,
    uint64_t *out_physical, uint64_t *out_physical_end)
{
    uint64_t table = cr3 & POORDS4_PG_FRAME;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t index = (address >> shift) & UINT64_C(0x1ff);
        uint64_t entry = 0;
        if (kernel_copyout(
                (intptr_t)(dmap + table + index * 8u),
                &entry, sizeof(entry)) != 0 ||
            !(entry & POORDS4_X86_PG_VALID))
            return -1;
        if ((entry & POORDS4_X86_PG_LARGE) || shift == 12) {
            uint64_t mask = (UINT64_C(1) << shift) - 1u;
            uint64_t physical =
                (entry & ~mask & UINT64_C(0x000fffffffffffff)) |
                (address & mask);
            *out_physical = physical;
            *out_physical_end = (physical | mask) + 1u;
            return 0;
        }
        table = entry & POORDS4_PG_FRAME;
    }
    return -1;
}

/* vmspace grew across firmware families (known vm_pmap offsets include
 * 0x2c0, 0x2e0, and 0x2e8).  Do not turn that layout detail into a firmware
 * lock. Discover the embedded pmap read-only and accept it only when exactly
 * one candidate translates a caller-supplied live user address to the same
 * bytes returned by mdbg. */
static int
game_bridge_get_process_paging(pid_t pid, intptr_t probe_address,
                               uint64_t *out_cr3, uint64_t *out_dmap)
{
    uint64_t proc = (uint64_t)kernel_get_proc(pid);
    uint64_t vmspace = 0;
    uint8_t expected[16];
    if (!proc || probe_address <= 0 || !out_cr3 || !out_dmap ||
        kernel_copyout(
            proc + (uint64_t)KERNEL_OFFSET_PROC_P_VMSPACE,
            &vmspace, sizeof(vmspace)) != 0 || !vmspace ||
        mdbg_copyout(pid, probe_address, expected,
                     sizeof(expected)) != 0)
        return -1;

    uint64_t selected_cr3 = 0;
    uint64_t selected_dmap = 0;
    uint64_t selected_offset = 0;
    int used_fallback = 0;
    unsigned matches = 0;
    for (uint64_t offset = UINT64_C(0x200);
         offset <= UINT64_C(0x380); offset += 8u) {
        uint64_t paging[2] = {0, 0};
        if (kernel_copyout(
                vmspace + offset + 32u,
                paging, sizeof(paging)) != 0)
            continue;
        uint64_t pml4 = paging[0];
        uint64_t cr3 = paging[1];
        if (pml4 <= cr3 || (pml4 >> 48) != UINT64_C(0xffff) ||
            (cr3 & POORDS4_PG_FRAME) == 0 ||
            (pml4 & UINT64_C(0xfff)) !=
                (cr3 & UINT64_C(0xfff)))
            continue;
        uint64_t dmap = pml4 - cr3;
        uint64_t physical = 0;
        uint64_t physical_end = 0;
        uint8_t translated[sizeof(expected)];
        if (game_bridge_translate_with_paging(
                cr3, dmap, (uint64_t)probe_address,
                &physical, &physical_end) != 0 ||
            physical_end - physical < sizeof(translated) ||
            kernel_copyout(
                (intptr_t)(dmap + physical), translated,
                sizeof(translated)) != 0 ||
            memcmp(expected, translated, sizeof(expected)) != 0)
            continue;
        selected_cr3 = cr3;
        selected_dmap = dmap;
        selected_offset = offset;
        matches++;
    }
    if (matches != 1u) {
        /* Preserve the already-proven family offsets as a compatibility
         * fallback. They are still validated against the live probe bytes;
         * the version alone never authorizes a physical access. */
        uint32_t version = kernel_get_fw_version() >> 16;
        uint64_t offset = 0;
        if (version >= 0x0600u && version <= 0x1270u)
            offset = UINT64_C(0x2e8);
        else if (version >= 0x0105u && version <= 0x0550u)
            offset = UINT64_C(0x2e0);
        else if (version >= 0x0100u && version <= 0x0102u)
            offset = UINT64_C(0x2c0);
        uint64_t paging[2] = {0, 0};
        uint64_t physical = 0;
        uint64_t physical_end = 0;
        uint8_t translated[sizeof(expected)];
        if (offset != 0 && kernel_copyout(
                vmspace + offset + 32u,
                paging, sizeof(paging)) == 0 &&
            paging[0] > paging[1] &&
            game_bridge_translate_with_paging(
                paging[1], paging[0] - paging[1],
                (uint64_t)probe_address,
                &physical, &physical_end) == 0 &&
            physical_end - physical >= sizeof(translated) &&
            kernel_copyout(
                (intptr_t)(paging[0] - paging[1] + physical),
                translated, sizeof(translated)) == 0 &&
            memcmp(expected, translated, sizeof(expected)) == 0) {
            selected_cr3 = paging[1];
            selected_dmap = paging[0] - paging[1];
            selected_offset = offset;
            used_fallback = 1;
            matches = 1;
        }
    }
    if (matches != 1u)
        return -1;
    *out_cr3 = selected_cr3;
    *out_dmap = selected_dmap;
    klog_printf(
        "[PoorDS4] paging layout pid=%d vm_pmap=0x%llx "
        "mode=%s live_probe=1\n",
        pid, (unsigned long long)selected_offset,
        used_fallback ? "validated-family-fallback" :
                        "dynamic-unique");
    return 0;
}

static int
game_bridge_process_virt_to_phys_entry(
    uint64_t address, uint64_t *out_physical,
    uint64_t *out_physical_end, uint64_t *out_entry)
{
    uint64_t table = g_game_bridge_direct_cr3 & POORDS4_PG_FRAME;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t index = (address >> shift) & UINT64_C(0x1ff);
        uint64_t entry = 0;
        if (kernel_copyout(
                (intptr_t)(g_game_bridge_direct_dmap + table + index * 8u),
                &entry, sizeof(entry)) != 0 ||
            !(entry & POORDS4_X86_PG_VALID))
            return -1;
        if ((entry & POORDS4_X86_PG_LARGE) || shift == 12) {
            uint64_t mask = (UINT64_C(1) << shift) - 1u;
            uint64_t physical =
                (entry & ~mask & UINT64_C(0x000fffffffffffff)) |
                (address & mask);
            *out_physical = physical;
            *out_physical_end = (physical | mask) + 1u;
            if (out_entry)
                *out_entry = entry;
            return 0;
        }
        table = entry & POORDS4_PG_FRAME;
    }
    return -1;
}

static int
game_bridge_process_virt_to_phys(
    uint64_t address, uint64_t *out_physical,
    uint64_t *out_physical_end)
{
    return game_bridge_process_virt_to_phys_entry(
        address, out_physical, out_physical_end, NULL);
}

/* Retail eboot processes allow mdbg reads but can reject live mdbg writes.
 * Resolve their already-faulted user pages and write through the kernel DMAP
 * instead. No ptrace attach and no game-side worker are needed per frame. */
static int
game_bridge_process_write(pid_t pid, intptr_t address,
                          const void *source, size_t length)
{
    if (pid <= 0 || address <= 0 || !source || length == 0)
        return -1;
    if (pid != g_game_bridge_direct_pid ||
        !g_game_bridge_direct_cr3 || !g_game_bridge_direct_dmap) {
        if (game_bridge_get_process_paging(
                pid, address, &g_game_bridge_direct_cr3,
                &g_game_bridge_direct_dmap) != 0)
            return -1;
        g_game_bridge_direct_pid = pid;
    }
    const uint8_t *bytes = (const uint8_t *)source;
    uint64_t cursor = (uint64_t)address;
    while (length != 0) {
        uint64_t physical = 0;
        uint64_t physical_end = 0;
        if (game_bridge_process_virt_to_phys(
                cursor, &physical, &physical_end) != 0 ||
            physical_end <= physical)
            return -1;
        size_t chunk = (size_t)(physical_end - physical);
        if (chunk > length)
            chunk = length;
        if (kernel_copyin(
                bytes,
                (intptr_t)(g_game_bridge_direct_dmap + physical),
                chunk) != 0)
            return -1;
        cursor += chunk;
        bytes += chunk;
        length -= chunk;
    }
    return 0;
}

static int
game_bridge_process_read(pid_t pid, intptr_t address,
                         void *destination, size_t length)
{
    if (pid <= 0 || address <= 0 || !destination || length == 0)
        return -1;
    if (pid != g_game_bridge_direct_pid ||
        !g_game_bridge_direct_cr3 || !g_game_bridge_direct_dmap) {
        if (game_bridge_get_process_paging(
                pid, address, &g_game_bridge_direct_cr3,
                &g_game_bridge_direct_dmap) != 0)
            return -1;
        g_game_bridge_direct_pid = pid;
    }
    uint8_t *bytes = (uint8_t *)destination;
    uint64_t cursor = (uint64_t)address;
    while (length != 0) {
        uint64_t physical = 0;
        uint64_t physical_end = 0;
        if (game_bridge_process_virt_to_phys(
                cursor, &physical, &physical_end) != 0 ||
            physical_end <= physical)
            return -1;
        size_t chunk = (size_t)(physical_end - physical);
        if (chunk > length)
            chunk = length;
        if (kernel_copyout(
                (intptr_t)(g_game_bridge_direct_dmap + physical),
                bytes, chunk) != 0)
            return -1;
        cursor += chunk;
        bytes += chunk;
        length -= chunk;
    }
    return 0;
}

/* The cleaned bridge keeps the proven bridge ABI v1 argument layout so a bridge left
 * by RC33 can be inspected and recovered without guessing another layout. */
static int
game_bridge_copy_args(pid_t pid, intptr_t args_address,
                      GamePadBridgeArgs *args)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_address; (void)args;
    return -1;
#else
    if (pid <= 0 || args_address <= 0 || !args)
        return -1;
    memset(args, 0, sizeof(*args));
    return remote_reader_copyout(
        pid, args_address, args, sizeof(*args));
#endif
}

static uint64_t
report_source_function_fingerprint(int fd, pid_t pid, intptr_t base,
                                   const char *name, intptr_t address)
{
    uint8_t code[256];
    memset(code, 0, sizeof(code));
    int copy_result = address > 0
        ? remote_reader_copyout(pid, address, code, sizeof(code)) : -1;
    report_printf(
        fd, "%s_address=0x%lx %s_offset=0x%lx %s_copy=%d "
        "%s_fnv256=0x%016llx\n",
        name, (unsigned long)address,
        name, address > 0 && base > 0
            ? (unsigned long)(address - base) : 0ul,
        name, copy_result,
        name, (unsigned long long)(copy_result == 0
            ? poords4_fnv1a64(code, sizeof(code)) : 0));
    report_printf(fd, "%s_prefix64=", name);
    if (copy_result == 0) {
        for (unsigned index = 0; index < 64; ++index)
            report_printf(fd, "%02x", code[index]);
    }
    report_printf(fd, "\n");
    return copy_result == 0
        ? poords4_fnv1a64(code, sizeof(code)) : 0;
}

int
wireless_ds4_remote_reader_start(
    const int32_t *user_ids, uint32_t user_count,
    PoorDS4PadSource *out_source, pid_t *out_pid,
    intptr_t *out_args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)user_ids; (void)user_count; (void)out_source;
    (void)out_pid; (void)out_args_kaddr;
    return -1;
#else
    pid_t pids[8];
    size_t count = find_pids("SceRemotePlay", pids, 8);
    pid_t target;
    int attached = 0;
    int launched = 0;
    int trap_saved = 0;
    int trap_writable = 0;
    int trap_protection = PROT_READ | PROT_EXEC;
    uint8_t original_trap_byte = 0;
    intptr_t trap_mem = 0;
    intptr_t probe_data_addr = 0;
    intptr_t probe_device_addr = 0;
    intptr_t remote_block = 0;
    intptr_t args_addr = 0;
    intptr_t stub_addr = 0;
    intptr_t thread_addr = 0;
    intptr_t fn_free = 0;
    intptr_t fn_pthread_detach = 0;
    intptr_t fn_kill = 0;
    int selected_opened_here = 0;

    if (out_pid) *out_pid = -1;
    if (out_args_kaddr) *out_args_kaddr = 0;
    if (out_source) {
        out_source->user_id = -1;
        out_source->pad_index = -1;
        out_source->pad_handle = -1;
        out_source->ds4_connected = 0;
    }
    g_source_pad_firmware = 0;
    memset(g_source_pad_fingerprints, 0,
           sizeof(g_source_pad_fingerprints));
    g_source_pad_fingerprint_mask = 0;
    g_source_controller_info_runtime_abi = 0;
    if (!user_ids || user_count == 0 ||
        user_count > POORDS4_MAX_USER_CANDIDATES)
        return -1;
    if (count == 0) {
        klog_printf("[PoorDS4] reader start: SceRemotePlay not found\n");
        return -1;
    }
    target = pids[0];

    klog_printf(
        "[PoorDS4] reader start: PT_ATTACH pid=%d users=%u "
        "process_matches=%zu\n", target, user_count, count);
    for (size_t process_index = 0; process_index < count; ++process_index)
        klog_printf("[PoorDS4] reader process[%zu]=%d\n",
                    process_index, pids[process_index]);
    if (sys_ptrace(PT_ATTACH, target, 0, 0) != 0) {
        klog_printf("[PoorDS4] reader start: attach failed errno=%d\n",
                    errno);
        return -1;
    }
    attached = 1;
    if (waitpid(target, NULL, 0) < 0)
        goto cleanup;

    uint32_t libpad_h = 0, libkernel_h = 0, libpthread_h = 0;
    uint32_t liblibc_h = 0;
    get_lib(target, "libScePad", &libpad_h);
    if (get_lib(target, "libkernel_sys", &libkernel_h) != 0)
        (void)get_lib(target, "libkernel", &libkernel_h);
    get_lib(target, "libpthread", &libpthread_h);
    get_lib(target, "libSceLibcInternal", &liblibc_h);

    intptr_t fn_gethandle = libpad_h
        ? resolve_sym(target, libpad_h, "scePadGetHandle") : 0;
    intptr_t fn_open = libpad_h
        ? resolve_sym(target, libpad_h, "scePadOpen") : 0;
    intptr_t fn_closepad = libpad_h
        ? resolve_sym(target, libpad_h, "scePadClose") : 0;
    intptr_t fn_isds4 = libpad_h
        ? resolve_sym(target, libpad_h, "scePadIsDS4Connected") : 0;
    intptr_t fn_getdeviceinfo = libpad_h
        ? resolve_sym(target, libpad_h, "scePadGetDeviceInfo") : 0;
    intptr_t fn_getdeviceid = libpad_h
        ? resolve_sym(target, libpad_h, "scePadGetDeviceId") : 0;
    intptr_t fn_readstate = libpad_h
        ? resolve_sym(target, libpad_h, "scePadReadState") : 0;
    intptr_t fn_readstate_ext = libpad_h
        ? resolve_sym(target, libpad_h, "scePadReadStateExt") : 0;
    intptr_t fn_read = libpad_h
        ? resolve_sym(target, libpad_h, "scePadRead") : 0;
    intptr_t fn_read_ext = libpad_h
        ? resolve_sym(target, libpad_h, "scePadReadExt") : 0;
    intptr_t fn_data_internal = libpad_h
        ? resolve_sym(target, libpad_h, "scePadGetDataInternal") : 0;
    intptr_t fn_controller_info = libpad_h
        ? resolve_sym(
            target, libpad_h, "scePadGetControllerInformation") : 0;
    intptr_t fn_setpriv = libpad_h
        ? resolve_sym(target, libpad_h, "scePadSetProcessPrivilege") : 0;
    intptr_t fn_usleep = libkernel_h
        ? resolve_sym(target, libkernel_h, "usleep") : 0;
    intptr_t fn_pthread_create = libkernel_h
        ? resolve_sym(target, libkernel_h, "pthread_create") : 0;
    fn_pthread_detach = libkernel_h
        ? resolve_sym(target, libkernel_h, "pthread_detach") : 0;
    fn_kill = libkernel_h
        ? resolve_sym(target, libkernel_h, "kill") : 0;
    intptr_t fn_malloc = liblibc_h
        ? resolve_sym(target, liblibc_h, "malloc") : 0;
    fn_free = liblibc_h
        ? resolve_sym(target, liblibc_h, "free") : 0;
    if (!fn_usleep && libpthread_h)
        fn_usleep = resolve_sym(target, libpthread_h, "usleep");
    if (!fn_pthread_create && libkernel_h)
        fn_pthread_create =
            resolve_sym(target, libkernel_h, "scePthreadCreate");
    if (!fn_pthread_create && libpthread_h)
        fn_pthread_create =
            resolve_sym(target, libpthread_h, "pthread_create");
    if (!fn_pthread_create && libpthread_h)
        fn_pthread_create =
            resolve_sym(target, libpthread_h, "scePthreadCreate");
    if (!fn_pthread_detach && libkernel_h)
        fn_pthread_detach =
            resolve_sym(target, libkernel_h, "scePthreadDetach");
    if (!fn_pthread_detach && libpthread_h)
        fn_pthread_detach =
            resolve_sym(target, libpthread_h, "pthread_detach");
    if (!fn_pthread_detach && libpthread_h)
        fn_pthread_detach =
            resolve_sym(target, libpthread_h, "scePthreadDetach");

    trap_mem = libpad_h ? kernel_dynlib_fini_addr(target, libpad_h) : 0;
    if (!trap_mem && libpad_h)
        trap_mem = kernel_dynlib_init_addr(target, libpad_h);
    uint32_t firmware = kernel_get_fw_version();
    intptr_t libpad_base = libpad_h
        ? kernel_dynlib_mapbase_addr(target, libpad_h) : 0;

    klog_printf("[PoorDS4] reader symbols: get=0x%lx open=0x%lx "
                "close=0x%lx isds4=0x%lx deviceid=0x%lx "
                "deviceinfo=0x%lx "
                "read=0x%lx sleep=0x%lx pthread=0x%lx detach=0x%lx "
                "kill=0x%lx malloc=0x%lx "
                "trap=0x%lx base=0x%lx fw=0x%08x\n",
                fn_gethandle, fn_open, fn_closepad, fn_isds4,
                fn_getdeviceid, fn_getdeviceinfo,
                fn_readstate, fn_usleep,
                fn_pthread_create, fn_pthread_detach, fn_kill,
                fn_malloc, trap_mem,
                libpad_base, firmware);
    {
        (void)mkdir("/data/poords4/reports", 0755);
        char source_report_path[160];
        int path_length = snprintf(
            source_report_path, sizeof(source_report_path),
            "/data/poords4/reports/source-fw-%08x-pid-%d.txt",
            firmware, target);
        int source_report_fd = path_length > 0 &&
            (size_t)path_length < sizeof(source_report_path)
                ? open(source_report_path,
                       O_WRONLY | O_CREAT | O_TRUNC, 0600)
                : -1;
        if (source_report_fd >= 0)
            report_printf(
                source_report_fd,
                "mode=read-only-source-libpad-fingerprint\n"
                "poords4_rc=%d\nreport_schema=5\n"
                "firmware=0x%08x\npid=%d\nlibpad_base=0x%lx\n",
                POORDS4_RC_VERSION, firmware, target,
                (unsigned long)libpad_base);

        static const char *const source_symbol_names[
            POORDS4_PAD_FINGERPRINT_COUNT] = {
            "read_state", "read_state_ext", "read", "read_ext",
            "data_internal", "controller_info"};
        const intptr_t source_symbol_addresses[
            POORDS4_PAD_FINGERPRINT_COUNT] = {
            fn_readstate, fn_readstate_ext, fn_read, fn_read_ext,
            fn_data_internal, fn_controller_info};
        g_source_pad_firmware = firmware;
        for (unsigned index = 0;
             index < POORDS4_PAD_FINGERPRINT_COUNT; ++index) {
            uint64_t fingerprint = report_source_function_fingerprint(
                source_report_fd, target, libpad_base,
                source_symbol_names[index], source_symbol_addresses[index]);
            g_source_pad_fingerprints[index] = fingerprint;
            if (fingerprint != 0)
                g_source_pad_fingerprint_mask |= UINT32_C(1) << index;
        }
        if (source_report_fd >= 0)
            close(source_report_fd);
        klog_printf(
            "[PoorDS4] source firmware report=%s open=%d "
            "fingerprint_mask=0x%02x\n",
            source_report_path, source_report_fd >= 0,
            g_source_pad_fingerprint_mask);
    }
    if (!fn_gethandle || !fn_open || !fn_closepad || !fn_isds4 ||
        !fn_getdeviceid || !fn_getdeviceinfo ||
        !fn_readstate || !fn_usleep || !fn_pthread_create ||
        !fn_kill || !fn_malloc || !trap_mem)
        goto cleanup;

    if (mdbg_copyout(target, trap_mem, &original_trap_byte, 1) != 0)
        goto cleanup;
    trap_saved = 1;
    {
        int protection = kernel_get_vmem_protection(target, trap_mem, 1);
        if (protection >= 0)
            trap_protection = protection;
    }
    if (kernel_set_vmem_protection(target, trap_mem, 16,
                                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto cleanup;
    trap_writable = 1;
    {
        uint8_t int3 = 0xcc;
        if (pt_io_write(target, trap_mem, &int3, 1) != 0)
            goto cleanup;
    }

    if (fn_setpriv) {
        int64_t result = pt_call(target, fn_setpriv, trap_mem,
                                 1, 0, 0, 0, 0, 0);
        klog_printf("[PoorDS4] reader setpriv=0x%llx\n",
                    (unsigned long long)(uint64_t)result);
    }

    pid_t owner_pid = getpid();
    int64_t owner_probe = pt_call(
        target, fn_kill, trap_mem, (uint32_t)owner_pid, 0, 0, 0, 0, 0);
    klog_printf(
        "[PoorDS4] reader owner liveness pid=%d probe=0x%llx\n",
        owner_pid, (unsigned long long)(uint64_t)owner_probe);
    if (owner_probe != 0)
        goto cleanup;

    probe_data_addr = (intptr_t)pt_call(
        target, fn_malloc, trap_mem,
        sizeof(ScePadData), 0, 0, 0, 0, 0);
    if (probe_data_addr <= 0)
        goto cleanup;
    probe_device_addr = (intptr_t)pt_call(
        target, fn_malloc, trap_mem, 256, 0, 0, 0, 0, 0);
    if (probe_device_addr <= 0)
        goto cleanup;

    int32_t handle = -1;
    int32_t selected_user = -1;
    int32_t selected_index = -1;
    int32_t selected_is_ds4 = 0;
    unsigned ds4_count = 0;
    for (uint32_t user_slot = 0; user_slot < user_count; ++user_slot) {
        int32_t candidate_user = user_ids[user_slot];
        if (candidate_user < 0)
            continue;
        for (int32_t pad_index = 0; pad_index < 8; ++pad_index) {
            int32_t candidate_handle = (int32_t)pt_call(
                target, fn_gethandle, trap_mem,
                (uint32_t)candidate_user, 0, (uint32_t)pad_index,
                0, 0, 0);
            int opened_here = 0;
            if (candidate_handle < 0) {
                candidate_handle = (int32_t)pt_call(
                    target, fn_open, trap_mem,
                    (uint32_t)candidate_user, 0,
                    (uint32_t)pad_index, 0, 0, 0);
                opened_here = candidate_handle >= 0;
            }
            int32_t is_ds4 = 0;
            if (candidate_handle >= 0)
                is_ds4 = (int32_t)pt_call(
                    target, fn_isds4, trap_mem,
                    (uint32_t)candidate_handle, 0, 0, 0, 0, 0);
            int32_t read_result = -1;
            int32_t read_connected = 0;
            if (candidate_handle >= 0) {
                ScePadData probe_data;
                memset(&probe_data, 0, sizeof(probe_data));
                if (pt_io_write(
                        target, probe_data_addr,
                        &probe_data, sizeof(probe_data)) == 0) {
                    read_result = (int32_t)pt_call(
                        target, fn_readstate, trap_mem,
                        (uint32_t)candidate_handle,
                        (uint64_t)probe_data_addr, 0, 0, 0, 0);
                    if (read_result == 0 &&
                        mdbg_copyout(
                            target, probe_data_addr,
                            &probe_data, sizeof(probe_data)) == 0)
                        read_connected = probe_data.connected != 0;
                }
            }
            uint8_t device_info[256];
            memset(device_info, 0xa5, sizeof(device_info));
            int32_t device_id = -1;
            int32_t device_id_result = -1;
            int32_t device_info_result = -1;
            int device_info_copy = -1;
            if (candidate_handle >= 0 && fn_getdeviceid &&
                pt_io_write(target, probe_device_addr,
                            &device_id, sizeof(device_id)) == 0) {
                device_id_result = (int32_t)pt_call(
                    target, fn_getdeviceid, trap_mem,
                    (uint32_t)candidate_handle,
                    (uint64_t)probe_device_addr, 0, 0, 0, 0);
                if (device_id_result == 0)
                    (void)mdbg_copyout(target, probe_device_addr,
                                       &device_id, sizeof(device_id));
            }
            if (device_id_result == 0 && device_id >= 0 &&
                fn_getdeviceinfo &&
                pt_io_write(target, probe_device_addr,
                            device_info, sizeof(device_info)) == 0) {
                device_info_result = (int32_t)pt_call(
                    target, fn_getdeviceinfo, trap_mem,
                    (uint32_t)device_id,
                    (uint64_t)probe_device_addr, 0, 0, 0, 0);
                if (device_info_result == 0)
                    device_info_copy = mdbg_copyout(
                        target, probe_device_addr,
                        device_info, sizeof(device_info));
            }
            int32_t table_connected = 0;
            uint16_t table_vendor = 0;
            uint16_t table_product = 0;
            int table_identity = 0;
            if (candidate_handle >= 0 &&
                firmware == POORDS4_GAME_BRIDGE_FW_1160 &&
                remote_pad_identity_1160(
                    target, libpad_base, candidate_handle,
                    &table_connected, &table_vendor,
                    &table_product) == 0) {
                table_identity = remote_pad_is_known_ds4(
                    table_connected, table_vendor, table_product);
            }
            uint16_t device_vendor = 0;
            uint16_t device_product = 0;
            if (device_info_copy == 0) {
                memcpy(&device_vendor, device_info + 16,
                       sizeof(device_vendor));
                memcpy(&device_product, device_info + 18,
                       sizeof(device_product));
            }
            int public_identity = device_info_result == 0 &&
                device_info_copy == 0 && read_connected &&
                remote_pad_is_known_ds4(
                    1, device_vendor, device_product);
            /* On retail 11.60 this API returns a positive handle-derived
             * value (for example 0x030d0301), not the literal boolean 1. */
            int api_identity = is_ds4 > 0;
            klog_printf(
                "[PoorDS4] source candidate user=0x%08x index=%d "
                "handle=0x%08x is_ds4=0x%08x opened=%d "
                "connected=%d vid=0x%04x pid=0x%04x "
                "read=0x%08x read_connected=%d "
                "device_id_call=0x%08x device_id=0x%08x "
                "device_info=0x%08x copy=%d device_vid=0x%04x "
                "device_pid=0x%04x prefix64=",
                (uint32_t)candidate_user, pad_index,
                (uint32_t)candidate_handle, (uint32_t)is_ds4,
                opened_here, table_connected, table_vendor, table_product,
                (uint32_t)read_result, read_connected,
                (uint32_t)device_id_result, (uint32_t)device_id,
                (uint32_t)device_info_result, device_info_copy,
                device_vendor, device_product);
            if (device_info_copy == 0) {
                for (unsigned byte = 0; byte < 64; ++byte)
                    klog_printf("%02x", device_info[byte]);
            }
            klog_printf("\n");
            /* A connected ScePadData sample proves that a handle is live, not
             * that it belongs to a DS4. RC22 treated every live 12.40 handle
             * as a DS4, so a DualSense connected first won enumeration. Only
             * GetDeviceId/GetDeviceInfo is the firmware-independent identity
             * path; the private 11.60 table remains a verified cross-check. */
            if (candidate_handle >= 0 &&
                (public_identity || api_identity || table_identity)) {
                ds4_count++;
                if (handle < 0) {
                    handle = candidate_handle;
                    selected_user = candidate_user;
                    selected_index = pad_index;
                    selected_is_ds4 = 1;
                    selected_opened_here = opened_here;
                    klog_printf(
                        "[PoorDS4] source match method=%s\n",
                        public_identity ? "public-device-info" :
                        (api_identity ? "api" : "fw1160-id"));
                } else if (opened_here && candidate_handle != handle) {
                    (void)pt_call(
                        target, fn_closepad, trap_mem,
                        (uint32_t)candidate_handle,
                        0, 0, 0, 0, 0);
                }
            } else if (opened_here) {
                /* Never retain or translate an unidentified controller. A
                 * later discovery pass will reopen it after association. */
                (void)pt_call(
                    target, fn_closepad, trap_mem,
                    (uint32_t)candidate_handle, 0, 0, 0, 0, 0);
            }
        }
    }
    uint8_t source_controller_probe[32];
    memset(source_controller_probe, 0xa5, sizeof(source_controller_probe));
    int32_t source_controller_call = -1;
    int source_controller_copy = -1;
    if (handle >= 0 && fn_controller_info && probe_device_addr > 0 &&
        pt_io_write(target, probe_device_addr, source_controller_probe,
                    sizeof(source_controller_probe)) == 0) {
        source_controller_call = (int32_t)pt_call(
            target, fn_controller_info, trap_mem,
            (uint32_t)handle, (uint64_t)probe_device_addr,
            0, 0, 0, 0);
        if (source_controller_call == 0)
            source_controller_copy = mdbg_copyout(
                target, probe_device_addr, source_controller_probe,
                sizeof(source_controller_probe));
    }
    uint16_t source_touch_x = 0;
    uint16_t source_touch_y = 0;
    int32_t source_connected = 0;
    int32_t source_device_class = -1;
    memcpy(&source_touch_x, source_controller_probe + 4,
           sizeof(source_touch_x));
    memcpy(&source_touch_y, source_controller_probe + 6,
           sizeof(source_touch_y));
    memcpy(&source_connected, source_controller_probe + 12,
           sizeof(source_connected));
    memcpy(&source_device_class, source_controller_probe + 16,
           sizeof(source_device_class));
    g_source_controller_info_runtime_abi =
        source_controller_call == 0 && source_controller_copy == 0 &&
        source_touch_x > 0 && source_touch_x <= 8192u &&
        source_touch_y > 0 && source_touch_y <= 8192u &&
        source_controller_probe[8] <= 127u &&
        source_controller_probe[9] <= 127u &&
        source_controller_probe[10] <= 8u &&
        source_connected > 0 && source_device_class >= 0 &&
        source_device_class <= 32;
    klog_printf(
        "[PoorDS4] source controller-info ABI=%d call=0x%08x "
        "copy=%d touch=%ux%u connection=%u connected=%d class=%d\n",
        g_source_controller_info_runtime_abi,
        (uint32_t)source_controller_call, source_controller_copy,
        source_touch_x, source_touch_y, source_controller_probe[10],
        source_connected, source_device_class);
    {
        char source_report_path[160];
        int path_length = snprintf(
            source_report_path, sizeof(source_report_path),
            "/data/poords4/reports/source-fw-%08x-pid-%d.txt",
            firmware, target);
        int source_report_fd = path_length > 0 &&
            (size_t)path_length < sizeof(source_report_path)
                ? open(source_report_path, O_WRONLY | O_APPEND, 0600)
                : -1;
        report_printf(
            source_report_fd,
            "controller_info_runtime_abi=%d call=0x%08x copy=%d "
            "touch=%ux%u deadzone=%u,%u connection=%u count=%u "
            "connected=%d class=%d\n",
            g_source_controller_info_runtime_abi,
            (uint32_t)source_controller_call, source_controller_copy,
            source_touch_x, source_touch_y, source_controller_probe[8],
            source_controller_probe[9], source_controller_probe[10],
            source_controller_probe[11], source_connected,
            source_device_class);
        if (source_report_fd >= 0)
            close(source_report_fd);
    }
    if (probe_data_addr > 0 && fn_free) {
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)probe_data_addr, 0, 0, 0, 0, 0);
        probe_data_addr = 0;
    }
    if (probe_device_addr > 0 && fn_free) {
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)probe_device_addr, 0, 0, 0, 0, 0);
        probe_device_addr = 0;
    }
    if (handle < 0)
        goto cleanup;
    klog_printf(
        "[PoorDS4] source selected user=0x%08x index=%d "
        "handle=0x%08x is_ds4=0x%08x matches=%u\n",
        (uint32_t)selected_user, selected_index, (uint32_t)handle,
        (uint32_t)selected_is_ds4, ds4_count);

    size_t stub_len =
        (size_t)((uintptr_t)remote_pad_reader_stub_end -
                 (uintptr_t)remote_pad_reader_stub);
    size_t stub_off = 16;
    size_t args_off = (stub_off + stub_len + 15) & ~(size_t)15;
    size_t thread_off =
        (args_off + sizeof(RemotePadReaderArgs) + 15) & ~(size_t)15;
    size_t alloc_size = thread_off + 16;

    remote_block = (intptr_t)pt_call(target, fn_malloc, trap_mem,
                                     alloc_size, 0, 0, 0, 0, 0);
    klog_printf("[PoorDS4] reader malloc(%zu)=0x%lx stub_len=%zu\n",
                alloc_size, remote_block, stub_len);
    if (remote_block <= 0)
        goto cleanup;
    if (kernel_set_vmem_protection(target, remote_block, alloc_size,
                                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        goto cleanup;

    stub_addr = remote_block + (intptr_t)stub_off;
    args_addr = remote_block + (intptr_t)args_off;
    thread_addr = remote_block + (intptr_t)thread_off;
    if (pt_io_write(target, stub_addr, remote_pad_reader_stub, stub_len) != 0)
        goto cleanup;

    RemotePadReaderArgs args;
    memset(&args, 0, sizeof(args));
    args.fp_readstate = fn_readstate;
    args.fp_usleep = fn_usleep;
    args.pad_handle = handle;
    args.interval_us = 8333;
    args.last_result = -1;
    args.fp_kill = fn_kill;
    args.fp_closepad = fn_closepad;
    args.owner_pid = owner_pid;
    args.close_pad_on_exit = selected_opened_here;
    args.owner_check_interval = 120;
    if (pt_io_write(target, args_addr, &args, sizeof(args)) != 0)
        goto cleanup;

    {
        int64_t result = pt_call(target, fn_pthread_create, trap_mem,
                                 (uint64_t)thread_addr, 0,
                                 (uint64_t)stub_addr, (uint64_t)args_addr,
                                 0, 0);
        klog_printf("[PoorDS4] reader pthread_create=0x%llx\n",
                    (unsigned long long)(uint64_t)result);
        if (result != 0)
            goto cleanup;
    }
    launched = 1;
    if (fn_pthread_detach) {
        uint64_t thread_id = 0;
        int thread_read = mdbg_copyout(
            target, thread_addr, &thread_id, sizeof(thread_id));
        int64_t detach_result = thread_read == 0 && thread_id != 0
            ? pt_call(target, fn_pthread_detach, trap_mem,
                      thread_id, 0, 0, 0, 0, 0)
            : -1;
        klog_printf(
            "[PoorDS4] reader thread detach read=%d id=0x%llx "
            "result=0x%llx\n",
            thread_read, (unsigned long long)thread_id,
            (unsigned long long)(uint64_t)detach_result);
    } else {
        klog_printf(
            "[PoorDS4] reader thread detach unavailable; "
            "owner watchdog remains enabled\n");
    }

cleanup:
    if (probe_device_addr > 0 && fn_free && trap_mem)
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)probe_device_addr, 0, 0, 0, 0, 0);
    if (probe_data_addr > 0 && fn_free && trap_mem)
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)probe_data_addr, 0, 0, 0, 0, 0);
    if (!launched && remote_block > 0 && fn_free && trap_mem)
        (void)pt_call(target, fn_free, trap_mem,
                      (uint64_t)remote_block, 0, 0, 0, 0, 0);
    if (!launched && selected_opened_here && handle >= 0 &&
        fn_closepad && trap_mem)
        (void)pt_call(target, fn_closepad, trap_mem,
                      (uint32_t)handle, 0, 0, 0, 0, 0);
    if (trap_saved && trap_writable)
        (void)pt_io_write(target, trap_mem, &original_trap_byte, 1);
    if (trap_writable)
        (void)kernel_set_vmem_protection(target, trap_mem, 16,
                                         trap_protection);
    if (attached)
        (void)sys_ptrace(PT_DETACH, target, 0, 0);

    if (!launched)
        return -1;

    for (unsigned attempt = 0; attempt < 500; attempt++) {
        RemotePadReaderArgs snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        if (remote_reader_copyout(target, args_addr, &snapshot,
                                  sizeof(snapshot)) == 0 &&
            snapshot.ready == 1 && snapshot.last_result == 0 &&
            snapshot.seq != 0 &&
            snapshot.pad_data[offsetof(ScePadData, connected)] != 0) {
            int identity_ok = 1;
            if (firmware == POORDS4_GAME_BRIDGE_FW_1160) {
                int32_t connected = 0;
                uint16_t vendor = 0;
                uint16_t product = 0;
                identity_ok = remote_pad_identity_1160(
                    target, libpad_base, handle,
                    &connected, &vendor, &product) == 0 &&
                    remote_pad_is_known_ds4(connected, vendor, product);
                klog_printf(
                    "[PoorDS4] async source identity connected=%d "
                    "vid=0x%04x pid=0x%04x accepted=%d\n",
                    connected, vendor, product, identity_ok);
            }
            if (!identity_ok) {
                klog_printf(
                    "[PoorDS4] reader rejected connected non-DS4 "
                    "handle=0x%08x\n", (uint32_t)handle);
                break;
            }
            if (out_pid) *out_pid = target;
            if (out_args_kaddr) *out_args_kaddr = args_addr;
            if (out_source) {
                out_source->user_id = selected_user;
                out_source->pad_index = selected_index;
                out_source->pad_handle = handle;
                out_source->ds4_connected = selected_is_ds4;
            }
            klog_printf("[PoorDS4] reader ready pid=%d args=0x%lx\n",
                        target, args_addr);
            return 0;
        }
        usleep(10000);
    }

    {
        RemotePadReaderArgs snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        (void)remote_reader_copyout(target, args_addr, &snapshot,
                                    sizeof(snapshot));
        klog_printf(
            "[PoorDS4] reader connected timeout ready=%d result=0x%08x "
            "seq=%u connected=%u\n", snapshot.ready,
            (uint32_t)snapshot.last_result, snapshot.seq,
            snapshot.pad_data[offsetof(ScePadData, connected)]);
    }
    (void)wireless_ds4_remote_reader_stop(target, args_addr);
    return -1;
#endif
}

int
wireless_ds4_remote_reader_read(pid_t pid, intptr_t args_kaddr,
                               void *pad_data, uint32_t pad_data_len,
                               uint32_t *out_seq)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_kaddr; (void)pad_data;
    (void)pad_data_len; (void)out_seq;
    return -1;
#else
    if (!pad_data || !args_kaddr)
        return -1;
    if (pad_data_len > POORDS4_REMOTE_PAD_CAPACITY)
        pad_data_len = POORDS4_REMOTE_PAD_CAPACITY;

    for (unsigned attempt = 0; attempt < 3; attempt++) {
        RemotePadReaderArgs snapshot;
        uint32_t seq_after = 0;
        if (remote_reader_copyout(pid, args_kaddr, &snapshot,
                                  sizeof(snapshot)) != 0)
            return -1;
        if (remote_reader_copyout(
                pid,
                args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, seq),
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        if ((snapshot.seq & 1u) == 0 && snapshot.seq == seq_after &&
            snapshot.ready == 1 &&
            snapshot.last_result == 0 && snapshot.seq != 0) {
            memcpy(pad_data, snapshot.pad_data, pad_data_len);
            if (out_seq) *out_seq = snapshot.seq;
            return 0;
        }
    }
    return -1;
#endif
}

int
wireless_ds4_remote_reader_status(
    pid_t pid, intptr_t args_kaddr,
    PoorDS4RemoteReaderStatus *out_status)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_kaddr; (void)out_status;
    return -1;
#else
    if (pid <= 0 || !args_kaddr || !out_status)
        return -1;
    RemotePadReaderArgs snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (remote_reader_copyout(
            pid, args_kaddr, &snapshot, sizeof(snapshot)) != 0 ||
        snapshot.fp_readstate <= 0 || snapshot.fp_usleep <= 0 ||
        snapshot.fp_kill <= 0 || snapshot.owner_pid <= 1 ||
        snapshot.owner_check_interval == 0)
        return -1;
    memset(out_status, 0, sizeof(*out_status));
    out_status->ready = snapshot.ready;
    out_status->stop = snapshot.stop;
    out_status->last_result = snapshot.last_result;
    out_status->seq = snapshot.seq;
    out_status->pad_handle = snapshot.pad_handle;
    out_status->owner_pid = snapshot.owner_pid;
    out_status->owner_check_interval = snapshot.owner_check_interval;
    out_status->owner_miss_count = snapshot.owner_miss_count;
    out_status->owner_watchdog_exits = snapshot.owner_watchdog_exits;
    out_status->close_pad_on_exit = snapshot.close_pad_on_exit;
    out_status->connected =
        snapshot.pad_data[offsetof(ScePadData, connected)];
    return 0;
#endif
}

int
wireless_ds4_remote_reader_stop(pid_t pid, intptr_t args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_kaddr;
    return -1;
#else
    int32_t stop = 1;
    intptr_t stop_addr =
        args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, stop);
    intptr_t ready_addr =
        args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, ready);
    int write_result =
        remote_reader_copyin(pid, &stop, stop_addr, sizeof(stop));
    int32_t observed_stop = 0;
    int read_result =
        remote_reader_copyout(pid, stop_addr, &observed_stop,
                              sizeof(observed_stop));
    klog_printf("[PoorDS4] reader stop: mdbg write=%d read=%d value=%d\n",
                write_result, read_result, observed_stop);

    if (write_result != 0 || read_result != 0 || observed_stop != 1) {
        klog_printf("[PoorDS4] reader stop: using one-time ptrace write\n");
        if (sys_ptrace(PT_ATTACH, pid, 0, 0) != 0)
            return -1;
        if (waitpid(pid, NULL, 0) < 0) {
            (void)sys_ptrace(PT_DETACH, pid, 0, 0);
            return -1;
        }
        int ptrace_result =
            pt_io_write(pid, stop_addr, &stop, sizeof(stop));
        (void)sys_ptrace(PT_DETACH, pid, 0, 0);
        if (ptrace_result != 0)
            return -1;
    }
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        int32_t ready = 0;
        if (remote_reader_copyout(pid, ready_addr, &ready,
                                  sizeof(ready)) == 0 && ready == 2)
            return 0;
        usleep(10000);
    }
    {
        int32_t ready = 0;
        int32_t final_stop = 0;
        (void)remote_reader_copyout(pid, ready_addr, &ready, sizeof(ready));
        (void)remote_reader_copyout(pid, stop_addr, &final_stop,
                                    sizeof(final_stop));
        klog_printf("[PoorDS4] reader stop timeout: ready=%d stop=%d\n",
                    ready, final_stop);
    }
    return -1;
#endif
}

#define POORDS4_READ_STATE_OFFSET_0860 UINT32_C(0x00002a10)
#define POORDS4_READ_STATE_FNV256_0860 UINT64_C(0xb39fa7c1c539da3c)
#define POORDS4_READ_OFFSET_0860 UINT32_C(0x00002a20)
#define POORDS4_READ_FNV256_0860 UINT64_C(0x95b5fbbe29e8dc2d)
#define POORDS4_READ_STATE_EXT_OFFSET_0860 UINT32_C(0x00002a30)
#define POORDS4_READ_STATE_EXT_FNV256_0860 UINT64_C(0x7dee2be11cea28a3)
#define POORDS4_READ_EXT_OFFSET_0860 UINT32_C(0x00002a40)
#define POORDS4_READ_EXT_FNV256_0860 UINT64_C(0x33f8eeb15ecdd836)
#define POORDS4_DATA_INTERNAL_OFFSET_0860 UINT32_C(0x00000e50)
#define POORDS4_DATA_INTERNAL_FNV256_0860 \
    UINT64_C(0x8bb51ee7c6a0b88f)
#define POORDS4_CONTROLLER_INFO_OFFSET_0860 UINT32_C(0x00004780)
#define POORDS4_CONTROLLER_INFO_FNV256_0860 \
    UINT64_C(0x419d0c07a15038b0)

#define POORDS4_READ_STATE_OFFSET_1160 UINT32_C(0x00002a80)
#define POORDS4_READ_STATE_FNV256_1160 UINT64_C(0xa4fea18d88eb7cc9)
#define POORDS4_READ_OFFSET_1160 UINT32_C(0x00002a90)
#define POORDS4_READ_FNV256_1160 UINT64_C(0x996f9a2e6fd4b22b)
#define POORDS4_READ_STATE_EXT_OFFSET_1160 UINT32_C(0x00002aa0)
#define POORDS4_READ_STATE_EXT_FNV256_1160 UINT64_C(0x54f5d565c9144b3e)
#define POORDS4_READ_EXT_OFFSET_1160 UINT32_C(0x00002ab0)
#define POORDS4_READ_EXT_FNV256_1160 UINT64_C(0x4d640c3e64e9029a)
#define POORDS4_DATA_INTERNAL_OFFSET_1160 UINT32_C(0x00000e10)
#define POORDS4_DATA_INTERNAL_FNV256_1160 \
    UINT64_C(0x644d37d059c8de86)
#define POORDS4_CONTROLLER_INFO_OFFSET_1160 UINT32_C(0x00004960)
#define POORDS4_CONTROLLER_INFO_FNV256_1160 \
    UINT64_C(0xb011e3f87d55e253)

#define POORDS4_READ_STATE_OFFSET_1240 UINT32_C(0x00002a60)
#define POORDS4_READ_STATE_FNV256_1240 UINT64_C(0x5167a88376045010)
#define POORDS4_READ_OFFSET_1240 UINT32_C(0x00002a70)
#define POORDS4_READ_FNV256_1240 UINT64_C(0x888290fbece09fd6)
#define POORDS4_READ_STATE_EXT_OFFSET_1240 UINT32_C(0x00002a80)
#define POORDS4_READ_STATE_EXT_FNV256_1240 UINT64_C(0x607ec10d42a3364f)
#define POORDS4_READ_EXT_OFFSET_1240 UINT32_C(0x00002a90)
#define POORDS4_READ_EXT_FNV256_1240 UINT64_C(0x9606b731866fd4cb)
#define POORDS4_DATA_INTERNAL_OFFSET_1240 UINT32_C(0x00000e10)
#define POORDS4_DATA_INTERNAL_FNV256_1240 \
    UINT64_C(0x5984e9f2ce099afe)
#define POORDS4_CONTROLLER_INFO_OFFSET_1240 UINT32_C(0x00004980)
#define POORDS4_CONTROLLER_INFO_FNV256_1240 \
    UINT64_C(0x6a2bda044bdb99bc)

enum {
    POORDS4_MANIFEST_NONE = 0,
    POORDS4_MANIFEST_STRUCTURAL = 1,
    POORDS4_MANIFEST_0860 = 860,
    POORDS4_MANIFEST_1160 = 1160,
    POORDS4_MANIFEST_1240 = 1240
};

static int
game_bridge_make_gateway(uint8_t gateway[16], intptr_t gateway_address,
                         intptr_t args_address, intptr_t stub_address,
                         int args_in_rcx)
{
    memset(gateway, 0x90, 16);
    gateway[0] = 0x48;
    gateway[1] = 0x8d;
    gateway[2] = args_in_rcx ? 0x0d : 0x15; /* lea rcx/rdx,[rip+disp32] */
    int64_t args_delta =
        (int64_t)args_address - (int64_t)(gateway_address + 7);
    int64_t stub_delta =
        (int64_t)stub_address - (int64_t)(gateway_address + 12);
    if (args_delta < INT32_MIN || args_delta > INT32_MAX ||
        stub_delta < INT32_MIN || stub_delta > INT32_MAX)
        return -1;
    int32_t args_rel = (int32_t)args_delta;
    int32_t stub_rel = (int32_t)stub_delta;
    memcpy(gateway + 3, &args_rel, sizeof(args_rel));
    gateway[7] = 0xe9;
    memcpy(gateway + 8, &stub_rel, sizeof(stub_rel));
    return 0;
}

/* Finite wrapper-shape parser used by the structural ABI gate and reports.
 * These short patterns are never sufficient alone: acceptance also requires
 * shared executable targets, an exact controller-info prologue, and a live
 * controller-info output-layout probe below. */
static int
game_bridge_nop_padding(const uint8_t *code, size_t length,
                        size_t *out_size)
{
    if (!code || !out_size || length == 0)
        return 0;
    if (code[0] == 0x90 || code[0] == 0xcc) {
        *out_size = 1;
        return 1;
    }
    static const uint8_t nops[][8] = {
        {0x66,0x90},
        {0x0f,0x1f,0x00},
        {0x0f,0x1f,0x40,0x00},
        {0x0f,0x1f,0x44,0x00,0x00},
        {0x66,0x0f,0x1f,0x44,0x00,0x00},
        {0x0f,0x1f,0x80,0x00,0x00,0x00,0x00},
        {0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00}
    };
    static const uint8_t sizes[] = {2,3,4,5,6,7,8};
    for (size_t index = 0; index < sizeof(sizes); ++index) {
        size_t size = sizes[index];
        if (length >= size && memcmp(code, nops[index], size) == 0) {
            *out_size = size;
            return 1;
        }
    }
    return 0;
}

/* Recognize the small ABI wrappers by their meaning, not only by the one
 * compiler spelling observed on 11.60.  kind 0/1 are the zeroing wrappers
 * for edx/ecx; kind 2/3/4 require the corresponding constant-one move.  The
 * accepted alternatives are deliberately finite and the tail must still be
 * only a direct/PLT jump plus padding. */
static int
game_bridge_report_wrapper_shape(const uint8_t *code, unsigned kind,
                                 size_t *out_jump_offset)
{
    if (!code || !out_jump_offset)
        return 0;
    size_t jump_offset = 0;
    if (kind == 0 || kind == 1) {
        uint8_t reg = kind == 0 ? 0xd2 : 0xc9;
        if (code[0] == 0x31 && code[1] == reg) {
            jump_offset = 2;
        } else if (code[0] == 0x29 && code[1] == reg) {
            /* sub edx,edx / sub ecx,ecx */
            jump_offset = 2;
        } else if (code[0] == (kind == 0 ? 0xba : 0xb9) &&
                   code[1] == 0x00 && code[2] == 0x00 &&
                   code[3] == 0x00 && code[4] == 0x00) {
            /* mov edx,0 / mov ecx,0 */
            jump_offset = 5;
        } else {
            return 0;
        }
    } else {
        uint8_t opcode = (kind == 2 || kind == 4) ? 0xba : 0xb9;
        if (code[0] != opcode || code[1] != 0x01 || code[2] != 0x00 ||
            code[3] != 0x00 || code[4] != 0x00)
            return 0;
        jump_offset = 5;
    }
    if (jump_offset + 5u > 16u)
        return 0;
    size_t jump_length = 0;
    if (code[jump_offset] == 0xe9) {
        jump_length = 5;
    } else if (jump_offset + 6u <= 16u &&
               code[jump_offset] == 0xff &&
               code[jump_offset + 1] == 0x25) {
        jump_length = 6;
    } else {
        return 0;
    }
    size_t padding = jump_offset + jump_length;
    while (padding < 16u) {
        size_t nop_size = 0;
        if (!game_bridge_nop_padding(code + padding, 16u - padding,
                                     &nop_size))
            return 0;
        padding += nop_size;
    }
    *out_jump_offset = jump_offset;
    return 1;
}

static int
game_bridge_target_within_sane_range(intptr_t base, intptr_t target)
{
    if (base <= 0 || target < base)
        return 0;
    uintptr_t delta = (uintptr_t)(target - base);
    /* Keep a malformed rel32 from escaping into another mapping. */
    return delta < (uintptr_t)0x01000000u;
}

static int
game_bridge_target_is_executable(pid_t pid, intptr_t target)
{
    for (unsigned attempt = 0; attempt < 3u; ++attempt) {
        int protection = kernel_get_vmem_protection(pid, target, 1);
        if (protection >= 0)
            return (protection & PROT_EXEC) != 0;
    }
    return 0;
}

/* Keep rejected-firmware reports useful to the person adding the next
 * manifest. Capture only bounded prefixes before any target write. */
static void
game_bridge_report_prefix(int fd, const char *name,
                          const uint8_t *code, size_t length)
{
    if (fd < 0 || !name || !code)
        return;
    size_t count = length < 64u ? length : 64u;
    report_printf(fd, "%s=", name);
    for (size_t index = 0; index < count; ++index)
        report_printf(fd, "%02x", code[index]);
    report_printf(fd, "\n");
}

/* Resolve either a direct rel32 wrapper or a six-byte RIP-relative PLT jump.
 * The latter is resolved while the target is stopped, so a stale or malformed
 * GOT entry cannot race the structural checks below. */
static int
game_bridge_wrapper_target(pid_t target, intptr_t function,
                           const uint8_t *code, size_t jump_offset,
                           intptr_t *out_target)
{
#if !defined(__PROSPERO__)
    (void)target; (void)function; (void)code; (void)jump_offset;
    (void)out_target;
    return -1;
#else
    if (!code || !out_target || jump_offset + 5u > 256u)
        return -1;
    if (code[jump_offset] == 0xe9) {
        int32_t displacement = 0;
        memcpy(&displacement, code + jump_offset + 1,
               sizeof(displacement));
        *out_target = function + (intptr_t)jump_offset + 5 + displacement;
        return *out_target > 0 ? 0 : -1;
    }
    if (jump_offset + 6u <= 256u &&
        code[jump_offset] == 0xff &&
        code[jump_offset + 1] == 0x25) {
        int32_t displacement = 0;
        memcpy(&displacement, code + jump_offset + 2,
               sizeof(displacement));
        intptr_t slot = function + (intptr_t)jump_offset + 6 + displacement;
        uint64_t pointer = 0;
        if (slot <= 0 || mdbg_copyout(
                target, slot, &pointer, sizeof(pointer)) != 0 ||
            pointer == 0 || pointer > (uint64_t)INTPTR_MAX)
            return -1;
        *out_target = (intptr_t)pointer;
        return 0;
    }
    return -1;
#endif
}

typedef struct {
    uint64_t next;
    uint64_t path;
    uint64_t unknown0[2];
    uint32_t refcount;
    uint32_t alignment0;
    uint64_t handle;
    uint64_t mapbase;
    uint64_t mapsize;
    uint64_t sections;
    uint64_t section_count;
    uint64_t unknown1;
    uint8_t reserved_to_dynsec[0x148u - 0x58u];
    uint64_t dynsec;
} GameBridgeDynlibObjectPrefix;

typedef struct {
    uint64_t descriptor;
    uint64_t address;
    uint64_t length;
} GameBridgeSharedLibSection;

enum {
    GAME_BRIDGE_SECTION_TEXT = 1,
    GAME_BRIDGE_SECTION_XOTEXT = 2,
    GAME_BRIDGE_SECTION_DATA = 16
};

typedef struct {
    uint64_t list_next;
    uint64_t list_prev;
    uint64_t sysvec;
    uint32_t refcount;
    uint32_t alignment0;
    uint64_t size;
    uint64_t symtab;
    uint64_t symtab_size;
    uint64_t strtab;
    uint64_t strtab_size;
    uint64_t plt_rela;
    uint64_t plt_rela_size;
} GameBridgeDynlibSectionPrefix;

typedef struct {
    intptr_t slot;
    intptr_t original;
    uint32_t protection;
    uint32_t kind;
} GameBridgeImportHook;

_Static_assert(offsetof(GameBridgeDynlibObjectPrefix, handle) == 0x28,
               "rtld object handle offset changed");
_Static_assert(offsetof(GameBridgeDynlibObjectPrefix, mapbase) == 0x30,
               "rtld object mapbase offset changed");
_Static_assert(offsetof(GameBridgeDynlibObjectPrefix, sections) == 0x40,
               "rtld object sections offset changed");
_Static_assert(offsetof(GameBridgeDynlibObjectPrefix, dynsec) == 0x148,
               "rtld object dynsec offset changed");
_Static_assert(sizeof(GameBridgeSharedLibSection) == 0x18,
               "rtld section size changed");
_Static_assert(offsetof(GameBridgeDynlibSectionPrefix, symtab) == 0x28,
               "rtld symtab offset changed");
_Static_assert(offsetof(GameBridgeDynlibSectionPrefix, plt_rela) == 0x48,
               "rtld PLT relocation offset changed");

static int game_bridge_collect_import_hooks(
    pid_t target, intptr_t libpad_base, const intptr_t originals[6],
    GameBridgeImportHook *hooks, uint32_t *out_count, int report_fd,
    int allow_nonoriginal, int include_modules);
static int game_cache_find_libpad_object(
    pid_t target, intptr_t base,
    GameBridgeDynlibObjectPrefix *out_object);
static int game_cache_find_table(
    pid_t target, intptr_t base, uint64_t mapsize,
    intptr_t read_state, intptr_t *out_table);

/* Read the same bounded rtld metadata prefixes used by the SDK's
 * kernel_dynlib helpers. Every followed size, index, pointer, and slot is
 * validated before a caller-owned import is changed. */
static int
game_bridge_symbol_kind(const char name[12], const char nids[6][12])
{
    if (!name || !nids)
        return -1;
    for (unsigned kind = 0; kind < 6; ++kind) {
        if (memcmp(name, nids[kind], 11) == 0)
            return (int)kind;
    }
    return -1;
}

static int
game_bridge_collect_import_hooks(
    pid_t target, intptr_t libpad_base, const intptr_t originals[6],
    GameBridgeImportHook *hooks, uint32_t *out_count, int report_fd,
    int allow_nonoriginal, int include_modules)
{
#if !defined(__PROSPERO__)
    (void)target; (void)libpad_base; (void)originals; (void)hooks;
    (void)out_count; (void)report_fd; (void)allow_nonoriginal;
    (void)include_modules;
    return -1;
#else
    if (target <= 0 || libpad_base <= 0 || !originals || !hooks ||
        !out_count)
        return -1;
    *out_count = 0;
    char nids[6][12];
    static const char *const names[6] = {
        "scePadReadState", "scePadReadStateExt", "scePadRead",
        "scePadReadExt", "scePadGetDataInternal",
        "scePadGetControllerInformation"};
    for (unsigned kind = 0; kind < 6; ++kind) {
        memset(nids[kind], 0, sizeof(nids[kind]));
        nid_encode(names[kind], nids[kind]);
    }

    uint64_t proc = (uint64_t)kernel_get_proc(target);
    uint64_t shared_object = 0;
    uint64_t object_address = 0;
    if (proc == 0) {
        report_printf(report_fd, "import_scan_error=proc\n");
        return -1;
    }
    if (kernel_copyout(
            proc + 0x3e8, &shared_object, sizeof(shared_object)) != 0 ||
        shared_object == 0) {
        report_printf(report_fd, "import_scan_error=shared_object\n");
        return -1;
    }
    if (kernel_copyout(
            shared_object, &object_address, sizeof(object_address)) != 0 ||
        object_address == 0) {
        report_printf(report_fd, "import_scan_error=first_object\n");
        return -1;
    }

    unsigned object_count = 0;
    unsigned relocation_count = 0;
    while (object_address != 0 && object_count++ < 512u) {
        GameBridgeDynlibObjectPrefix object;
        memset(&object, 0, sizeof(object));
        if (kernel_copyout(
                (intptr_t)object_address, &object, sizeof(object)) != 0) {
            report_printf(
                report_fd, "import_scan_error=object index=%u addr=0x%llx\n",
                object_count - 1u,
                (unsigned long long)object_address);
            return -1;
        }
        object_address = object.next;
        if (!object.mapbase || object.mapsize < sizeof(uint64_t) ||
            !object.dynsec ||
            object.mapbase == (uint64_t)libpad_base)
            continue;
        /* Only the eboot owns the lifecycle we track. Patching imports in a
         * backported/fakelib SPRX would couple PoorDS4 to ShadowMount's
         * union overlay and can leave a library hook alive past game cleanup. */
        if (!include_modules && object.handle != 0)
            continue;
        char object_path[192];
        memset(object_path, 0, sizeof(object_path));
        if (object.path)
            (void)mdbg_copyout(
                target, (intptr_t)object.path, object_path,
                sizeof(object_path) - 1u);
        GameBridgeDynlibSectionPrefix section;
        memset(&section, 0, sizeof(section));
        if (kernel_copyout(
                (intptr_t)object.dynsec, &section, sizeof(section)) != 0) {
            report_printf(
                report_fd,
                "import_scan_error=dynsec object=%u addr=0x%llx\n",
                object_count - 1u,
                (unsigned long long)object.dynsec);
            return -1;
        }
        if (!section.symtab || !section.strtab || !section.plt_rela ||
            section.symtab_size == 0 || section.strtab_size == 0 ||
            section.plt_rela_size == 0 ||
            section.symtab_size > UINT64_C(0x02000000) ||
            section.strtab_size > UINT64_C(0x02000000) ||
            section.plt_rela_size > UINT64_C(0x02000000) ||
            section.symtab_size % sizeof(Elf64_Sym) != 0 ||
            section.plt_rela_size % sizeof(Elf64_Rela) != 0)
            continue;
        uint64_t symbol_count = section.symtab_size / sizeof(Elf64_Sym);
        uint64_t rela_count = section.plt_rela_size / sizeof(Elf64_Rela);
        relocation_count += rela_count > UINT_MAX
            ? UINT_MAX : (unsigned)rela_count;
        for (uint64_t index = 0; index < rela_count; ++index) {
            Elf64_Rela rela;
            if (kernel_copyout(
                    (intptr_t)(section.plt_rela +
                        index * sizeof(rela)),
                    &rela, sizeof(rela)) != 0) {
                report_printf(
                    report_fd,
                    "import_scan_error=rela object=%u index=%llu\n",
                    object_count - 1u, (unsigned long long)index);
                return -1;
            }
            uint64_t symbol_index = ELF64_R_SYM(rela.r_info);
            if (symbol_index >= symbol_count)
                continue;
            Elf64_Sym symbol;
            if (kernel_copyout(
                    (intptr_t)(section.symtab +
                        symbol_index * sizeof(symbol)),
                    &symbol, sizeof(symbol)) != 0) {
                report_printf(
                    report_fd,
                    "import_scan_error=symbol object=%u index=%llu\n",
                    object_count - 1u,
                    (unsigned long long)symbol_index);
                return -1;
            }
            if ((uint64_t)symbol.st_name + 11u >= section.strtab_size)
                continue;
            char symbol_name[12];
            if (kernel_copyout(
                    (intptr_t)(section.strtab + symbol.st_name),
                    symbol_name, sizeof(symbol_name)) != 0) {
                report_printf(
                    report_fd,
                    "import_scan_error=name object=%u index=%llu\n",
                    object_count - 1u,
                    (unsigned long long)symbol_index);
                return -1;
            }
            int kind = game_bridge_symbol_kind(symbol_name, nids);
            if (kind < 0 || originals[kind] <= 0 ||
                rela.r_offset > object.mapsize - sizeof(uint64_t))
                continue;
            intptr_t slot = (intptr_t)(object.mapbase + rela.r_offset);
            uint64_t current = 0;
            if (slot <= 0 || mdbg_copyout(
                    target, slot, &current, sizeof(current)) != 0) {
                report_printf(
                    report_fd,
                    "import_slot_skipped unreadable module=0x%llx "
                    "slot=0x%lx kind=%d\n",
                    (unsigned long long)object.mapbase,
                    (unsigned long)slot, kind);
                continue;
            }
            /* BIND_NOW imports should already equal libScePad's export.
             * Refuse lazy/unexpected slots instead of overwriting a resolver
             * or a hook owned by another payload. */
            if (current != (uint64_t)originals[kind] &&
                !allow_nonoriginal) {
                report_printf(
                    report_fd,
                    "import_slot_skipped module=0x%llx slot=0x%lx "
                    "kind=%d current=0x%llx expected=0x%lx\n",
                    (unsigned long long)object.mapbase,
                    (unsigned long)slot, kind,
                    (unsigned long long)current,
                    (unsigned long)originals[kind]);
                continue;
            }
            int duplicate = 0;
            for (uint32_t prior = 0; prior < *out_count; ++prior) {
                if (hooks[prior].slot == slot) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (*out_count >= POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS) {
                report_printf(report_fd, "error=too_many_import_hooks\n");
                return -1;
            }
            int protection = kernel_get_vmem_protection(
                target, slot, sizeof(uint64_t));
            if (protection < 0)
                return -1;
            hooks[*out_count].slot = slot;
            hooks[*out_count].original = originals[kind];
            hooks[*out_count].protection = (uint32_t)protection;
            hooks[*out_count].kind = (uint32_t)kind;
            report_printf(
                report_fd,
                "import_hook index=%u object=%u module_handle=0x%x "
                "module_path=%s "
                "module_base=0x%llx slot=0x%lx kind=%d name=%s "
                "protection=0x%x current=0x%llx\n",
                *out_count, object_count - 1u, object.handle, object_path,
                (unsigned long long)object.mapbase,
                (unsigned long)slot, kind, names[kind], protection,
                (unsigned long long)current);
            (*out_count)++;
        }
    }
    report_printf(
        report_fd,
        "import_scan_objects=%u relocations=%u hooks=%u\n",
        object_count, relocation_count, *out_count);
    return *out_count > 0 ? 0 : -1;
#endif
}

/* Recover a bridge ABI v1 instance whose supervisor disappeared before it restored the
 * eboot import slots.  This path never stops the game and never invokes a
 * syscall in it: it proves that every unexpected slot belongs to one coherent
 * bridge ABI v1 target-anonymous mapping, then uses the physical-write quiesce path.
 *
 * Return 0 when no stale bridge exists, 1 after recovery, and -1 when an
 * unexpected import cannot be proven to belong to PoorDS4. */
static int
game_bridge_recover_stale_v1(
    pid_t target, intptr_t libpad_base, const intptr_t originals[6],
    GameBridgeImportHook hooks[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS],
    uint32_t *out_hook_count, int report_fd)
{
#if !defined(__PROSPERO__)
    (void)target; (void)libpad_base; (void)originals; (void)hooks;
    (void)out_hook_count; (void)report_fd;
    return -1;
#else
    if (!hooks || !out_hook_count)
        return -1;
    *out_hook_count = 0;
    memset(hooks, 0,
           sizeof(*hooks) * POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS);
    if (game_bridge_collect_import_hooks(
            target, libpad_base, originals, hooks, out_hook_count,
            report_fd, 1, 0) != 0)
        return 0;
    uint32_t hook_count = *out_hook_count;

    intptr_t args_address = 0;
    unsigned unexpected = 0;
    for (uint32_t index = 0; index < hook_count; ++index) {
        intptr_t current = 0;
        if (game_bridge_process_read(
                target, hooks[index].slot, &current,
                sizeof(current)) != 0)
            return -1;
        if (current == hooks[index].original)
            continue;
        unexpected++;
        uint8_t gateway[16];
        int32_t displacement = 0;
        if (current <= 0 || game_bridge_process_read(
                target, current, gateway, sizeof(gateway)) != 0 ||
            gateway[0] != 0x48 || gateway[1] != 0x8d ||
            (gateway[2] != 0x15 && gateway[2] != 0x0d) ||
            gateway[7] != 0xe9 || gateway[12] != 0x90 ||
            gateway[13] != 0x90 || gateway[14] != 0x90 ||
            gateway[15] != 0x90) {
            report_printf(
                report_fd,
                "stale_recovery_error=foreign_import index=%u "
                "slot=0x%lx current=0x%lx\n",
                index, (unsigned long)hooks[index].slot,
                (unsigned long)current);
            return -1;
        }
        memcpy(&displacement, gateway + 3, sizeof(displacement));
        intptr_t candidate = current + 7 + (intptr_t)displacement;
        if (candidate <= 0 ||
            (args_address != 0 && candidate != args_address)) {
            report_printf(
                report_fd,
                "stale_recovery_error=args_disagree index=%u "
                "candidate=0x%lx expected=0x%lx\n",
                index, (unsigned long)candidate,
                (unsigned long)args_address);
            return -1;
        }
        args_address = candidate;
    }
    if (unexpected == 0)
        return 0;

    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    const size_t expected_mapping_size =
        2u * (size_t)POORDS4_TARGET_PAGE_SIZE;
    if (game_bridge_copy_args(target, args_address, &args) != 0 ||
        args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
        args.pad_size != POORDS4_GAME_BRIDGE_PAD_SIZE ||
        args.reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1 ||
        args.remote_block <= 0 ||
        args.remote_block_size != expected_mapping_size ||
        ((uintptr_t)args.remote_block &
            (POORDS4_TARGET_PAGE_SIZE - 1u)) != 0 ||
        args_address != args.remote_block +
            (intptr_t)POORDS4_TARGET_PAGE_SIZE ||
        args.import_hook_count != hook_count ||
        args.import_hook_count == 0 ||
        args.import_hook_count > POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS ||
        args.read_state_address != originals[0] ||
        args.read_state_ext_address != originals[1] ||
        args.read_address != originals[2] ||
        args.read_ext_address != originals[3] ||
        args.data_internal_address != originals[4] ||
        args.controller_info_address != originals[5]) {
        report_printf(
            report_fd,
            "stale_recovery_error=args_validation args=0x%lx "
            "unexpected=%u hooks=%u saved=%u\n",
            (unsigned long)args_address, unexpected, hook_count,
            args.import_hook_count);
        return -1;
    }

    unsigned saved_unexpected = 0;
    for (uint32_t index = 0; index < args.import_hook_count; ++index) {
        uint32_t kind = args.import_hook_kinds[index];
        intptr_t slot = args.import_hook_slots[index];
        intptr_t original = args.import_hook_originals[index];
        intptr_t gateway_address = args.import_hook_gateways[index];
        if (kind >= 6u || slot <= 0 || original != originals[kind] ||
            gateway_address < args.remote_block ||
            gateway_address + 16 > args.remote_block +
                (intptr_t)POORDS4_TARGET_PAGE_SIZE) {
            report_printf(report_fd,
                          "stale_recovery_error=saved_hook index=%u\n",
                          index);
            return -1;
        }
        for (uint32_t prior = 0; prior < index; ++prior) {
            if (args.import_hook_slots[prior] == slot) {
                report_printf(
                    report_fd,
                    "stale_recovery_error=duplicate_slot index=%u\n",
                    index);
                return -1;
            }
        }

        int found = 0;
        for (uint32_t candidate = 0; candidate < hook_count; ++candidate) {
            if (hooks[candidate].slot == slot &&
                hooks[candidate].kind == kind &&
                hooks[candidate].original == original) {
                found = 1;
                break;
            }
        }
        intptr_t current = 0;
        uint8_t gateway[16];
        int32_t displacement = 0;
        if (!found || game_bridge_process_read(
                target, slot, &current, sizeof(current)) != 0 ||
            (current != original && current != gateway_address) ||
            game_bridge_process_read(
                target, gateway_address, gateway, sizeof(gateway)) != 0 ||
            gateway[0] != 0x48 || gateway[1] != 0x8d ||
            (gateway[2] != 0x15 && gateway[2] != 0x0d) ||
            gateway[7] != 0xe9) {
            report_printf(
                report_fd,
                "stale_recovery_error=ownership index=%u "
                "slot=0x%lx current=0x%lx gateway=0x%lx\n",
                index, (unsigned long)slot, (unsigned long)current,
                (unsigned long)gateway_address);
            return -1;
        }
        memcpy(&displacement, gateway + 3, sizeof(displacement));
        if (gateway_address + 7 + (intptr_t)displacement != args_address) {
            report_printf(
                report_fd,
                "stale_recovery_error=gateway_args index=%u\n", index);
            return -1;
        }
        if (current == gateway_address)
            saved_unexpected++;
    }
    if (saved_unexpected != unexpected) {
        report_printf(
            report_fd,
            "stale_recovery_error=unexpected_count imports=%u saved=%u\n",
            unexpected, saved_unexpected);
        return -1;
    }

    int recovery = wireless_ds4_game_bridge_quiesce(
        target, args_address);
    report_printf(
        report_fd,
        "stale_recovery=layout-v1-no-ptrace args=0x%lx imports=%u result=%d\n",
        (unsigned long)args_address, hook_count, recovery);
    klog_printf(
        "[PoorDS4] stale layout-v1 recovery pid=%d args=0x%lx imports=%u "
        "result=%d mapping_retained_until_game_exit=1\n",
        target, (unsigned long)args_address, hook_count, recovery);
    return recovery == 0 ? 1 : -1;
#endif
}

static int
game_bridge_select_pad_handle(
    pid_t target, intptr_t libpad_base, intptr_t read_state,
    int32_t source_user_id, int32_t source_pad_index, int report_fd,
    int32_t *out_handle, int32_t *out_index)
{
    if (target <= 0 || libpad_base <= 0 || read_state <= 0 ||
        source_user_id < 0 || source_pad_index < 0 ||
        source_pad_index >= 8 ||
        !out_handle || !out_index)
        return -1;

    *out_handle = -1;
    *out_index = -1;
    GameBridgeDynlibObjectPrefix object;
    memset(&object, 0, sizeof(object));
    intptr_t table = 0;
    if (game_cache_find_libpad_object(
            target, libpad_base, &object) != 0) {
        report_printf(report_fd, "error=libpad_object_discovery\n");
        return -1;
    }
    report_printf(
        report_fd,
        "libpad_object mapbase=0x%llx mapsize=0x%llx "
        "sections=%llu refcount=%u\n",
        (unsigned long long)object.mapbase,
        (unsigned long long)object.mapsize,
        (unsigned long long)object.section_count, object.refcount);
    if (game_cache_find_table(
            target, libpad_base, object.mapsize,
            read_state, &table) != 0) {
        report_printf(report_fd, "error=pad_client_table_discovery\n");
        return -1;
    }

    int32_t sole_handle = -1;
    int32_t sole_index = -1;
    int32_t unique_ds4_handle = -1;
    int32_t unique_ds4_index = -1;
    int32_t unique_inactive_handle = -1;
    int32_t unique_inactive_index = -1;
    int32_t identity_inactive_handle = -1;
    int32_t identity_inactive_index = -1;
    unsigned active_count = 0;
    unsigned indexed_count = 0;
    unsigned indexed_ds4_count = 0;
    unsigned indexed_inactive_count = 0;
    unsigned identity_count = 0;
    unsigned identity_inactive_count = 0;
    unsigned ds4_count = 0;
    unsigned inactive_count = 0;
    report_printf(
        report_fd, "pad_client_table=0x%lx mapsize=0x%llx\n",
        (unsigned long)table, (unsigned long long)object.mapsize);

    for (unsigned slot = 0; slot < 24u; ++slot) {
        intptr_t entry = table + (intptr_t)slot *
            POORDS4_PAD_CLIENT_STRIDE_1160;
        int32_t connected = 0;
        int32_t handle = 0;
        int32_t user_id = -1;
        int32_t valid = 0;
        uint16_t vendor = 0;
        uint16_t product = 0;
        if (game_bridge_process_read(
                target, entry + POORDS4_PAD_CLIENT_CONNECTED_1160,
                &connected, sizeof(connected)) != 0 ||
            game_bridge_process_read(
                target, entry + POORDS4_PAD_CLIENT_HANDLE_1160,
                &handle, sizeof(handle)) != 0 ||
            game_bridge_process_read(
                target, entry + POORDS4_PAD_CLIENT_USER_ID_1160,
                &user_id, sizeof(user_id)) != 0 ||
            game_bridge_process_read(
                target, entry + 0x38, &valid, sizeof(valid)) != 0 ||
            game_bridge_process_read(
                target, entry + POORDS4_PAD_CLIENT_VENDOR_1160,
                &vendor, sizeof(vendor)) != 0 ||
            game_bridge_process_read(
                target, entry + POORDS4_PAD_CLIENT_PRODUCT_1160,
                &product, sizeof(product)) != 0) {
            report_printf(report_fd, "error=pad_client_read slot=%u\n",
                          slot);
            return -1;
        }
        if (handle <= 0)
            continue;

        /* Preserve enough per-entry structure to diagnose a future firmware
         * without requiring a special payload or a second reproduction. */
        uint8_t entry_prefix[128];
        uint8_t pad_cache_prefix[64];
        if (game_bridge_process_read(
                target, entry, entry_prefix,
                sizeof(entry_prefix)) == 0 &&
            game_bridge_process_read(
                target, entry + 0x4b8, pad_cache_prefix,
                sizeof(pad_cache_prefix)) == 0) {
            char prefix_name[64];
            snprintf(prefix_name, sizeof(prefix_name),
                     "pad_client_%u_header_0000", slot);
            game_bridge_report_prefix(
                report_fd, prefix_name, entry_prefix, 64u);
            snprintf(prefix_name, sizeof(prefix_name),
                     "pad_client_%u_header_0040", slot);
            game_bridge_report_prefix(
                report_fd, prefix_name, entry_prefix + 64u, 64u);
            snprintf(prefix_name, sizeof(prefix_name),
                     "pad_client_%u_cache_04b8", slot);
            game_bridge_report_prefix(
                report_fd, prefix_name, pad_cache_prefix,
                sizeof(pad_cache_prefix));
        } else {
            report_printf(report_fd,
                          "pad_client_prefix_error slot=%u\n", slot);
        }

        int32_t inferred_index = handle & 0xff;
        int is_ds4 = remote_pad_is_known_ds4(
            connected, vendor, product);
        report_printf(
            report_fd,
            "pad_client slot=%u entry=0x%lx handle=0x%08x "
            "user_id=0x%08x inferred_index=%d connected=%d valid=%d "
            "vid=0x%04x pid=0x%04x ds4=%d\n",
            slot, (unsigned long)entry, (uint32_t)handle,
            (uint32_t)user_id,
            inferred_index, connected, valid, vendor, product, is_ds4);

        active_count++;
        sole_handle = handle;
        sole_index = inferred_index;
        if (is_ds4) {
            ds4_count++;
            unique_ds4_handle = handle;
            unique_ds4_index = inferred_index;
        }
        if (!connected && !valid) {
            inactive_count++;
            unique_inactive_handle = handle;
            unique_inactive_index = inferred_index;
        }
        if (inferred_index == source_pad_index) {
            indexed_count++;
            if (is_ds4)
                indexed_ds4_count++;
            if (!connected && !valid)
                indexed_inactive_count++;
        }
        if (user_id == source_user_id &&
            inferred_index == source_pad_index) {
            identity_count++;
            if (!connected && !valid) {
                identity_inactive_count++;
                identity_inactive_handle = handle;
                identity_inactive_index = inferred_index;
            }
        }
    }

    const char *method = "none";
    /* pad_index is local to one user. It is not the game's P1/P2 slot: two
     * logged-in users normally both expose source index zero. The game table
     * is global and orders its handles by player slot. Native PS5 titles keep
     * each unsupported DS4-facing slot allocated but disconnected, while
     * real DualSense slots are connected. The (user,index) pair identifies
     * one controller even when a title preallocates several inactive slots;
     * an index alone is not global and is never used as a fallback. */
    if (ds4_count == 1u) {
        *out_handle = unique_ds4_handle;
        *out_index = unique_ds4_index;
        method = "global-exact-ds4";
    } else if (ds4_count == 0u && identity_count == 1u &&
               identity_inactive_count == 1u) {
        *out_handle = identity_inactive_handle;
        *out_index = identity_inactive_index;
        method = "source-user-index-inactive";
    } else if (ds4_count == 0u && inactive_count == 1u) {
        *out_handle = unique_inactive_handle;
        *out_index = unique_inactive_index;
        method = "global-inactive-unique";
    } else if (ds4_count == 0u && inactive_count == 0u &&
               active_count == 1u && identity_count == 1u) {
        *out_handle = sole_handle;
        *out_index = sole_index;
        method = "sole-entry";
    }
    report_printf(
        report_fd,
        "pad_selection=%s handle=0x%08x index=%d "
        "active=%u ds4=%u inactive=%u source_indexed=%u "
        "source_indexed_ds4=%u source_indexed_inactive=%u "
        "source_identity=%u source_identity_inactive=%u\n",
        method, (uint32_t)*out_handle, *out_index,
        active_count, ds4_count, inactive_count, indexed_count,
        indexed_ds4_count,
        indexed_inactive_count, identity_count,
        identity_inactive_count);
    return *out_handle >= 0 ? 0 : -1;
}

static int
wireless_ds4_game_bridge_run_passive(
    const PoorDS4PadSource *source, pid_t *out_game_pid,
    intptr_t *out_args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)source; (void)out_game_pid; (void)out_args_kaddr;
    return -1;
#else
    pid_t pids[8];
    int report_fd = -1;
    int result = -1;
    uint32_t patched_count = 0;
    GameBridgeImportHook hooks[POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS];
    uint32_t hook_count = 0;
    uint32_t firmware = kernel_get_fw_version();
    intptr_t remote_mapping = 0;
    size_t remote_mapping_size = 0;
    if (out_game_pid)
        *out_game_pid = -1;
    if (out_args_kaddr)
        *out_args_kaddr = 0;
    if (!source || source->user_id < 0 ||
        source->pad_index < 0 || source->pad_index >= 8)
        return -1;

    size_t process_count = find_pids("eboot.bin", pids, 8);
    if (process_count == 0)
        return -2;
    if (process_count != 1)
        return -3;
    pid_t target = pids[0];
    if (out_game_pid)
        *out_game_pid = target;

    mkdir("/data/poords4", 0755);
    mkdir("/data/poords4/reports", 0755);
    (void)unlink("/data/poords4/game-pad-bridge-last.txt");
    report_fd = open(
        "/data/poords4/game-pad-bridge-last.txt",
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    char archive_path[160];
    int archive_length = snprintf(
        archive_path, sizeof(archive_path),
        "/data/poords4/reports/fw-%08x-pid-%d.txt",
        firmware, target);
    if (archive_length > 0 &&
        (size_t)archive_length < sizeof(archive_path))
        g_report_archive_fd = open(
            archive_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    report_printf(
        report_fd,
        "firmware=0x%08x\npid=%d\nuser_id=0x%08x\n"
        "source_pad_index=%d\nsource_handle=0x%08x\n"
        "source_is_ds4=0x%08x\n"
        "poords4_rc=%d\nreport_schema=5\n"
        "install_mode=passive-target-anonymous\nptrace_calls=0\n"
        "remote_game_calls=0\ngame_heap_allocations=0\n",
        firmware, target, (uint32_t)source->user_id,
        source->pad_index, (uint32_t)source->pad_handle,
        (uint32_t)source->ds4_connected, POORDS4_RC_VERSION);

    uint32_t libpad_handle = 0;
    if (get_lib(target, "libScePad", &libpad_handle) != 0) {
        report_printf(report_fd, "error=libpad\n");
        goto done;
    }
    intptr_t base = kernel_dynlib_mapbase_addr(target, libpad_handle);
    intptr_t originals[6] = {
        resolve_sym(target, libpad_handle, "scePadReadState"),
        resolve_sym(target, libpad_handle, "scePadReadStateExt"),
        resolve_sym(target, libpad_handle, "scePadRead"),
        resolve_sym(target, libpad_handle, "scePadReadExt"),
        resolve_sym(target, libpad_handle, "scePadGetDataInternal"),
        resolve_sym(target, libpad_handle,
                    "scePadGetControllerInformation")
    };
    uint8_t fingerprints[6][256];
    memset(fingerprints, 0, sizeof(fingerprints));
    for (unsigned index = 0; index < 6; ++index) {
        if (!base || originals[index] <= 0 ||
            mdbg_copyout(target, originals[index], fingerprints[index],
                         sizeof(fingerprints[index])) != 0) {
            report_printf(report_fd, "error=resolve index=%u\n", index);
            goto done;
        }
    }
    static const char *const symbol_names[6] = {
        "read_state", "read_state_ext", "read", "read_ext",
        "data_internal", "controller_info"};
    report_printf(report_fd, "libpad_handle=0x%x libpad_base=0x%lx\n",
                  libpad_handle, (unsigned long)base);
    const uint32_t offsets_1160[6] = {
        POORDS4_READ_STATE_OFFSET_1160,
        POORDS4_READ_STATE_EXT_OFFSET_1160,
        POORDS4_READ_OFFSET_1160,
        POORDS4_READ_EXT_OFFSET_1160,
        POORDS4_DATA_INTERNAL_OFFSET_1160,
        POORDS4_CONTROLLER_INFO_OFFSET_1160
    };
    const uint64_t hashes_1160[6] = {
        POORDS4_READ_STATE_FNV256_1160,
        POORDS4_READ_STATE_EXT_FNV256_1160,
        POORDS4_READ_FNV256_1160,
        POORDS4_READ_EXT_FNV256_1160,
        POORDS4_DATA_INTERNAL_FNV256_1160,
        POORDS4_CONTROLLER_INFO_FNV256_1160
    };
    const uint32_t offsets_0860[6] = {
        POORDS4_READ_STATE_OFFSET_0860,
        POORDS4_READ_STATE_EXT_OFFSET_0860,
        POORDS4_READ_OFFSET_0860,
        POORDS4_READ_EXT_OFFSET_0860,
        POORDS4_DATA_INTERNAL_OFFSET_0860,
        POORDS4_CONTROLLER_INFO_OFFSET_0860
    };
    const uint64_t hashes_0860[6] = {
        POORDS4_READ_STATE_FNV256_0860,
        POORDS4_READ_STATE_EXT_FNV256_0860,
        POORDS4_READ_FNV256_0860,
        POORDS4_READ_EXT_FNV256_0860,
        POORDS4_DATA_INTERNAL_FNV256_0860,
        POORDS4_CONTROLLER_INFO_FNV256_0860
    };
    const uint32_t offsets_1240[6] = {
        POORDS4_READ_STATE_OFFSET_1240,
        POORDS4_READ_STATE_EXT_OFFSET_1240,
        POORDS4_READ_OFFSET_1240,
        POORDS4_READ_EXT_OFFSET_1240,
        POORDS4_DATA_INTERNAL_OFFSET_1240,
        POORDS4_CONTROLLER_INFO_OFFSET_1240
    };
    const uint64_t hashes_1240[6] = {
        POORDS4_READ_STATE_FNV256_1240,
        POORDS4_READ_STATE_EXT_FNV256_1240,
        POORDS4_READ_FNV256_1240,
        POORDS4_READ_EXT_FNV256_1240,
        POORDS4_DATA_INTERNAL_FNV256_1240,
        POORDS4_CONTROLLER_INFO_FNV256_1240
    };
    const uint32_t *expected_offsets =
        firmware == POORDS4_GAME_BRIDGE_FW_0860 ? offsets_0860 :
        firmware == POORDS4_GAME_BRIDGE_FW_1160 ? offsets_1160 :
        firmware == POORDS4_GAME_BRIDGE_FW_1240 ? offsets_1240 : NULL;
    const uint64_t *expected_hashes =
        firmware == POORDS4_GAME_BRIDGE_FW_0860 ? hashes_0860 :
        firmware == POORDS4_GAME_BRIDGE_FW_1160 ? hashes_1160 :
        firmware == POORDS4_GAME_BRIDGE_FW_1240 ? hashes_1240 : NULL;
    int exact_manifest = expected_offsets && expected_hashes;
    for (unsigned index = 0; index < 6; ++index) {
        uint32_t offset = (uint32_t)(originals[index] - base);
        uint64_t hash = poords4_fnv1a64(
            fingerprints[index], sizeof(fingerprints[index]));
        report_printf(
            report_fd,
            "pad_symbol=%u name=%s address=0x%lx offset=0x%x "
            "hash=0x%016llx\n",
            index, symbol_names[index],
            (unsigned long)originals[index], offset,
            (unsigned long long)hash);
        char prefix_name[48];
        snprintf(prefix_name, sizeof(prefix_name), "%s_prefix64",
                 symbol_names[index]);
        game_bridge_report_prefix(
            report_fd, prefix_name, fingerprints[index],
            sizeof(fingerprints[index]));
        if (exact_manifest &&
            (offset != expected_offsets[index] ||
             hash != expected_hashes[index]))
            exact_manifest = 0;
    }
    if (GC_FORCE_STRUCTURAL_FIRMWARE)
        exact_manifest = 0;

    /* Exact hashes remain useful evidence, but the bridge does not depend on
     * fixed addresses. Admit another firmware only when its public wrapper
     * ABI proves the same relationships read-only. This is the structural
     * gate used by the older implementation, minus its forbidden game ptrace
     * probe. The public ScePadControllerInformation layout is guarded either
     * by the known prologue or by an actual successful call on the live DS4
     * in RemotePlay plus identical libScePad fingerprints in the game. */
    static const uint8_t controller_info_prologue[20] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
        0xd8,0x00,0x00,0x00};
    static const unsigned wrapper_kinds[5] = {0, 2, 1, 3, 4};
    size_t jump_offsets[5] = {0, 0, 0, 0, 0};
    intptr_t wrapper_targets[5] = {0, 0, 0, 0, 0};
    int wrapper_shapes_ok[5] = {0, 0, 0, 0, 0};
    int wrapper_targets_ok[5] = {0, 0, 0, 0, 0};
    int wrapper_targets_exec[5] = {0, 0, 0, 0, 0};
    int structural_abi = 1;
    for (unsigned index = 0; index < 5; ++index) {
        wrapper_shapes_ok[index] = game_bridge_report_wrapper_shape(
            fingerprints[index], wrapper_kinds[index],
            &jump_offsets[index]);
        wrapper_targets_ok[index] = wrapper_shapes_ok[index] &&
            game_bridge_wrapper_target(
                target, originals[index], fingerprints[index],
                jump_offsets[index], &wrapper_targets[index]) == 0 &&
            game_bridge_target_within_sane_range(
                base, wrapper_targets[index]);
        if (!wrapper_shapes_ok[index] || !wrapper_targets_ok[index])
            structural_abi = 0;
    }
    int common_state_target =
        wrapper_targets[0] == wrapper_targets[1];
    int common_read_target =
        wrapper_targets[2] == wrapper_targets[3];
    int state_target_exec = common_state_target &&
        game_bridge_target_is_executable(target, wrapper_targets[0]);
    int read_target_exec = common_read_target &&
        game_bridge_target_is_executable(target, wrapper_targets[2]);
    int data_target_exec = wrapper_targets_ok[4] &&
        game_bridge_target_is_executable(target, wrapper_targets[4]);
    wrapper_targets_exec[0] = wrapper_targets_exec[1] =
        state_target_exec;
    wrapper_targets_exec[2] = wrapper_targets_exec[3] =
        read_target_exec;
    wrapper_targets_exec[4] = data_target_exec;
    int controller_info_prefix_ok =
        memcmp(fingerprints[5], controller_info_prologue,
               sizeof(controller_info_prologue)) == 0;
    int controller_info_exec =
        game_bridge_target_is_executable(target, originals[5]);
    uint64_t game_pad_hashes[POORDS4_PAD_FINGERPRINT_COUNT];
    int source_library_match =
        g_source_pad_firmware == firmware &&
        g_source_pad_fingerprint_mask ==
            ((UINT32_C(1) << POORDS4_PAD_FINGERPRINT_COUNT) - 1u);
    for (unsigned index = 0;
         index < POORDS4_PAD_FINGERPRINT_COUNT; ++index) {
        game_pad_hashes[index] = poords4_fnv1a64(
            fingerprints[index], sizeof(fingerprints[index]));
        if (game_pad_hashes[index] != g_source_pad_fingerprints[index])
            source_library_match = 0;
    }
    int source_runtime_abi_match = source_library_match &&
        g_source_controller_info_runtime_abi;
    int controller_info_abi_ok = controller_info_prefix_ok ||
        source_runtime_abi_match;
    if (!common_state_target || !common_read_target ||
        !state_target_exec || !read_target_exec || !data_target_exec ||
        !controller_info_abi_ok || !controller_info_exec)
        structural_abi = 0;
    report_printf(
        report_fd,
        "firmware_gate=%s structural_abi=%d "
        "wrapper_shapes=%zu,%zu,%zu,%zu,%zu\n"
        "wrapper_targets=0x%lx,0x%lx,0x%lx,0x%lx,0x%lx\n",
        exact_manifest
            ? (firmware == POORDS4_GAME_BRIDGE_FW_0860
                ? "8.60-exact"
                : firmware == POORDS4_GAME_BRIDGE_FW_1160
                    ? "11.60-exact" : "12.40-exact")
            : "structural-runtime",
        structural_abi, jump_offsets[0], jump_offsets[1],
        jump_offsets[2], jump_offsets[3], jump_offsets[4],
        (unsigned long)wrapper_targets[0],
        (unsigned long)wrapper_targets[1],
        (unsigned long)wrapper_targets[2],
        (unsigned long)wrapper_targets[3],
        (unsigned long)wrapper_targets[4]);
    report_printf(
        report_fd,
        "structural_parts shapes=%d,%d,%d,%d,%d "
        "targets=%d,%d,%d,%d,%d exec=%d,%d,%d,%d,%d "
        "common_state=%d common_read=%d info_prefix=%d info_exec=%d "
        "info_abi=%d\n"
        "source_evidence firmware=0x%08x mask=0x%02x "
        "library_match=%d controller_runtime_abi=%d\n",
        wrapper_shapes_ok[0], wrapper_shapes_ok[1],
        wrapper_shapes_ok[2], wrapper_shapes_ok[3],
        wrapper_shapes_ok[4], wrapper_targets_ok[0],
        wrapper_targets_ok[1], wrapper_targets_ok[2],
        wrapper_targets_ok[3], wrapper_targets_ok[4],
        wrapper_targets_exec[0], wrapper_targets_exec[1],
        wrapper_targets_exec[2], wrapper_targets_exec[3],
        wrapper_targets_exec[4], common_state_target,
        common_read_target, controller_info_prefix_ok,
        controller_info_exec, controller_info_abi_ok,
        g_source_pad_firmware, g_source_pad_fingerprint_mask,
        source_library_match, g_source_controller_info_runtime_abi);
    if (!structural_abi) {
        report_printf(report_fd, "error=unsupported_firmware_abi\n");
        result = 0;
        goto done;
    }

    int32_t game_pad_handle = -1;
    int32_t game_pad_index = -1;
    if (game_bridge_select_pad_handle(
            target, base, originals[0], source->user_id,
            source->pad_index,
            report_fd, &game_pad_handle, &game_pad_index) != 0) {
        report_printf(report_fd, "state=waiting_for_game_pad_handle\n");
        result = -4;
        goto done;
    }

    memset(hooks, 0, sizeof(hooks));
    int stale_result = game_bridge_recover_stale_v1(
        target, base, originals, hooks, &hook_count, report_fd);
    if (stale_result != 0) {
        /* A successful recovery asks the supervisor to retry once after the
         * original slots are visible. An unowned unexpected import fails
         * closed and must not be overwritten. */
        result = stale_result > 0 ? -5 : -6;
        goto done;
    }


    uintptr_t local_stub_base = (uintptr_t)game_pad_read_state_stub;
    uintptr_t local_stub_end =
        (uintptr_t)game_pad_bridge_stub_end;
    const uintptr_t local_stubs[6] = {
        (uintptr_t)game_pad_read_state_stub,
        (uintptr_t)game_pad_read_state_ext_stub,
        (uintptr_t)game_pad_read_stub,
        (uintptr_t)game_pad_read_ext_stub,
        (uintptr_t)game_pad_get_data_internal_stub,
        (uintptr_t)game_pad_get_controller_info_stub
    };
    if (local_stub_end <= local_stub_base)
        goto done;
    for (unsigned index = 0; index < 6; ++index) {
        if (local_stubs[index] < local_stub_base ||
            local_stubs[index] >= local_stub_end) {
            report_printf(report_fd, "error=stub_layout index=%u\n", index);
            goto done;
        }
    }
    size_t stub_size = (size_t)(local_stub_end - local_stub_base);
    size_t gateway_offset = (stub_size + 15u) & ~(size_t)15u;
    size_t code_size = gateway_offset + 6u * 16u;
    intptr_t code_address = 0;
    intptr_t args_address = 0;
    size_t code_mapping_size =
        (code_size + POORDS4_TARGET_PAGE_SIZE - 1u) &
        ~(size_t)(POORDS4_TARGET_PAGE_SIZE - 1u);
    size_t args_mapping_size =
        (sizeof(GamePadBridgeArgs) + POORDS4_TARGET_PAGE_SIZE - 1u) &
        ~(size_t)(POORDS4_TARGET_PAGE_SIZE - 1u);
    remote_mapping_size = code_mapping_size + args_mapping_size;

    if (g_game_bridge_direct_pid != target) {
        g_game_bridge_direct_cr3 = 0;
        g_game_bridge_direct_dmap = 0;
    }
    g_game_bridge_direct_pid = target;
    g_game_bridge_direct_args = 0;
    if (!poords4_kekcall_available()) {
        report_printf(report_fd, "error=kekcall_unavailable\n");
        result = 0;
        goto done;
    }
    if (poords4_remote_map(
            target, remote_mapping_size, &remote_mapping) != 0) {
        report_printf(report_fd, "error=target_anonymous_map\n");
        result = 0;
        goto done;
    }
    code_address = remote_mapping;
    args_address = remote_mapping + (intptr_t)code_mapping_size;
    report_printf(
        report_fd,
        "target_mapping=0x%lx mapping_size=0x%zx "
        "code_mapping_size=0x%zx args_address=0x%lx\n",
        (unsigned long)remote_mapping, remote_mapping_size,
        code_mapping_size, (unsigned long)args_address);
    if (game_bridge_process_write(
            target, code_address,
            (const void *)local_stub_base, stub_size) != 0) {
        report_printf(report_fd, "error=code_write\n");
        goto done;
    }
    intptr_t remote_stubs[6];
    static const uint8_t args_in_rcx[6] = {0, 0, 1, 1, 0, 0};
    for (unsigned index = 0; index < 6; ++index) {
        remote_stubs[index] = code_address +
            (intptr_t)(local_stubs[index] - local_stub_base);
        intptr_t gateway = code_address +
            (intptr_t)gateway_offset + (intptr_t)index * 16;
        uint8_t gateway_bytes[16];
        if (game_bridge_make_gateway(
                gateway_bytes, gateway, args_address,
                remote_stubs[index], args_in_rcx[index]) != 0 ||
            game_bridge_process_write(
                target, gateway, gateway_bytes,
                sizeof(gateway_bytes)) != 0) {
            report_printf(report_fd, "error=gateway_write index=%u\n", index);
            goto done;
        }
    }

    uint64_t code_protection_args[6] = {
        (uint64_t)code_address, (uint64_t)code_mapping_size,
        PROT_READ | PROT_EXEC, 0, 0, 0};
    if (poords4_remote_syscall(
            target, SYS_mprotect, code_protection_args) != 0) {
        report_printf(report_fd, "error=code_mprotect_rx\n");
        goto done;
    }

    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    args.magic = POORDS4_GAME_BRIDGE_MAGIC;
    args.pad_size = POORDS4_GAME_BRIDGE_PAD_SIZE;
    args.pad_handle = game_pad_handle;
    args.reserved0 = (uint32_t)game_pad_index;
    args.reserved1 = POORDS4_GAME_BRIDGE_LAYOUT_V1;
    args.fp_state_internal = originals[0];
    args.fp_read_internal = originals[2];
    args.fp_data_internal = originals[4];
    args.fp_get_controller_info_trampoline = originals[5];
    args.remote_block = code_address;
    args.remote_block_size = (uint32_t)remote_mapping_size;
    args.read_state_address = originals[0];
    args.read_state_ext_address = originals[1];
    args.read_address = originals[2];
    args.read_ext_address = originals[3];
    args.data_internal_address = originals[4];
    args.controller_info_address = originals[5];
    args.controller_info_gateway = code_address +
        (intptr_t)gateway_offset + 5 * 16;
    memcpy(args.original_read_state, fingerprints[0], 16);
    memcpy(args.original_read_state_ext, fingerprints[1], 16);
    memcpy(args.original_read, fingerprints[2], 16);
    memcpy(args.original_read_ext, fingerprints[3], 16);
    memcpy(args.original_data_internal, fingerprints[4], 16);
    memcpy(args.original_controller_info, fingerprints[5],
           sizeof(args.original_controller_info));
    args.import_hook_count = hook_count;
    for (uint32_t index = 0; index < hook_count; ++index) {
        args.import_hook_slots[index] = hooks[index].slot;
        args.import_hook_originals[index] = hooks[index].original;
        args.import_hook_gateways[index] = code_address +
            (intptr_t)gateway_offset + (intptr_t)hooks[index].kind * 16;
        args.import_hook_protections[index] = hooks[index].protection;
        args.import_hook_kinds[index] = hooks[index].kind;
    }
    if (game_bridge_process_write(
            target, args_address, &args, sizeof(args)) != 0) {
        report_printf(report_fd, "error=args_write\n");
        goto done;
    }
    for (uint32_t index = 0; index < hook_count; ++index) {
        uint64_t current = 0;
        int64_t write_stages[9];
        if (game_bridge_process_read(
                target, hooks[index].slot, &current,
                sizeof(current)) != 0 ||
            current != (uint64_t)hooks[index].original) {
            report_printf(
                report_fd, "error=slot_changed index=%u current=0x%llx\n",
                index, (unsigned long long)current);
            goto rollback;
        }
        intptr_t gateway = args.import_hook_gateways[index];
        if (poords4_remote_cow_write(
                target, hooks[index].slot, &gateway,
                sizeof(gateway), (int)hooks[index].protection,
                write_stages) != 0) {
            report_printf(
                report_fd,
                "error=slot_write index=%u mprotect_rw=%lld "
                "physical_write=%lld restore=%lld\n",
                index, (long long)write_stages[4],
                (long long)write_stages[5],
                (long long)write_stages[6]);
            goto rollback;
        }
        patched_count++;
    }
    g_game_bridge_direct_args = args_address;
    g_game_bridge_direct_seq = 0;
    g_game_bridge_direct_packets = 0;
    if (out_args_kaddr)
        *out_args_kaddr = args_address;
    report_printf(
        report_fd,
        "stub_size=%zu\ncode_size=%zu\ncode_address=0x%lx\n"
        "args_address=0x%lx\npatched_game_imports=%u\n"
        "libpad_text_patched=0\nresult=1\n",
        stub_size, code_size, (unsigned long)code_address,
        (unsigned long)args_address, hook_count);
    result = 1;
    goto done;

rollback:
    while (patched_count > 0) {
        uint32_t index = --patched_count;
        int64_t restore_stages[9];
        (void)poords4_remote_cow_write(
            target, hooks[index].slot, &hooks[index].original,
            sizeof(hooks[index].original),
            (int)hooks[index].protection, restore_stages);
    }
done:
    if (result != 1)
        report_printf(report_fd, "result=%d\n", result);
    if (report_fd >= 0)
        close(report_fd);
    if (g_report_archive_fd >= 0) {
        close(g_report_archive_fd);
        g_report_archive_fd = -1;
    }
    if (result != 1) {
        if (remote_mapping > 0 && remote_mapping_size != 0)
            (void)poords4_remote_unmap(
                target, remote_mapping, remote_mapping_size);
        g_game_bridge_direct_pid = -1;
        g_game_bridge_direct_args = 0;
        g_game_bridge_direct_cr3 = 0;
        g_game_bridge_direct_dmap = 0;
    }
    return result;
#endif
}

int
wireless_ds4_game_bridge_install(
    const PoorDS4PadSource *source, pid_t *out_game_pid,
    intptr_t *out_args_kaddr)
{
    return wireless_ds4_game_bridge_run_passive(
        source, out_game_pid, out_args_kaddr);
}

int
wireless_ds4_game_bridge_find_target(pid_t *out_game_pid)
{
#if !defined(__PROSPERO__)
    (void)out_game_pid;
    return -1;
#else
    pid_t pids[8];
    if (out_game_pid)
        *out_game_pid = -1;
    size_t count = find_pids("eboot.bin", pids, 8);
    if (count == 0)
        return -2;
    if (count != 1)
        return -3;
    pid_t target = pids[0];
    if (out_game_pid)
        *out_game_pid = target;
    uint32_t libpad = 0;
    uint32_t libc = 0;
    uint32_t libkernel = 0;
    if (get_lib_quiet(target, "libScePad", &libpad) != 0 ||
        get_lib_quiet(target, "libSceLibcInternal", &libc) != 0 ||
        (get_lib_quiet(target, "libkernel_sys", &libkernel) != 0 &&
         get_lib_quiet(target, "libkernel", &libkernel) != 0))
        return -4;
    return 1;
#endif
}

int
wireless_ds4_game_bridge_abandon(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    g_game_bridge_direct_pid = -1;
    g_game_bridge_direct_args = 0;
    g_game_bridge_direct_seq = 0;
    g_game_bridge_direct_packets = 0;
    g_game_bridge_direct_cr3 = 0;
    g_game_bridge_direct_dmap = 0;
    return 0;
#endif
}

int
wireless_ds4_game_bridge_quiesce(pid_t game_pid, intptr_t args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr;
    return -1;
#else
    if (game_pid <= 0 || args_kaddr <= 0)
        return -1;
    g_game_bridge_direct_pid = game_pid;
    g_game_bridge_direct_args = args_kaddr;
    g_game_bridge_direct_cr3 = 0;
    g_game_bridge_direct_dmap = 0;

    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    if (game_bridge_process_read(
            game_pid, args_kaddr, &args, sizeof(args)) != 0 ||
        args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
        args.reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1 ||
        args.import_hook_count == 0 ||
        args.import_hook_count > POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS)
        return -1;

    const uint32_t disabled[2] = {0, 0};
    if (game_bridge_process_write(
            game_pid,
            args_kaddr + (intptr_t)offsetof(
                GamePadBridgeArgs, direct_lease),
            disabled, sizeof(disabled)) != 0)
        return -1;

    for (uint32_t index = 0; index < args.import_hook_count; ++index) {
        intptr_t slot = args.import_hook_slots[index];
        intptr_t original = args.import_hook_originals[index];
        intptr_t gateway = args.import_hook_gateways[index];
        intptr_t current = 0;
        if (slot <= 0 || original <= 0 || gateway < args.remote_block ||
            gateway >= args.remote_block + (intptr_t)args.remote_block_size ||
            game_bridge_process_read(
                game_pid, slot, &current, sizeof(current)) != 0 ||
            (current != gateway && current != original)) {
            klog_printf(
                "[PoorDS4] quiesce ownership failed pid=%d index=%u "
                "slot=0x%lx current=0x%lx\n",
                game_pid, index, (unsigned long)slot,
                (unsigned long)current);
            return -1;
        }
    }
    for (int index = (int)args.import_hook_count - 1;
         index >= 0; --index) {
        intptr_t current = 0;
        if (game_bridge_process_read(
                game_pid, args.import_hook_slots[index],
                &current, sizeof(current)) != 0)
            return -1;
        if (current == args.import_hook_originals[index])
            continue;
        /* Deliberately use the physical mapping here. Suspend teardown must
         * not borrow a target thread or depend on kstuff's syscall state. */
        if (game_bridge_process_write(
                game_pid, args.import_hook_slots[index],
                &args.import_hook_originals[index],
                sizeof(args.import_hook_originals[index])) != 0)
            return -1;
    }
    klog_printf(
        "[PoorDS4] bridge quiesced pid=%d imports=%u "
        "without ptrace, target syscalls, or unmap\n",
        game_pid, args.import_hook_count);
    g_game_bridge_direct_pid = -1;
    g_game_bridge_direct_args = 0;
    g_game_bridge_direct_seq = 0;
    g_game_bridge_direct_packets = 0;
    g_game_bridge_direct_cr3 = 0;
    g_game_bridge_direct_dmap = 0;
    return 0;
#endif
}

int
wireless_ds4_game_bridge_update(pid_t game_pid, intptr_t args_kaddr,
                               const void *pad_data,
                               uint32_t pad_data_len)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)pad_data; (void)pad_data_len;
    return -1;
#else
    if (game_pid <= 0 || !args_kaddr || !pad_data ||
        pad_data_len < POORDS4_GAME_BRIDGE_PAD_SIZE)
        return -1;
    if (game_pid != g_game_bridge_direct_pid ||
        args_kaddr != g_game_bridge_direct_args) {
        g_game_bridge_direct_pid = game_pid;
        g_game_bridge_direct_args = args_kaddr;
        g_game_bridge_direct_seq = 0;
        g_game_bridge_direct_packets = 0;
        g_game_bridge_direct_cr3 = 0;
        g_game_bridge_direct_dmap = 0;
    }
    uint32_t next_seq = g_game_bridge_direct_seq + 1u;
    if (next_seq == 0)
        next_seq = 1;
    uint32_t slot = next_seq & 1u;
    intptr_t frame_address = args_kaddr + (intptr_t)offsetof(
        GamePadBridgeArgs, direct_pad_data) +
        (intptr_t)slot * POORDS4_GAME_BRIDGE_PAD_SIZE;
    if (game_bridge_process_write(
            game_pid, frame_address, pad_data,
            POORDS4_GAME_BRIDGE_PAD_SIZE) != 0)
        return -1;
    struct {
        uint32_t seq;
        uint32_t lease;
        uint32_t active;
        uint32_t packets;
    } publication;
    publication.seq = next_seq;
    publication.lease = POORDS4_GAME_BRIDGE_DIRECT_LEASE;
    publication.active = 1;
    publication.packets = g_game_bridge_direct_packets + 1u;
    if (game_bridge_process_write(
            game_pid,
            args_kaddr + (intptr_t)offsetof(
                GamePadBridgeArgs, direct_seq), &publication,
            sizeof(publication)) != 0)
        return -1;
    g_game_bridge_direct_seq = next_seq;
    g_game_bridge_direct_packets = publication.packets;
    return 0;
#endif
}

int
wireless_ds4_game_bridge_status(pid_t game_pid, intptr_t args_kaddr,
                               PoorDS4GameBridgeStatus *out_status)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)out_status;
    return -1;
#else
    if (game_pid <= 0 || !args_kaddr || !out_status)
        return -1;
    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    int stable_snapshot = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t seq_after = 0;
        if (game_bridge_copy_args(
                game_pid, args_kaddr, &args) != 0)
            return -1;
        if (args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
            args.reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1)
            return -1;
        intptr_t seq_address = args_kaddr +
            (intptr_t)offsetof(GamePadBridgeArgs, direct_seq);
        if (remote_reader_copyout(
                game_pid, seq_address,
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        if (args.direct_seq == seq_after) {
            stable_snapshot = 1;
            break;
        }
    }
    if (!stable_snapshot ||
        args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
        args.pad_size != POORDS4_GAME_BRIDGE_PAD_SIZE ||
        args.remote_block <= 0 || args.remote_block_size == 0)
        return -1;
    ScePadData pad;
    memcpy(&pad, args.direct_pad_data[args.direct_seq & 1u],
           sizeof(pad));
    memset(out_status, 0, sizeof(*out_status));
    out_status->layout_marker = args.reserved1;
    out_status->active = args.direct_active;
    out_status->seq = args.direct_seq;
    out_status->pad_handle = args.pad_handle;
    out_status->bridge_ready = 1;
    out_status->published_packets = args.direct_packets;
    out_status->lease_expirations = args.lease_expirations;
    out_status->read_state_calls = args.read_state_calls;
    out_status->read_state_ext_calls = args.read_state_ext_calls;
    out_status->read_calls = args.read_calls;
    out_status->read_ext_calls = args.read_ext_calls;
    out_status->data_internal_calls = args.data_internal_calls;
    out_status->controller_info_calls = args.controller_info_calls;
    out_status->controller_info_spoofs = args.controller_info_spoofs;
    out_status->snapshot_contention_fallbacks =
        args.snapshot_contention_fallbacks;
    out_status->controller_info_result_overrides =
        args.controller_info_result_overrides;
    out_status->native_backing_calls = args.native_backing_calls;
    out_status->native_backing_errors = args.native_backing_errors;
    out_status->import_hook_count = args.import_hook_count;
    out_status->game_pad_index = (int32_t)args.reserved0;
    out_status->buttons = pad.buttons;
    out_status->connected = pad.connected;
    return 0;
#endif
}

int
wireless_ds4_game_bridge_remove(pid_t game_pid, intptr_t args_kaddr)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr;
    return -1;
#else
    if (!args_kaddr)
        return -1;
    errno = 0;
    if (game_pid <= 0 ||
        (kill(game_pid, 0) != 0 && errno != EPERM)) {
        klog_printf(
            "[PoorDS4] game bridge remove abandoned dead pid=%d errno=%d\n",
            game_pid, errno);
        (void)wireless_ds4_game_bridge_abandon();
        return -2;
    }
    GamePadBridgeArgs transport_args;
    memset(&transport_args, 0, sizeof(transport_args));
    int transport_read = game_bridge_copy_args(
        game_pid, args_kaddr, &transport_args);
    if (transport_read == 0 &&
        transport_args.reserved1 == POORDS4_GAME_BRIDGE_LAYOUT_V1) {
        if (transport_args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
            transport_args.remote_block <= 0 ||
            transport_args.remote_block_size == 0 ||
            transport_args.import_hook_count == 0 ||
            transport_args.import_hook_count >
                POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS) {
            klog_printf(
                "[PoorDS4] passive remove rejected invalid args "
                "pid=%d args=0x%lx\n", game_pid,
                (unsigned long)args_kaddr);
            return -1;
        }
        g_game_bridge_direct_pid = game_pid;
        g_game_bridge_direct_args = args_kaddr;
        g_game_bridge_direct_cr3 = 0;
        g_game_bridge_direct_dmap = 0;
        const uint32_t direct_off[2] = {0, 0};
        if (game_bridge_process_write(
                game_pid,
                args_kaddr + (intptr_t)offsetof(
                    GamePadBridgeArgs, direct_lease),
                direct_off, sizeof(direct_off)) != 0)
            return -1;
        for (uint32_t index = 0;
             index < transport_args.import_hook_count; ++index) {
            intptr_t slot = transport_args.import_hook_slots[index];
            intptr_t original =
                transport_args.import_hook_originals[index];
            intptr_t gateway =
                transport_args.import_hook_gateways[index];
            intptr_t current = 0;
            if (slot <= 0 || original <= 0 ||
                transport_args.import_hook_kinds[index] >= 6u ||
                gateway < transport_args.remote_block ||
                gateway >= transport_args.remote_block +
                    (intptr_t)transport_args.remote_block_size ||
                game_bridge_process_read(
                    game_pid, slot, &current, sizeof(current)) != 0 ||
                (current != gateway && current != original)) {
                klog_printf(
                    "[PoorDS4] passive remove owner validation failed "
                    "pid=%d index=%u slot=0x%lx current=0x%lx\n",
                    game_pid, index, (unsigned long)slot,
                    (unsigned long)current);
                return -1;
            }
        }
        for (int index =
                 (int)transport_args.import_hook_count - 1;
             index >= 0; --index) {
            intptr_t slot = transport_args.import_hook_slots[index];
            intptr_t original =
                transport_args.import_hook_originals[index];
            intptr_t current = 0;
            if (game_bridge_process_read(
                    game_pid, slot, &current, sizeof(current)) != 0)
                return -1;
            if (current != original) {
                int64_t restore_stages[9];
                if (poords4_remote_cow_write(
                        game_pid, slot, &original, sizeof(original),
                        (int)transport_args.import_hook_protections[index],
                        restore_stages) != 0) {
                    klog_printf(
                        "[PoorDS4] passive restore failed pid=%d "
                        "index=%d rw=%lld write=%lld reprotect=%lld\n",
                        game_pid, index,
                        (long long)restore_stages[4],
                        (long long)restore_stages[5],
                        (long long)restore_stages[6]);
                    return -1;
                }
            }
        }
        /* Import restoration prevents new entries. Give any pad call that
         * already crossed a gateway time to return before the anonymous
         * code/args mapping disappears. */
        usleep(50000);
        int unmap_result = poords4_remote_unmap(
            game_pid, transport_args.remote_block,
            transport_args.remote_block_size);
        klog_printf(
            "[PoorDS4] passive game bridge removed imports=%u "
            "without ptrace calls state=%llu controller_info=%llu "
            "unmap=%d\n",
            transport_args.import_hook_count,
            (unsigned long long)transport_args.read_state_calls,
            (unsigned long long)transport_args.controller_info_calls,
            unmap_result);
        g_game_bridge_direct_pid = -1;
        g_game_bridge_direct_args = 0;
        g_game_bridge_direct_seq = 0;
        g_game_bridge_direct_packets = 0;
        g_game_bridge_direct_cr3 = 0;
        g_game_bridge_direct_dmap = 0;
        return 0;
    }
    /* Only the no-ptrace bridge ABI v1 layout is valid. Never attach to a game to
     * clean up an unknown argument block. */
    klog_printf(
        "[PoorDS4] remove refused non-passive layout "
        "pid=%d args=0x%lx read=%d layout=0x%x\n",
        game_pid, (unsigned long)args_kaddr, transport_read,
        transport_read == 0 ? transport_args.reserved1 : 0u);
    (void)wireless_ds4_game_bridge_abandon();
    return -1;
#endif
}

static void
report_printf(int fd, const char *format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    size_t write_length = (size_t)length;
    if (write_length >= sizeof(buffer))
        write_length = sizeof(buffer) - 1;
    if (fd >= 0)
        (void)write(fd, buffer, write_length);
    if (g_report_archive_fd >= 0 && g_report_archive_fd != fd)
        (void)write(g_report_archive_fd, buffer, write_length);
}

static int
remote_pad_identity_1160(pid_t pid, intptr_t libpad_base,
                         int32_t pad_handle, int32_t *out_connected,
                         uint16_t *out_vendor, uint16_t *out_product)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)libpad_base; (void)pad_handle;
    (void)out_connected; (void)out_vendor; (void)out_product;
    return -1;
#else
    if (libpad_base <= 0 || pad_handle < 0 || !out_connected ||
        !out_vendor || !out_product)
        return -1;

    for (unsigned slot = 0; slot < 16; ++slot) {
        intptr_t entry = libpad_base + POORDS4_PAD_CLIENT_TABLE_1160 +
            (intptr_t)slot * POORDS4_PAD_CLIENT_STRIDE_1160;
        uint32_t candidate_handle = 0;
        if (remote_reader_copyout(
                pid, entry + POORDS4_PAD_CLIENT_HANDLE_1160,
                &candidate_handle, sizeof(candidate_handle)) != 0)
            return -1;
        if (candidate_handle != (uint32_t)pad_handle)
            continue;

        int32_t connected = 0;
        uint16_t vendor = 0;
        uint16_t product = 0;
        if (remote_reader_copyout(
                pid, entry + POORDS4_PAD_CLIENT_CONNECTED_1160,
                &connected, sizeof(connected)) != 0 ||
            remote_reader_copyout(
                pid, entry + POORDS4_PAD_CLIENT_VENDOR_1160,
                &vendor, sizeof(vendor)) != 0 ||
            remote_reader_copyout(
                pid, entry + POORDS4_PAD_CLIENT_PRODUCT_1160,
                &product, sizeof(product)) != 0)
            return -1;

        *out_connected = connected;
        *out_vendor = vendor;
        *out_product = product;
        return 0;
    }
    return -1;
#endif
}

static int
remote_pad_is_known_ds4(int32_t connected, uint16_t vendor,
                        uint16_t product)
{
    if (!connected || vendor != UINT16_C(0x054c))
        return 0;
    return product == UINT16_C(0x05c4) ||
           product == UINT16_C(0x09cc) ||
           product == UINT16_C(0x0ba0);
}

static int
game_cache_find_libpad_object(pid_t target, intptr_t base,
                              GameBridgeDynlibObjectPrefix *out_object)
{
#if !defined(__PROSPERO__)
    (void)target; (void)base; (void)out_object;
    return -1;
#else
    if (target <= 0 || base <= 0 || !out_object)
        return -1;
    uint64_t proc = (uint64_t)kernel_get_proc(target);
    uint64_t shared_object = 0;
    uint64_t object_address = 0;
    if (!proc || kernel_copyout(
            proc + 0x3e8, &shared_object, sizeof(shared_object)) != 0 ||
        !shared_object || kernel_copyout(
            shared_object, &object_address, sizeof(object_address)) != 0)
        return -1;
    for (unsigned index = 0; object_address && index < 512u; ++index) {
        GameBridgeDynlibObjectPrefix object;
        memset(&object, 0, sizeof(object));
        if (kernel_copyout(
                (intptr_t)object_address, &object, sizeof(object)) != 0)
            return -1;
        object_address = object.next;
        if (object.mapbase == (uint64_t)base) {
            if (!object.mapsize || object.mapsize > UINT64_C(0x02000000))
                return -1;
            *out_object = object;
            return 0;
        }
    }
    return -1;
#endif
}

static int
game_cache_follow_read_state(pid_t target, intptr_t base,
                             uint64_t mapsize, intptr_t read_state,
                             intptr_t *out_internal)
{
#if !defined(__PROSPERO__)
    (void)target; (void)base; (void)mapsize; (void)read_state;
    (void)out_internal;
    return -1;
#else
    if (!out_internal || read_state < base ||
        (uint64_t)(read_state - base) >= mapsize)
        return -1;
    uint8_t code[24];
    if (mdbg_copyout(target, read_state, code, sizeof(code)) != 0)
        return -1;
    for (unsigned offset = 0; offset + 5u <= sizeof(code); ++offset) {
        if (code[offset] != 0xe9)
            continue;
        int32_t displacement = 0;
        memcpy(&displacement, code + offset + 1u, sizeof(displacement));
        intptr_t destination = read_state + (intptr_t)offset + 5 +
                               (intptr_t)displacement;
        if (destination >= base &&
            (uint64_t)(destination - base) < mapsize) {
            *out_internal = destination;
            return 0;
        }
    }
    /* Some firmwares export the full implementation rather than a tiny
     * mode-selecting tail jump. */
    if (code[0] == 0x48 || code[0] == 0x55) {
        *out_internal = read_state;
        return 0;
    }
    return -1;
#endif
}

static int
game_cache_find_table(pid_t target, intptr_t base, uint64_t mapsize,
                      intptr_t read_state, intptr_t *out_table)
{
#if !defined(__PROSPERO__)
    (void)target; (void)base; (void)mapsize; (void)read_state;
    (void)out_table;
    return -1;
#else
    intptr_t internal = 0;
    if (!out_table || game_cache_follow_read_state(
            target, base, mapsize, read_state, &internal) != 0)
        return -1;
    uint8_t code[0x300];
    if (mdbg_copyout(target, internal, code, sizeof(code)) != 0)
        return -1;
    for (size_t offset = 0; offset + 7u <= sizeof(code); ++offset) {
        if (code[offset] != 0x48 || code[offset + 1u] != 0x8d ||
            code[offset + 2u] != 0x0d)
            continue;
        int32_t displacement = 0;
        memcpy(&displacement, code + offset + 3u, sizeof(displacement));
        intptr_t candidate = internal + (intptr_t)offset + 7 +
                             (intptr_t)displacement;
        if (candidate < base || (uint64_t)(candidate - base) >= mapsize ||
            mapsize - (uint64_t)(candidate - base) < UINT64_C(0x8ac0))
            continue;
        /* The read implementation walks 24 entries with a 0x5c8 stride.
         * Require both loop constants near the RIP-relative table reference;
         * this discovers the table from code shape instead of a firmware
         * offset. */
        int has_stride = 0;
        int has_limit = 0;
        size_t end = offset + 160u;
        if (end > sizeof(code)) end = sizeof(code);
        for (size_t scan = offset; scan + 6u <= end; ++scan) {
            static const uint8_t stride_pattern[6] =
                {0x48, 0x05, 0xc8, 0x05, 0x00, 0x00};
            static const uint8_t limit_pattern[6] =
                {0x48, 0x3d, 0xc0, 0x8a, 0x00, 0x00};
            if (memcmp(code + scan, stride_pattern,
                       sizeof(stride_pattern)) == 0)
                has_stride = 1;
            if (memcmp(code + scan, limit_pattern,
                       sizeof(limit_pattern)) == 0)
                has_limit = 1;
        }
        if (!has_stride || !has_limit)
            continue;
        *out_table = candidate;
        return 0;
    }
    return -1;
#endif
}
