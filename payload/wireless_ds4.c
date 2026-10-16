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
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <elf.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>

#include "wireless_ds4.h"

#include "pad_types.h"
#include "game_process_watch.h"

#ifndef GC_FORCE_STRUCTURAL_FIRMWARE
#define GC_FORCE_STRUCTURAL_FIRMWARE 0
#endif

#ifndef POORDS4_DATA_DIR
#define POORDS4_DATA_DIR "/data/poords4"
#endif

extern void poords4_log(const char *format, ...);
#define klog_printf poords4_log
#define POORDS4_REMOTE_PAD_CAPACITY 256u
#define POORDS4_GAME_BRIDGE_FW_0602 UINT32_C(0x06020004)
#define POORDS4_GAME_BRIDGE_FW_0860 UINT32_C(0x08600004)
#define POORDS4_GAME_BRIDGE_FW_1001 UINT32_C(0x10010000)
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
    if (wireless_ds4_game_bridge_image_state(pid) != 0)
        return -1;

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
    if (stages[4] == 0 && wireless_ds4_game_bridge_image_state(pid) == 0) {
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

/* libScePad's private client-table layout verified across firmwares 6.02 - 12.40+. */
#define POORDS4_PAD_CLIENT_TABLE_1160 UINT32_C(0x00020018)
#define POORDS4_PAD_CLIENT_STRIDE_0602 UINT32_C(0x00000548)
#define POORDS4_PAD_CLIENT_SPAN_0602   UINT32_C(0x00007ec0)
#define POORDS4_PAD_CLIENT_STRIDE_1160 UINT32_C(0x000005c8)
#define POORDS4_PAD_CLIENT_SPAN_1160   UINT32_C(0x00008ac0)
#define POORDS4_PAD_CLIENT_CONNECTED_1160 UINT32_C(0x18)
#define POORDS4_PAD_CLIENT_HANDLE_1160 UINT32_C(0x1c)
#define POORDS4_PAD_CLIENT_USER_ID_1160 UINT32_C(0x24)
#define POORDS4_PAD_CLIENT_VENDOR_1160 UINT32_C(0x60)
#define POORDS4_PAD_CLIENT_PRODUCT_1160 UINT32_C(0x62)

static void report_printf(int fd, const char *format, ...);
static int remote_pad_identity(
    pid_t pid, intptr_t libpad_base, int32_t pad_handle,
    int32_t *out_connected, uint16_t *out_vendor, uint16_t *out_product);
static int __attribute__((unused)) remote_pad_identity_1160(
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
    volatile uint16_t reset_combo_ticks;
    volatile uint16_t reset_requested;
    /* Appended source ABI: older firmware can expose a live pad through the
     * queued read path while a secondary thread's state cache remains idle. */
    intptr_t fp_read;
    volatile int32_t last_read_result;
    volatile uint32_t read_success_frames;
    volatile uint32_t read_empty_frames;
    volatile uint32_t read_error_frames;
    volatile uint32_t state_fallback_frames;
    volatile uint32_t reader_mode; /* 1=state, 2=queued read */
    /* Multi-controller reader slots (up to 4 DS4 controllers concurrently) */
    int32_t slot_handles[POORDS4_MAX_SLOTS];
    volatile uint32_t slot_seqs[POORDS4_MAX_SLOTS];
    volatile int32_t slot_results[POORDS4_MAX_SLOTS];
    uint8_t slot_data[POORDS4_MAX_SLOTS][POORDS4_REMOTE_PAD_CAPACITY];
} RemotePadReaderArgs;

_Static_assert(sizeof(ScePadData) == 120,
               "ScePadData ABI changed");
_Static_assert(sizeof(RemotePadReaderArgs) == 1440,
               "RemotePadReaderArgs ABI changed");

#include "reader_rumble_channel.h"

static int g_reader_rumble_fd=-1;
static pid_t g_reader_rumble_pid=-1;
static intptr_t g_reader_rumble_args;
static uint64_t g_reader_rumble_sent[POORDS4_MAX_SLOTS];

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
    RemotePadReaderTransport *transport=(RemotePadReaderTransport*)arg;
    RemotePadReaderArgs *a = &transport->input;
    typedef int32_t (*read_fn_t)(int32_t, void *);
    typedef int32_t (*read_events_fn_t)(int32_t, void *, int32_t);
    typedef void (*usleep_fn_t)(unsigned int);
    typedef int32_t (*kill_fn_t)(int32_t, int32_t);
    typedef int32_t (*close_pad_fn_t)(int32_t);
    read_fn_t readstate = (read_fn_t)(uintptr_t)a->fp_readstate;
    read_events_fn_t read_events =
        (read_events_fn_t)(uintptr_t)a->fp_read;
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
        int32_t result = -1;
        int32_t queued_result = -1;
        if (read_events) {
            queued_result = read_events(
                a->pad_handle, a->pad_data, 1);
            __atomic_store_n(
                &a->last_read_result, queued_result,
                __ATOMIC_RELAXED);
            if (queued_result > 0) {
                result = 0;
                __atomic_store_n(
                    &a->reader_mode, 2, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &a->read_success_frames, 1,
                    __ATOMIC_RELAXED);
            } else {
                if (queued_result == 0)
                    (void)__atomic_fetch_add(
                        &a->read_empty_frames, 1,
                        __ATOMIC_RELAXED);
                else
                    (void)__atomic_fetch_add(
                        &a->read_error_frames, 1,
                        __ATOMIC_RELAXED);
            }
        }
        if (queued_result <= 0) {
            result = readstate(a->pad_handle, a->pad_data);
            __atomic_store_n(
                &a->reader_mode, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(
                &a->state_fallback_frames, 1,
                __ATOMIC_RELAXED);
        }
        if (result == 0) {
            const ScePadData *pad =
                (const ScePadData *)(const void *)a->pad_data;
            int l1_held = (pad->buttons & 0x00000400u) != 0;
            int r1_held = (pad->buttons & 0x00000800u) != 0;
            int l2_held = ((pad->buttons & 0x00000100u) != 0) ||
                (pad->analogButtons.l2 >= 32u);
            int r2_held = ((pad->buttons & 0x00000200u) != 0) ||
                (pad->analogButtons.r2 >= 32u);
            if (l1_held && r1_held && l2_held && r2_held) {
                uint16_t ticks = a->reset_combo_ticks;
                if (ticks < 65535u)
                    ticks++;
                a->reset_combo_ticks = ticks;
                if (ticks >= 45u)
                    __atomic_store_n(
                        &a->reset_requested, 1u, __ATOMIC_RELEASE);
            } else {
                if (a->reset_combo_ticks >= 1u)
                    a->reset_combo_ticks -= 1u;
                else
                    a->reset_combo_ticks = 0;
            }
        }
        __atomic_store_n(&a->last_result, result, __ATOMIC_RELAXED);
        __atomic_store_n(&a->seq, odd + 1u, __ATOMIC_RELEASE);

        /* Mirror slot 0 data and poll secondary physical controllers (slots 1..3) */
        a->slot_handles[0] = a->pad_handle;
        __atomic_store_n(&a->slot_results[0], result, __ATOMIC_RELAXED);
        __atomic_store_n(&a->slot_seqs[0], odd + 1u, __ATOMIC_RELEASE);

        for (unsigned s = 1; s < POORDS4_MAX_SLOTS; ++s) {
            int32_t sh = a->slot_handles[s];
            if (sh > 0) {
                uint32_t s_odd =
                    (__atomic_load_n(&a->slot_seqs[s], __ATOMIC_RELAXED) + 1u) | 1u;
                __atomic_store_n(&a->slot_seqs[s], s_odd, __ATOMIC_RELEASE);
                int32_t s_res = -1;
                if (read_events) {
                    int32_t qr = read_events(sh, a->slot_data[s], 1);
                    if (qr > 0)
                        s_res = 0;
                }
                if (s_res != 0)
                    s_res = readstate(sh, a->slot_data[s]);
                __atomic_store_n(&a->slot_results[s], s_res, __ATOMIC_RELAXED);
                __atomic_store_n(&a->slot_seqs[s], s_odd + 1u, __ATOMIC_RELEASE);
            }
        }

        reader_rumble_service(transport,0);
        poords4_source_discovery_poll(&transport->discovery, a);
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
    reader_rumble_service(transport,1);
    poords4_source_discovery_release(&transport->discovery, a);
    if (close_pad) {
        if (a->close_pad_on_exit && a->pad_handle >= 0)
            (void)close_pad(a->pad_handle);
        for (unsigned s = 1; s < POORDS4_MAX_SLOTS; ++s) {
            if ((transport->owned_secondary_pad_slots & (1u << s)) != 0 &&
                a->slot_handles[s] > 0 && a->slot_handles[s] != a->pad_handle) {
                int already_closed = 0;
                for (unsigned previous = 1; previous < s; ++previous)
                    already_closed |=
                        (transport->owned_secondary_pad_slots & (1u << previous)) != 0 &&
                        a->slot_handles[previous] == a->slot_handles[s];
                if (already_closed)
                    continue;
                (void)close_pad(a->slot_handles[s]);
            }
        }
    }
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
#define POORDS4_GAME_BRIDGE_API_COUNT 12u
#define POORDS4_GAME_BRIDGE_MAGIC UINT32_C(0x34424744) /* "DGB4" */
#define POORDS4_GAME_BRIDGE_PAD_SIZE 120u
#define POORDS4_GAME_BRIDGE_LAYOUT_V1 UINT32_C(0x52433433) /* legacy ABI v1 */
#define POORDS4_GAME_BRIDGE_SNAPSHOT_RETRIES 64u
#define POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS 64u
#define POORDS4_GAME_BRIDGE_DIRECT_LEASE 2048u

typedef struct {
    int32_t  pad_handle;
    int32_t  pad_index;
    volatile uint32_t active;
    uint32_t is_dualsense;
    uint32_t is_simulated;
    volatile uint32_t direct_seq;
    volatile uint32_t direct_lease;
    volatile uint32_t direct_packets;
    uint8_t  direct_pad_data[2][POORDS4_GAME_BRIDGE_PAD_SIZE];
    volatile uint64_t read_state_calls;
    volatile uint64_t read_state_ext_calls;
    volatile uint64_t read_calls;
    volatile uint64_t read_ext_calls;
    volatile uint64_t data_internal_calls;
    volatile uint64_t direct_fallback_frames;
    volatile uint64_t native_passthrough_frames;
    volatile uint32_t last_read_seq;
    uint32_t reserved[3];
} GamePadBridgeSlot;

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
    intptr_t fp_get_ext_controller_info_trampoline;
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
    /* Legacy-compatible bridge ABI v1 offsets permit recovery of RC33.
     * The receiver is inactive. R29's marker in legacy_receiver_port identifies
     * packets/timeouts as duplicate-buffer restores/native-disconnected counts. */
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
    volatile uint8_t reset_ticks[4];
    /* RC31 diagnostics for one-frame continuity protection. */
    volatile uint64_t snapshot_contention_fallbacks;
    volatile uint64_t controller_info_result_overrides;
    volatile uint64_t native_backing_calls;
    volatile uint64_t native_backing_errors;
    /* RC33 redirects caller-owned PLT/GOT slots instead of modifying
     * libScePad text.  The saved entries make removal transactional. */
    uint32_t import_hook_count;
    volatile uint32_t reset_combo_ticks;
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
    /* RC40 path diagnostics. A connected native game frame is always
     * authoritative. RemotePlay is the fallback only when Sony's game-local
     * path reports the backing slot disconnected or returns an error. */
    volatile uint64_t native_passthrough_frames;
    volatile uint64_t native_connected_frames;
    volatile uint64_t direct_fallback_frames;
    volatile uint64_t direct_active_fallbacks;
    volatile int32_t last_native_result;
    volatile uint32_t reset_requested;
    volatile uint64_t native_success_frames;
    volatile uint64_t native_input_activity_frames;
    volatile uint64_t last_native_timestamp;
    volatile uint32_t last_native_buttons;
    volatile uint8_t last_native_lx;
    volatile uint8_t last_native_ly;
    volatile uint8_t last_native_rx;
    volatile uint8_t last_native_ry;
    volatile uint8_t last_native_l2;
    volatile uint8_t last_native_r2;
    volatile uint8_t last_native_connected;
    volatile uint8_t last_native_count;
    /* Diagnostic Telemetry & Observability Suite */
    volatile int32_t last_caller_handle;
    volatile int32_t last_mismatched_handle;
    volatile uint64_t handle_match_calls;
    volatile uint64_t handle_mismatch_calls;
    volatile uint64_t read_zero_returns;
    volatile uint64_t read_nonzero_returns;
    volatile uint32_t current_read_streak;
    volatile uint32_t max_read_streak;
    volatile int32_t observed_handles[4];
    volatile uint64_t observed_handle_calls[4];
    volatile uint32_t event_ring_head;
    PoorDS4InputEvent event_ring[POORDS4_GAME_BRIDGE_EVENT_RING_SIZE];
    GamePadBridgeSlot slots[POORDS4_MAX_SLOTS];
} GamePadBridgeArgs;

_Static_assert(offsetof(GamePadBridgeArgs, legacy_fp_setsockopt) == 496,
               "PoorDS4 bridge ABI v1 prefix changed");
_Static_assert(sizeof(GamePadBridgeArgs) == 5968,
               "GamePadBridgeArgs ABI changed");

#define POORDS4_TRIGGER_ARGS_MAGIC UINT32_C(0x31525444)
#define POORDS4_EFFECT_ARGS_MAGIC UINT32_C(0x31465854)
#define POORDS4_EFFECT_DIAG_MAGIC UINT32_C(0x31445854)
#define POORDS4_DEVICE_CLASS_DIAG_MAGIC UINT32_C(0x31444350)
#define POORDS4_EMPTY_READ_DIAG_MAGIC UINT32_C(0x31524550)
#define POORDS4_CLIENT_BINDING_MAGIC UINT32_C(0x31424350)
#include "trigger_effect_state.h"
typedef struct {
    GamePadBridgeArgs input;
    uint32_t magic, reserved;
    intptr_t fp_get_trigger_effect_state;
    volatile uint64_t calls, neutral_overrides;
    volatile int32_t last_native_result;
    uint32_t reserved2;
    PoorDS4GameOutput rumble;
    intptr_t fp_set_trigger_effect;
    uint32_t effect_magic, effect_reserved;
    volatile uint64_t effect_set_calls, effect_set_accepts, effect_state_overrides;
    volatile int32_t effect_native_result;
    uint32_t effect_reserved2;
    PoorDS4TriggerCache effects[POORDS4_MAX_SLOTS];
    /* Appended after the R25 cache: diagnostic fields are individually atomic,
     * not a coherent multi-field request snapshot. No additional input reads. */
    uint32_t effect_diag_magic;
    volatile uint32_t effect_last_skip;
    volatile uint64_t effect_skip_counts[9];
    volatile int32_t effect_last_slot, effect_last_call_handle;
    volatile uint32_t effect_last_mask, effect_last_left_mode, effect_last_right_mode;
    volatile uint64_t effect_compat_successes, effect_cached_unavailable;
    /* Optional information-query observation. The original input/output
     * prefix and all Sony results/buffers remain unchanged. */
    intptr_t fp_device_class_info;
    uint32_t device_class_diag_magic, device_class_reserved;
    volatile uint64_t device_class_calls, device_class_errors, device_class_matched_calls;
    volatile int32_t device_class_last_result, device_class_last_handle;
    volatile int32_t device_class_last_slot, device_class_last_value;
    /* Game-local lookup: fixed physical feed owners, live Sony client handles.
     * The original 5968-byte input prefix and prior extensions stay intact. */
    uint32_t client_binding_magic, client_stride;
    intptr_t client_table;
    int32_t client_users[POORDS4_MAX_SLOTS];
    uint32_t client_primary_slot, client_reserved;
    volatile uint64_t client_bindings, client_native_rejects, client_missing;
    /* Observe the packet returned to the consumer, rather than treating an
     * advancing publication counter as proof that the game received it. */
    volatile uint64_t consumer_samples, consumer_disconnected;
    volatile intptr_t consumer_output;
    volatile int32_t consumer_handle, consumer_result;
    volatile uint32_t consumer_connected, consumer_count;
    /* One queued-read cursor per Sony client and API. Aliases of a physical
     * user share its publication, but must not drain another module's queue.
     * The upper word keys each cursor by the complete handle generation. */
    volatile uint64_t client_read_cursors[2][24];
} GamePadBridgeTriggerArgs;
_Static_assert(offsetof(GamePadBridgeTriggerArgs,magic)==5968,
               "Trigger extension changed the original input prefix");

static pid_t g_game_bridge_direct_pid = -1;
static PoorDS4GameProcessWatch g_game_process_watch = {.fd = -1, .pid = -1};
/* Supervisor-local routing; both injected input prefixes stay unchanged. */
static int32_t g_reader_users[POORDS4_MAX_SLOTS] = {-1, -1, -1, -1};
static int g_game_reader_slots[POORDS4_MAX_SLOTS] = {-1, -1, -1, -1};
static intptr_t g_game_bridge_direct_args = 0;
static uint32_t g_game_bridge_direct_seq = 0;
static uint32_t g_game_bridge_direct_packets = 0;
static uint32_t g_game_bridge_slot_seq[POORDS4_MAX_SLOTS] = {0};
static uint32_t g_game_bridge_slot_packets[POORDS4_MAX_SLOTS] = {0};
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
extern int32_t game_pad_get_ext_controller_info_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_get_trigger_effect_state_stub(
    int32_t handle, void *out, GamePadBridgeArgs *args);
extern int32_t game_pad_set_vibration_stub(
    int32_t handle,const void *input,GamePadBridgeArgs *args);
extern int32_t game_pad_set_vibration_mode_stub(
    int32_t handle,int32_t mode,GamePadBridgeArgs *args);
extern void game_pad_bridge_stub_end(void);

static __attribute__((always_inline)) inline int
game_bridge_find_slot(GamePadBridgeArgs *args, int32_t handle)
{
    if (!args || handle <= 0 || (handle & 0xffffff00) == 0)
        return -1;
    if (args->slots[0].reserved[2] == POORDS4_CLIENT_BINDING_MAGIC) {
        GamePadBridgeTriggerArgs *extension = (GamePadBridgeTriggerArgs *)args;
        uint32_t stride = extension->client_stride;
        if (extension->client_binding_magic != POORDS4_CLIENT_BINDING_MAGIC ||
            extension->client_table <= 0 || (stride != 0x5c8u && stride != 0x548u))
            return -1;
        for (unsigned row = 0; row < 24u; ++row) {
            const volatile uint8_t *entry = (const volatile uint8_t *)(uintptr_t)
                (extension->client_table + (intptr_t)row * stride);
            int32_t found = *(const volatile int32_t *)(entry + 0x1c);
            if (found != handle) continue;
            int32_t user = *(const volatile int32_t *)(entry + 0x24);
            int32_t connected = *(const volatile int32_t *)(entry + 0x18);
            uint32_t device = *(const volatile uint32_t *)(entry + 0x60);
            if (*(const volatile int32_t *)(entry + 0x1c) != found ||
                *(const volatile int32_t *)(entry + 0x24) != user)
                return -1;
            /* A DualSense can reuse an old DS4 index. Its native identity
             * takes precedence over every cached handle-generation alias. */
            int known_ds4 = device == UINT32_C(0x05c4054c) || device == UINT32_C(0x09cc054c) ||
                device == UINT32_C(0x0ba0054c) || device == UINT32_C(0x0000054c);
            if (device == UINT32_C(0x0ce6054c) || device == UINT32_C(0x0df2054c) ||
                connected < 0 || connected > 1 || (connected && !known_ds4)) {
                (void)__atomic_fetch_add(&extension->client_native_rejects, 1, __ATOMIC_RELAXED);
                return -1;
            }
            int owner = -1;
            for (unsigned slot = 0; slot < POORDS4_MAX_SLOTS; ++slot) {
                if (user < 0 || extension->client_users[slot] != user) continue;
                if (owner >= 0) return -1;
                owner = (int)slot;
            }
            if (owner < 0) return -1;
            /* Multiple modules may open clients for one user. Resolve every
             * validated alias to that user's existing physical endpoint;
             * never allocate another feed or borrow an unrelated user's data. */
            if (__atomic_load_n(&args->slots[owner].pad_handle, __ATOMIC_RELAXED) != handle) {
                __atomic_store_n(&args->slots[owner].pad_index, handle & 0xff, __ATOMIC_RELAXED);
                __atomic_store_n(&args->slots[owner].pad_handle, handle, __ATOMIC_RELEASE);
                (void)__atomic_fetch_add(&extension->client_bindings, 1, __ATOMIC_RELAXED);
            }
            if ((unsigned)owner == extension->client_primary_slot) {
                __atomic_store_n(&args->reserved0, (uint32_t)(handle & 0xff), __ATOMIC_RELAXED);
                __atomic_store_n(&args->pad_handle, handle, __ATOMIC_RELEASE);
            }
            return owner;
        }
        (void)__atomic_fetch_add(&extension->client_missing, 1, __ATOMIC_RELAXED);
        return -1;
    }
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        if (args->slots[s].pad_handle > 0 && args->slots[s].pad_handle == handle)
            return (int)s;
    }
    if (args->pad_handle > 0 && handle == args->pad_handle)
        return 0;
    int32_t handle_idx = handle & 0xff;
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        if (args->slots[s].pad_index >= 0 && args->slots[s].pad_index == handle_idx &&
            (args->slots[s].active || args->slots[s].is_simulated || args->slots[s].is_dualsense))
            return (int)s;
    }
    int32_t legacy_idx = (int32_t)args->reserved0;
    if (legacy_idx >= 0 && legacy_idx < 24 && handle_idx == legacy_idx &&
        (args->slots[0].active || args->slots[0].is_simulated || args->slots[0].is_dualsense))
        return 0;
    if (handle_idx >= 0 && handle_idx < (int32_t)POORDS4_MAX_SLOTS) {
        /* A new handle alone does not prove physical source ownership. */
        if (args->slots[handle_idx].is_simulated) {
            if (args->slots[handle_idx].pad_handle <= 0)
                args->slots[handle_idx].pad_handle = handle;
            if (args->slots[handle_idx].pad_index < 0)
                args->slots[handle_idx].pad_index = handle_idx;
            return handle_idx;
        }
    }
    return -1;
}

static __attribute__((always_inline)) inline int
game_bridge_direct_available_slot(GamePadBridgeArgs *args, unsigned slot)
{
    if (!args || slot >= POORDS4_MAX_SLOTS ||
        args->reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1)
        return 0;
    /* A matching user does not give this handle another slot's physical feed.
     * Handle-generation aliases already resolve to their own slot. */
    if (slot == 0) {
        if (__atomic_load_n(&args->slots[0].active, __ATOMIC_ACQUIRE)) {
            uint32_t lease = __atomic_load_n(&args->slots[0].direct_lease, __ATOMIC_RELAXED);
            while (lease != 0) {
                if (__atomic_compare_exchange_n(&args->slots[0].direct_lease, &lease, lease - 1u, 0,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    return 1;
            }
        }
        if (!__atomic_load_n(&args->direct_active, __ATOMIC_ACQUIRE))
            return 0;
        uint32_t lease = __atomic_load_n(&args->direct_lease, __ATOMIC_RELAXED);
        while (lease != 0) {
            if (__atomic_compare_exchange_n(&args->direct_lease, &lease, lease - 1u, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                return 1;
        }
        if (__atomic_exchange_n(&args->direct_active, 0, __ATOMIC_ACQ_REL) != 0)
            (void)__atomic_fetch_add(&args->lease_expirations, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (!__atomic_load_n(&args->slots[slot].active, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t lease = __atomic_load_n(&args->slots[slot].direct_lease, __ATOMIC_RELAXED);
    while (lease != 0) {
        if (__atomic_compare_exchange_n(&args->slots[slot].direct_lease, &lease, lease - 1u, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    if (__atomic_exchange_n(&args->slots[slot].active, 0, __ATOMIC_ACQ_REL) != 0)
        (void)__atomic_fetch_add(&args->lease_expirations, 1, __ATOMIC_RELAXED);
    return 0;
}

static __attribute__((always_inline)) inline volatile uint64_t *
game_bridge_client_read_cursor(GamePadBridgeArgs *args, int32_t handle, unsigned api)
{
    if (args->slots[0].reserved[2] != POORDS4_CLIENT_BINDING_MAGIC ||
        (handle & 0xff) >= 24 || api >= 2)
        return NULL;
    GamePadBridgeTriggerArgs *extension = (GamePadBridgeTriggerArgs *)args;
    if (extension->client_binding_magic != POORDS4_CLIENT_BINDING_MAGIC)
        return NULL;
    return &extension->client_read_cursors[api][handle & 0xff];
}

static __attribute__((always_inline, unused)) inline int
game_bridge_direct_available(GamePadBridgeArgs *args)
{
    return game_bridge_direct_available_slot(args, 0);
}

static __attribute__((always_inline, unused)) inline int
game_bridge_handle_matches(GamePadBridgeArgs *args, int32_t handle)
{
    if (!args || handle <= 0 || (handle & 0xffffff00) == 0)
        return 0;
    if (handle == args->pad_handle)
        return 1;
    /* libScePad regenerates the upper handle bits when a controller/client
     * is reopened, while the low byte remains the global player slot.  The
     * installer derives and validates reserved0 from the selected client
     * table entry.  Following that slot keeps P2 alive across a handle
     * generation change without ever matching the connected P1 entry. */
    int32_t index = (int32_t)args->reserved0;
    if (index >= 0 && index < 24 && (handle & 0xff) == index)
        return 1;
    return game_bridge_find_slot(args, handle) >= 0;
}

static __attribute__((always_inline)) inline int
game_bridge_copy_direct_slot(GamePadBridgeArgs *args, unsigned slot, void *out,
                             volatile uint64_t *call_counter,
                             volatile uint64_t *slot_counter)
{
    if (!args || slot >= POORDS4_MAX_SLOTS)
        return 0;
    volatile uint8_t *destination = (volatile uint8_t *)out;

    unsigned source_slot = slot;

    volatile uint32_t *p_seq = (source_slot == 0 && args->slots[0].direct_packets == 0)
        ? &args->direct_seq : &args->slots[source_slot].direct_seq;
    volatile uint32_t *p_active = (source_slot == 0 && args->slots[0].direct_packets == 0)
        ? &args->direct_active : &args->slots[source_slot].active;

    for (unsigned attempt = 0; attempt < POORDS4_GAME_BRIDGE_SNAPSHOT_RETRIES; ++attempt) {
        uint32_t before = __atomic_load_n(p_seq, __ATOMIC_ACQUIRE);
        const uint8_t *src = (source_slot == 0 && args->slots[0].direct_packets == 0)
            ? args->direct_pad_data[before & 1u]
            : args->slots[source_slot].direct_pad_data[before & 1u];
        for (unsigned byte = 0; byte < POORDS4_GAME_BRIDGE_PAD_SIZE; ++byte)
            destination[byte] = src[byte];
        if (before == __atomic_load_n(p_seq, __ATOMIC_ACQUIRE)) {
            if (call_counter)
                (void)__atomic_fetch_add(call_counter, 1, __ATOMIC_RELAXED);
            if (slot_counter)
                (void)__atomic_fetch_add(slot_counter, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(&args->slots[slot].direct_fallback_frames, 1, __ATOMIC_RELAXED);
            return 1;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    if (!__atomic_load_n(p_active, __ATOMIC_ACQUIRE))
        return 0;
    uint32_t current = __atomic_load_n(p_seq, __ATOMIC_ACQUIRE);
    const uint8_t *src = (source_slot == 0 && args->slots[0].direct_packets == 0)
        ? args->direct_pad_data[current & 1u]
        : args->slots[source_slot].direct_pad_data[current & 1u];
    for (unsigned byte = 0; byte < POORDS4_GAME_BRIDGE_PAD_SIZE; ++byte)
        destination[byte] = src[byte];
    destination[offsetof(ScePadData, connected)] = 1;
    (void)__atomic_fetch_add(&args->snapshot_contention_fallbacks, 1, __ATOMIC_RELAXED);
    if (call_counter)
        (void)__atomic_fetch_add(call_counter, 1, __ATOMIC_RELAXED);
    if (slot_counter)
        (void)__atomic_fetch_add(slot_counter, 1, __ATOMIC_RELAXED);
    (void)__atomic_fetch_add(&args->slots[slot].direct_fallback_frames, 1, __ATOMIC_RELAXED);
    return 1;
}

static __attribute__((always_inline, unused)) inline int
game_bridge_copy_direct(GamePadBridgeArgs *args, void *out,
                        volatile uint64_t *call_counter)
{
    return game_bridge_copy_direct_slot(args, 0, out, call_counter, NULL);
}

static __attribute__((always_inline)) inline int
game_bridge_direct_controls_active_slot(GamePadBridgeArgs *args, unsigned slot)
{
    if (!args || slot >= POORDS4_MAX_SLOTS)
        return 0;
    unsigned source_slot = slot;
    volatile uint32_t *p_seq = (source_slot == 0 && args->slots[0].direct_packets == 0)
        ? &args->direct_seq : &args->slots[source_slot].direct_seq;
    uint32_t sequence = __atomic_load_n(p_seq, __ATOMIC_ACQUIRE);
    const volatile uint8_t *pad = (source_slot == 0 && args->slots[0].direct_packets == 0)
        ? args->direct_pad_data[sequence & 1u]
        : args->slots[source_slot].direct_pad_data[sequence & 1u];
    uint32_t buttons = (uint32_t)pad[0] |
        ((uint32_t)pad[1] << 8u) |
        ((uint32_t)pad[2] << 16u) |
        ((uint32_t)pad[3] << 24u);
    if (buttons != 0 || pad[8] != 0 || pad[9] != 0)
        return 1;
    return 0;
}

static __attribute__((always_inline, unused)) inline int
game_bridge_direct_controls_active(GamePadBridgeArgs *args)
{
    return game_bridge_direct_controls_active_slot(args, 0);
}

static __attribute__((always_inline)) inline int
game_bridge_native_connected(void *out)
{
    return ((volatile uint8_t *)out)[offsetof(ScePadData, connected)] != 0;
}


static __attribute__((always_inline)) inline void
game_bridge_record_native(GamePadBridgeArgs *args, void *out)
{
    volatile ScePadData *pad = (volatile ScePadData *)out;
    (void)__atomic_fetch_add(
        &args->native_success_frames, 1, __ATOMIC_RELAXED);
    int active = pad->buttons != 0 ||
        pad->analogButtons.l2 != 0 || pad->analogButtons.r2 != 0 ||
        pad->leftStick.x < 96 || pad->leftStick.x > 160 ||
        pad->leftStick.y < 96 || pad->leftStick.y > 160 ||
        pad->rightStick.x < 96 || pad->rightStick.x > 160 ||
        pad->rightStick.y < 96 || pad->rightStick.y > 160;
    if (active)
        (void)__atomic_fetch_add(
            &args->native_input_activity_frames, 1, __ATOMIC_RELAXED);
    __atomic_store_n(
        &args->last_native_timestamp, pad->timestamp, __ATOMIC_RELAXED);
    __atomic_store_n(
        &args->last_native_buttons, pad->buttons, __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_lx, pad->leftStick.x,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_ly, pad->leftStick.y,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_rx, pad->rightStick.x,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_ry, pad->rightStick.y,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_l2, pad->analogButtons.l2,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_r2, pad->analogButtons.r2,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_connected, pad->connected,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&args->last_native_count, pad->count,
                     __ATOMIC_RELAXED);
}

static __attribute__((always_inline)) inline int
game_bridge_is_reset_combo_held(uint32_t buttons, uint8_t l2, uint8_t r2)
{
    int l1_held = (buttons & 0x00000400u) != 0;
    int r1_held = (buttons & 0x00000800u) != 0;
    int l2_held = ((buttons & 0x00000100u) != 0) || (l2 >= 32u);
    int r2_held = ((buttons & 0x00000200u) != 0) || (r2 >= 32u);
    return l1_held && r1_held && l2_held && r2_held;
}

static __attribute__((always_inline)) inline void
game_bridge_check_reset_shortcut(GamePadBridgeArgs *args, int32_t handle, void *out)
{
    if (!args || args->magic != POORDS4_GAME_BRIDGE_MAGIC || !out)
        return;
    const ScePadData *pad = (const ScePadData *)out;
    int slot = game_bridge_find_slot(args, handle);
    /* The client index is not the logical bridge slot. Modulo routing can
     * conflate a high-index DS4 with a native controller and allows unknown
     * handles to request a reset. Disconnected records cannot hold a combo. */
    if (slot < 0 || !pad->connected)
        return;
    if (game_bridge_is_reset_combo_held(pad->buttons, pad->analogButtons.l2, pad->analogButtons.r2)) {
        uint8_t ticks = args->reset_ticks[slot];
        if (ticks < 255u)
            ticks++;
        args->reset_ticks[slot] = ticks;
        if ((uint32_t)ticks > args->reset_combo_ticks)
            __atomic_store_n(&args->reset_combo_ticks, (uint32_t)ticks, __ATOMIC_RELAXED);
        if (ticks >= 45u) {
            __atomic_store_n(&args->reset_requested, 1u, __ATOMIC_RELEASE);
        }
    } else {
        uint8_t ticks = args->reset_ticks[slot];
        args->reset_ticks[slot] = (ticks >= 1u) ? (ticks - 1u) : 0u;
    }
}

static __attribute__((always_inline)) inline void
game_bridge_record_call(GamePadBridgeArgs *args, int32_t handle, int matched)
{
    if (!args)
        return;
    __atomic_store_n(&args->last_caller_handle, handle, __ATOMIC_RELAXED);
    if (matched) {
        (void)__atomic_fetch_add(&args->handle_match_calls, 1, __ATOMIC_RELAXED);
    } else {
        /* Filter out uninitialized/dummy handles (0, negative SCE error codes like
         * 0x80920004, or non-handle values) queried by game engines for unassigned slots. */
        if (handle > 0 && (handle & 0xffffff00) != 0 && (handle & 0x80000000) == 0) {
            __atomic_store_n(&args->last_mismatched_handle, handle, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(&args->handle_mismatch_calls, 1, __ATOMIC_RELAXED);
        }
    }
    if (handle <= 0 || (handle & 0xffffff00) == 0 || (handle & 0x80000000) != 0)
        return;
    for (unsigned i = 0; i < 4u; ++i) {
        int32_t obs = __atomic_load_n(&args->observed_handles[i], __ATOMIC_RELAXED);
        if (obs == handle) {
            (void)__atomic_fetch_add(&args->observed_handle_calls[i], 1, __ATOMIC_RELAXED);
            return;
        }
        if (obs == -1 || obs == 0) {
            int32_t expected = obs;
            if (__atomic_compare_exchange_n(&args->observed_handles[i], &expected, handle,
                                            0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                (void)__atomic_fetch_add(&args->observed_handle_calls[i], 1, __ATOMIC_RELAXED);
                return;
            }
            if (__atomic_load_n(&args->observed_handles[i], __ATOMIC_RELAXED) == handle) {
                (void)__atomic_fetch_add(&args->observed_handle_calls[i], 1, __ATOMIC_RELAXED);
                return;
            }
        }
    }
}

static __attribute__((always_inline)) inline void
game_bridge_record_event(GamePadBridgeArgs *args, uint8_t stub_kind,
                         int32_t handle, int32_t return_val,
                         const void *pad_data, uint8_t base_flags)
{
    if (!args)
        return;
    uint32_t head = __atomic_fetch_add(&args->event_ring_head, 1, __ATOMIC_RELAXED);
    uint32_t slot = head % POORDS4_GAME_BRIDGE_EVENT_RING_SIZE;
    volatile PoorDS4InputEvent *evt = &args->event_ring[slot];

    evt->seq = head;
    evt->handle = handle;
    evt->stub_kind = stub_kind;

    uint8_t flags = base_flags;
    if (pad_data && return_val >= 0) {
        const ScePadData *pad = (const ScePadData *)pad_data;
        if (args->slots[0].reserved[2] == POORDS4_CLIENT_BINDING_MAGIC) {
            GamePadBridgeTriggerArgs *extension = (void *)args;
            if (extension->client_binding_magic == POORDS4_CLIENT_BINDING_MAGIC) {
                __atomic_store_n(&extension->consumer_output, (intptr_t)pad_data, __ATOMIC_RELAXED);
                __atomic_store_n(&extension->consumer_handle, handle, __ATOMIC_RELAXED);
                __atomic_store_n(&extension->consumer_result, return_val, __ATOMIC_RELAXED);
                __atomic_store_n(&extension->consumer_connected, pad->connected, __ATOMIC_RELAXED);
                __atomic_store_n(&extension->consumer_count, pad->count, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(&extension->consumer_samples, 1, __ATOMIC_RELAXED);
                if (!pad->connected)
                    (void)__atomic_fetch_add(&extension->consumer_disconnected, 1, __ATOMIC_RELAXED);
            }
        }
        evt->timestamp_ms = (uint32_t)(pad->timestamp / 1000ull);
        evt->buttons = pad->buttons;
        evt->lx = pad->leftStick.x;
        evt->ly = pad->leftStick.y;
        evt->rx = pad->rightStick.x;
        evt->ry = pad->rightStick.y;
        evt->l2 = pad->analogButtons.l2;
        evt->r2 = pad->analogButtons.r2;

        int active = (pad->buttons != 0) ||
                     (pad->analogButtons.l2 != 0) || (pad->analogButtons.r2 != 0) ||
                     (pad->leftStick.x < 96 || pad->leftStick.x > 160) ||
                     (pad->leftStick.y < 96 || pad->leftStick.y > 160) ||
                     (pad->rightStick.x < 96 || pad->rightStick.x > 160) ||
                     (pad->rightStick.y < 96 || pad->rightStick.y > 160);
        if (active)
            flags |= POORDS4_EVT_FLAG_CHANGED;
    } else {
        evt->timestamp_ms = 0;
        evt->buttons = 0;
        evt->lx = 128;
        evt->ly = 128;
        evt->rx = 128;
        evt->ry = 128;
        evt->l2 = 0;
        evt->r2 = 0;
    }
    evt->flags = flags;
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
    int32_t final_result = 0;
    int direct_copied = 0;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (args->slots[slot].is_dualsense) {
            if (original) {
                native_result = original(handle, out, 0);
                native_called = 1;
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1, __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result == 0) {
                game_bridge_record_native(args, out);
                (void)__atomic_fetch_add(
                    &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->read_state_calls, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->slots[slot].read_state_calls, 1, __ATOMIC_RELAXED);
            }
            final_result = native_called ? native_result :
                (original ? original(handle, out, 0) : native_result);
        } else if (game_bridge_direct_available_slot(args, (unsigned)slot)) {
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
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result == 0)
                game_bridge_record_native(args, out);
            int native_connected = native_result == 0 &&
                game_bridge_native_connected(out);
            int direct_active = game_bridge_direct_controls_active_slot(args, (unsigned)slot);
            if (native_connected)
                (void)__atomic_fetch_add(
                    &args->native_connected_frames, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(
                &args->direct_fallback_frames, 1, __ATOMIC_RELAXED);
            if (direct_active)
                (void)__atomic_fetch_add(
                    &args->direct_active_fallbacks, 1, __ATOMIC_RELAXED);
            if (game_bridge_copy_direct_slot(
                    args, (unsigned)slot, out, &args->read_state_calls,
                    &args->slots[slot].read_state_calls)) {
                final_result = 0;
                direct_copied = 1;
            } else {
                if (native_connected) {
                    (void)__atomic_fetch_add(
                        &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                    (void)__atomic_fetch_add(
                        &args->read_state_calls, 1, __ATOMIC_RELAXED);
                    (void)__atomic_fetch_add(
                        &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                }
                final_result = native_called ? native_result :
                    (original ? original(handle, out, 0) : native_result);
            }
        } else {
            final_result = native_called ? native_result :
                (original ? original(handle, out, 0) : native_result);
        }
    } else {
        final_result = native_called ? native_result :
            (original ? original(handle, out, 0) : native_result);
    }
    if (final_result == 0 && out)
        game_bridge_check_reset_shortcut(args, handle, out);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC && final_result == 0) {
        uint8_t flags = (direct_copied ? POORDS4_EVT_FLAG_DIRECT : POORDS4_EVT_FLAG_NATIVE);
        if (!matched) flags |= POORDS4_EVT_FLAG_MISMATCH;
        game_bridge_record_event(args, POORDS4_EVT_KIND_READ_STATE, handle, final_result, out, flags);
    }
    return final_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_state_ext_stub(int32_t handle, void *out,
                             GamePadBridgeArgs *args)
{
    typedef int32_t (*read_state_ext_fn)(int32_t, void *);
    typedef int32_t (*state_internal_fn)(int32_t, void *, int32_t);
    read_state_ext_fn original_ext =
        (read_state_ext_fn)(uintptr_t)(
            args ? args->read_state_ext_address : 0);
    state_internal_fn fallback_internal =
        (state_internal_fn)(uintptr_t)(
            args ? args->fp_state_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    int32_t final_result = 0;
    int direct_copied = 0;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (args->slots[slot].is_dualsense) {
            if (original_ext) {
                native_result = original_ext(handle, out);
                native_called = 1;
            } else if (fallback_internal) {
                native_result = fallback_internal(handle, out, 1);
                native_called = 1;
            }
            if (native_called) {
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1, __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result == 0) {
                game_bridge_record_native(args, out);
                (void)__atomic_fetch_add(
                    &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->read_state_ext_calls, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->slots[slot].read_state_ext_calls, 1, __ATOMIC_RELAXED);
            }
            final_result = native_called ? native_result :
                (original_ext ? original_ext(handle, out) :
                 (fallback_internal ? fallback_internal(handle, out, 1) : native_result));
        } else if (game_bridge_direct_available_slot(args, (unsigned)slot)) {
            if (original_ext) {
                native_result = original_ext(handle, out);
                native_called = 1;
            } else if (fallback_internal) {
                native_result = fallback_internal(handle, out, 1);
                native_called = 1;
            }
            if (native_called) {
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1,
                        __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result == 0)
                game_bridge_record_native(args, out);
            int native_connected = native_result == 0 &&
                game_bridge_native_connected(out);
            int direct_active = game_bridge_direct_controls_active_slot(args, (unsigned)slot);
            if (native_connected)
                (void)__atomic_fetch_add(
                    &args->native_connected_frames, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(
                &args->direct_fallback_frames, 1, __ATOMIC_RELAXED);
            if (direct_active)
                (void)__atomic_fetch_add(
                    &args->direct_active_fallbacks, 1, __ATOMIC_RELAXED);
            if (game_bridge_copy_direct_slot(
                    args, (unsigned)slot, out, &args->read_state_ext_calls,
                    &args->slots[slot].read_state_ext_calls)) {
                final_result = 0;
                direct_copied = 1;
            } else {
                if (native_connected) {
                    (void)__atomic_fetch_add(
                        &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                    (void)__atomic_fetch_add(
                        &args->read_state_ext_calls, 1, __ATOMIC_RELAXED);
                    (void)__atomic_fetch_add(
                        &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                }
                final_result = native_called ? native_result :
                    (original_ext ? original_ext(handle, out) :
                     (fallback_internal ? fallback_internal(handle, out, 1) : native_result));
            }
        } else {
            final_result = native_called ? native_result :
                (original_ext ? original_ext(handle, out) :
                 (fallback_internal ? fallback_internal(handle, out, 1) : native_result));
        }
    } else {
        final_result = native_called ? native_result :
            (original_ext ? original_ext(handle, out) :
             (fallback_internal ? fallback_internal(handle, out, 1) : native_result));
    }
    if (final_result == 0 && out)
        game_bridge_check_reset_shortcut(args, handle, out);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC && final_result == 0) {
        uint8_t flags = (direct_copied ? POORDS4_EVT_FLAG_DIRECT : POORDS4_EVT_FLAG_NATIVE);
        if (!matched) flags |= POORDS4_EVT_FLAG_MISMATCH;
        game_bridge_record_event(args, POORDS4_EVT_KIND_READ_STATE_EXT, handle, final_result, out, flags);
    }
    return final_result;
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
    int32_t final_result = 0;
    int direct_copied = 0;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out && num > 0 && num <= 64 &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (args->slots[slot].is_dualsense) {
            if (original) {
                native_result = original(handle, out, num, 0);
                native_called = 1;
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1, __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result > 0) {
                game_bridge_record_native(args, out);
                (void)__atomic_fetch_add(
                    &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->read_calls, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->slots[slot].read_calls, 1, __ATOMIC_RELAXED);
            }
            final_result = native_called ? native_result :
                (original ? original(handle, out, num, 0) : native_result);
        } else if (game_bridge_direct_available_slot(args, (unsigned)slot)) {
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
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result > 0)
                game_bridge_record_native(args, out);
            int native_connected = native_result > 0 &&
                game_bridge_native_connected(out);
            int direct_active = game_bridge_direct_controls_active_slot(args, (unsigned)slot);
            if (native_connected)
                (void)__atomic_fetch_add(
                    &args->native_connected_frames, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(
                &args->direct_fallback_frames, 1, __ATOMIC_RELAXED);
            if (direct_active)
                (void)__atomic_fetch_add(
                    &args->direct_active_fallbacks, 1, __ATOMIC_RELAXED);
            volatile uint32_t *read_seq_table =
                (volatile uint32_t *)(void *)args->legacy_pad_data;
            uint32_t current_seq = (slot == 0 && args->slots[0].direct_packets == 0)
                ? __atomic_load_n(&args->direct_seq, __ATOMIC_ACQUIRE)
                : __atomic_load_n(&args->slots[slot].direct_seq, __ATOMIC_ACQUIRE);
            uint32_t last_seq = (slot == 0 && args->slots[0].direct_packets == 0)
                ? read_seq_table[handle & 7u]
                : args->slots[slot].last_read_seq;
            volatile uint64_t *client_cursor = game_bridge_client_read_cursor(args, handle, 0);
            uint64_t cursor_before = client_cursor
                ? __atomic_load_n(client_cursor, __ATOMIC_RELAXED) : 0;
            if (client_cursor)
                last_seq = (uint32_t)(cursor_before >> 32) == (uint32_t)handle
                    ? (uint32_t)cursor_before : 0;
            if (current_seq == 0 || current_seq == last_seq) {
                /* Sony can overwrite out with its disconnected backing record
                 * before the queue says there is no new bridged sample. Unity
                 * still examines out[0].connected on an always-update frame.
                 * Keep the latest real packet in the buffer without reporting
                 * another event. No publication means nothing to restore. */
                int native_buffer_disconnected = !game_bridge_native_connected(out);
                if (current_seq != 0 && game_bridge_copy_direct_slot(
                        args, (unsigned)slot, out, NULL, NULL) &&
                    args->legacy_receiver_port == POORDS4_EMPTY_READ_DIAG_MAGIC) {
                    (void)__atomic_fetch_add(&args->legacy_receiver_packets, 1, __ATOMIC_RELAXED);
                    if (native_buffer_disconnected)
                        (void)__atomic_fetch_add(&args->legacy_receiver_timeouts, 1, __ATOMIC_RELAXED);
                }
                final_result = 0;
            } else {
                if (game_bridge_copy_direct_slot(
                        args, (unsigned)slot, out, &args->read_calls,
                        &args->slots[slot].read_calls)) {
                    read_seq_table[handle & 7u] = current_seq;
                    args->slots[slot].last_read_seq = current_seq;
                    final_result = 1;
                    direct_copied = 1;
                    if (client_cursor && !__atomic_compare_exchange_n(client_cursor,
                            &cursor_before, ((uint64_t)(uint32_t)handle << 32) | current_seq,
                            0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                        final_result = 0;
                        direct_copied = 0;
                    }
                } else {
                    if (native_connected) {
                        (void)__atomic_fetch_add(
                            &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                        (void)__atomic_fetch_add(
                            &args->read_calls, 1, __ATOMIC_RELAXED);
                        (void)__atomic_fetch_add(
                            &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                    }
                    final_result = native_called ? native_result :
                        (original ? original(handle, out, num, 0) : native_result);
                }
            }
        } else {
            final_result = native_called ? native_result :
                (original ? original(handle, out, num, 0) : native_result);
        }
    } else {
        final_result = native_called ? native_result :
            (original ? original(handle, out, num, 0) : native_result);
    }
    if (final_result > 0 && out)
        game_bridge_check_reset_shortcut(args, handle, out);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC) {
        if (final_result == 0) {
            (void)__atomic_fetch_add(&args->read_zero_returns, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&args->current_read_streak, 0, __ATOMIC_RELAXED);
        } else if (final_result > 0) {
            (void)__atomic_fetch_add(&args->read_nonzero_returns, 1, __ATOMIC_RELAXED);
            uint32_t streak = __atomic_add_fetch(&args->current_read_streak, 1, __ATOMIC_RELAXED);
            uint32_t cur_max = __atomic_load_n(&args->max_read_streak, __ATOMIC_RELAXED);
            while (streak > cur_max) {
                if (__atomic_compare_exchange_n(&args->max_read_streak, &cur_max, streak,
                                                0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    break;
            }
            uint8_t flags = (direct_copied ? POORDS4_EVT_FLAG_DIRECT : POORDS4_EVT_FLAG_NATIVE);
            if (!matched) flags |= POORDS4_EVT_FLAG_MISMATCH;
            game_bridge_record_event(args, POORDS4_EVT_KIND_READ, handle, final_result, out, flags);
        }
    }
    return final_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_read_ext_stub(int32_t handle, void *out, int32_t num,
                       GamePadBridgeArgs *args)
{
    typedef int32_t (*read_ext_fn)(int32_t, void *, int32_t);
    typedef int32_t (*read_internal_fn)(int32_t, void *, int32_t, int32_t);
    read_ext_fn original_ext =
        (read_ext_fn)(uintptr_t)(
            args ? args->read_ext_address : 0);
    read_internal_fn fallback_internal =
        (read_internal_fn)(uintptr_t)(
            args ? args->fp_read_internal : 0);
    int native_called = 0;
    int32_t native_result = (int32_t)0x80920001u;
    int32_t final_result = 0;
    int direct_copied = 0;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out && num > 0 && num <= 64 &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (args->slots[slot].is_dualsense) {
            if (original_ext) {
                native_result = original_ext(handle, out, num);
                native_called = 1;
            } else if (fallback_internal) {
                native_result = fallback_internal(handle, out, num, 1);
                native_called = 1;
            }
            if (native_called) {
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1, __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result > 0) {
                game_bridge_record_native(args, out);
                (void)__atomic_fetch_add(
                    &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->read_ext_calls, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->slots[slot].read_ext_calls, 1, __ATOMIC_RELAXED);
            }
            final_result = native_called ? native_result :
                (original_ext ? original_ext(handle, out, num) :
                 (fallback_internal ? fallback_internal(handle, out, num, 1) : native_result));
        } else if (game_bridge_direct_available_slot(args, (unsigned)slot)) {
            if (original_ext) {
                native_result = original_ext(handle, out, num);
                native_called = 1;
            } else if (fallback_internal) {
                native_result = fallback_internal(handle, out, num, 1);
                native_called = 1;
            }
            if (native_called) {
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1,
                        __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result > 0)
                game_bridge_record_native(args, out);
            int native_connected = native_result > 0 &&
                game_bridge_native_connected(out);
            int direct_active = game_bridge_direct_controls_active_slot(args, (unsigned)slot);
            if (native_connected)
                (void)__atomic_fetch_add(
                    &args->native_connected_frames, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(
                &args->direct_fallback_frames, 1, __ATOMIC_RELAXED);
            if (direct_active)
                (void)__atomic_fetch_add(
                    &args->direct_active_fallbacks, 1, __ATOMIC_RELAXED);
            volatile uint32_t *read_ext_seq_table =
                (volatile uint32_t *)(void *)(args->legacy_pad_data + 32);
            uint32_t current_seq = (slot == 0 && args->slots[0].direct_packets == 0)
                ? __atomic_load_n(&args->direct_seq, __ATOMIC_ACQUIRE)
                : __atomic_load_n(&args->slots[slot].direct_seq, __ATOMIC_ACQUIRE);
            uint32_t last_seq = (slot == 0 && args->slots[0].direct_packets == 0)
                ? read_ext_seq_table[handle & 7u]
                : args->slots[slot].last_read_seq;
            volatile uint64_t *client_cursor = game_bridge_client_read_cursor(args, handle, 1);
            uint64_t cursor_before = client_cursor
                ? __atomic_load_n(client_cursor, __ATOMIC_RELAXED) : 0;
            if (client_cursor)
                last_seq = (uint32_t)(cursor_before >> 32) == (uint32_t)handle
                    ? (uint32_t)cursor_before : 0;
            if (current_seq == 0 || current_seq == last_seq) {
                /* Preserve the real cached packet on empty queued reads; the
                 * return value remains zero, so no duplicate edge is emitted. */
                int native_buffer_disconnected = !game_bridge_native_connected(out);
                if (current_seq != 0 && game_bridge_copy_direct_slot(
                        args, (unsigned)slot, out, NULL, NULL) &&
                    args->legacy_receiver_port == POORDS4_EMPTY_READ_DIAG_MAGIC) {
                    (void)__atomic_fetch_add(&args->legacy_receiver_packets, 1, __ATOMIC_RELAXED);
                    if (native_buffer_disconnected)
                        (void)__atomic_fetch_add(&args->legacy_receiver_timeouts, 1, __ATOMIC_RELAXED);
                }
                final_result = 0;
            } else {
                if (game_bridge_copy_direct_slot(
                        args, (unsigned)slot, out, &args->read_ext_calls,
                        &args->slots[slot].read_ext_calls)) {
                    read_ext_seq_table[handle & 7u] = current_seq;
                    args->slots[slot].last_read_seq = current_seq;
                    final_result = 1;
                    direct_copied = 1;
                    if (client_cursor && !__atomic_compare_exchange_n(client_cursor,
                            &cursor_before, ((uint64_t)(uint32_t)handle << 32) | current_seq,
                            0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                        final_result = 0;
                        direct_copied = 0;
                    }
                } else {
                    if (native_connected) {
                        (void)__atomic_fetch_add(
                            &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                        (void)__atomic_fetch_add(
                            &args->read_ext_calls, 1, __ATOMIC_RELAXED);
                        (void)__atomic_fetch_add(
                            &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                    }
                    final_result = native_called ? native_result :
                        (original_ext ? original_ext(handle, out, num) :
                         (fallback_internal ? fallback_internal(handle, out, num, 1) : native_result));
                }
            }
        } else {
            final_result = native_called ? native_result :
                (original_ext ? original_ext(handle, out, num) :
                 (fallback_internal ? fallback_internal(handle, out, num, 1) : native_result));
        }
    } else {
        final_result = native_called ? native_result :
            (original_ext ? original_ext(handle, out, num) :
             (fallback_internal ? fallback_internal(handle, out, num, 1) : native_result));
    }
    if (final_result > 0 && out)
        game_bridge_check_reset_shortcut(args, handle, out);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC) {
        if (final_result == 0) {
            (void)__atomic_fetch_add(&args->read_zero_returns, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&args->current_read_streak, 0, __ATOMIC_RELAXED);
        } else if (final_result > 0) {
            (void)__atomic_fetch_add(&args->read_nonzero_returns, 1, __ATOMIC_RELAXED);
            uint32_t streak = __atomic_add_fetch(&args->current_read_streak, 1, __ATOMIC_RELAXED);
            uint32_t cur_max = __atomic_load_n(&args->max_read_streak, __ATOMIC_RELAXED);
            while (streak > cur_max) {
                if (__atomic_compare_exchange_n(&args->max_read_streak, &cur_max, streak,
                                                0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    break;
            }
            uint8_t flags = (direct_copied ? POORDS4_EVT_FLAG_DIRECT : POORDS4_EVT_FLAG_NATIVE);
            if (!matched) flags |= POORDS4_EVT_FLAG_MISMATCH;
            game_bridge_record_event(args, POORDS4_EVT_KIND_READ_EXT, handle, final_result, out, flags);
        }
    }
    return final_result;
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
    int32_t final_result = 0;
    int direct_copied = 0;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out &&
        args->pad_size == POORDS4_GAME_BRIDGE_PAD_SIZE) {
        if (args->slots[slot].is_dualsense) {
            if (original) {
                native_result = original(handle, out, 1);
                native_called = 1;
                (void)__atomic_fetch_add(
                    &args->native_backing_calls, 1, __ATOMIC_RELAXED);
                if (native_result < 0)
                    (void)__atomic_fetch_add(
                        &args->native_backing_errors, 1, __ATOMIC_RELAXED);
            }
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result == 0) {
                game_bridge_record_native(args, out);
                (void)__atomic_fetch_add(
                    &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->data_internal_calls, 1, __ATOMIC_RELAXED);
                (void)__atomic_fetch_add(
                    &args->slots[slot].data_internal_calls, 1, __ATOMIC_RELAXED);
            }
            final_result = native_called ? native_result :
                (original ? original(handle, out, 1) : native_result);
        } else if (game_bridge_direct_available_slot(args, (unsigned)slot)) {
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
            __atomic_store_n(
                &args->last_native_result, native_result, __ATOMIC_RELAXED);
            if (native_called && native_result == 0)
                game_bridge_record_native(args, out);
            int native_connected = native_result == 0 &&
                game_bridge_native_connected(out);
            int direct_active = game_bridge_direct_controls_active_slot(args, (unsigned)slot);
            if (native_connected)
                (void)__atomic_fetch_add(
                    &args->native_connected_frames, 1, __ATOMIC_RELAXED);
            (void)__atomic_fetch_add(
                &args->direct_fallback_frames, 1, __ATOMIC_RELAXED);
            if (direct_active)
                (void)__atomic_fetch_add(
                    &args->direct_active_fallbacks, 1, __ATOMIC_RELAXED);
            if (game_bridge_copy_direct_slot(
                    args, (unsigned)slot, out, &args->data_internal_calls,
                    &args->slots[slot].data_internal_calls)) {
                final_result = 0;
                direct_copied = 1;
            } else {
                if (native_connected) {
                    (void)__atomic_fetch_add(
                        &args->native_passthrough_frames, 1, __ATOMIC_RELAXED);
                    (void)__atomic_fetch_add(
                        &args->data_internal_calls, 1, __ATOMIC_RELAXED);
                    (void)__atomic_fetch_add(
                        &args->slots[slot].native_passthrough_frames, 1, __ATOMIC_RELAXED);
                }
                final_result = native_called ? native_result :
                    (original ? original(handle, out, 1) : native_result);
            }
        } else {
            final_result = native_called ? native_result :
                (original ? original(handle, out, 1) : native_result);
        }
    } else {
        final_result = native_called ? native_result :
            (original ? original(handle, out, 1) : native_result);
    }
    if (final_result == 0 && out)
        game_bridge_check_reset_shortcut(args, handle, out);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC && final_result == 0) {
        uint8_t flags = (direct_copied ? POORDS4_EVT_FLAG_DIRECT : POORDS4_EVT_FLAG_NATIVE);
        if (!matched) flags |= POORDS4_EVT_FLAG_MISMATCH;
        game_bridge_record_event(args, POORDS4_EVT_KIND_DATA_INTERNAL, handle, final_result, out, flags);
    }
    return final_result;
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
    uint8_t live_pad[POORDS4_GAME_BRIDGE_PAD_SIZE];
    typedef int32_t (*get_info_fn)(int32_t, void *);
    get_info_fn original = (get_info_fn)(uintptr_t)(
        args ? args->fp_get_controller_info_trampoline : 0);
    int32_t result = original
        ? original(handle, out) : (int32_t)0x80920001u;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args)
        (void)__atomic_fetch_add(
            &args->controller_info_calls, 1, __ATOMIC_RELAXED);
    if (args &&
        args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out &&
        !args->slots[slot].is_dualsense &&
        game_bridge_direct_available_slot(args, (unsigned)slot) &&
        game_bridge_copy_direct_slot(args, (unsigned)slot, live_pad, NULL, NULL)) {
        volatile uint8_t *info = (volatile uint8_t *)out;
        uint16_t touch_x = (uint16_t)info[4] |
            ((uint16_t)info[5] << 8u);
        uint16_t touch_y = (uint16_t)info[6] |
            ((uint16_t)info[7] << 8u);
        /* Preserve a successful firmware-native metadata prefix. This matters
         * on 8.60, whose live controller reports 1920x1080 rather than the
         * later build's geometry. Normalize DS4 942-line touch to 1080 to match
         * scaled coordinates and prevent Unreal Engine assertion panics. */
        if (result != 0 || touch_x == 0 || touch_x > 8192u ||
            touch_y == 0 || touch_y > 8192u || touch_y == 942u) {
            info[0] = 0x00;
            info[1] = 0x00;
            info[2] = 0x80;
            info[3] = 0x3f;
            info[4] = 0x80;
            info[5] = 0x07;
            info[6] = 0x38;
            info[7] = 0x04;
            info[8] = 13;
            info[9] = 13;
            info[10] = 1; /* Standard controller class (DualSense) */
            for (unsigned byte = 20; byte < 28; ++byte)
                info[byte] = 0;
        }
        info[10] = 1; /* Standard controller class (DualSense) */
        /* The same physical feed supplies input and connection generation. */
        info[11] = live_pad[offsetof(ScePadData, count)];
        info[12] = 1;
        info[13] = 0;
        info[14] = 0;
        info[15] = 0;
        info[16] = 0;
        info[17] = 0;
        info[18] = 0;
        info[19] = 0;
        (void)__atomic_fetch_add(
            &args->controller_info_spoofs, 1, __ATOMIC_RELAXED);
        if (result != 0)
            (void)__atomic_fetch_add(
                &args->controller_info_result_overrides, 1,
                __ATOMIC_RELAXED);
        result = 0;
    }
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC) {
        uint8_t flags = (matched ? 0 : POORDS4_EVT_FLAG_MISMATCH);
        game_bridge_record_event(args, POORDS4_EVT_KIND_CONTROLLER_INFO, handle, result, NULL, flags);
    }
    return result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_get_ext_controller_info_stub(int32_t handle, void *out,
                                      GamePadBridgeArgs *args)
{
    uint8_t live_pad[POORDS4_GAME_BRIDGE_PAD_SIZE];
    typedef int32_t (*get_ext_info_fn)(int32_t, void *);
    get_ext_info_fn original = (get_ext_info_fn)(uintptr_t)(
        args ? args->fp_get_ext_controller_info_trampoline : 0);
    int32_t result = original
        ? original(handle, out) : (int32_t)0x80920001u;
    int slot = (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        ? game_bridge_find_slot(args, handle) : -1;
    int matched = (slot >= 0);
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC)
        game_bridge_record_call(args, handle, matched);
    if (args)
        (void)__atomic_fetch_add(
            &args->controller_info_calls, 1, __ATOMIC_RELAXED);
    if (args &&
        args->magic == POORDS4_GAME_BRIDGE_MAGIC &&
        matched && out &&
        !args->slots[slot].is_dualsense &&
        game_bridge_direct_available_slot(args, (unsigned)slot) &&
        game_bridge_copy_direct_slot(args, (unsigned)slot, live_pad, NULL, NULL)) {
        volatile uint8_t *info = (volatile uint8_t *)out;
        uint16_t touch_x = (uint16_t)info[4] |
            ((uint16_t)info[5] << 8u);
        uint16_t touch_y = (uint16_t)info[6] |
            ((uint16_t)info[7] << 8u);
        if (result != 0 || touch_x == 0 || touch_x > 8192u ||
            touch_y == 0 || touch_y > 8192u || touch_y == 942u) {
            info[0] = 0x00;
            info[1] = 0x00;
            info[2] = 0x80;
            info[3] = 0x3f;
            info[4] = 0x80;
            info[5] = 0x07;
            info[6] = 0x38;
            info[7] = 0x04;
            info[8] = 13;
            info[9] = 13;
            info[10] = 1;
            for (unsigned byte = 20; byte < 28; ++byte)
                info[byte] = 0;
        }
        info[10] = 1; /* Standard controller class (DualSense) */
        info[11] = live_pad[offsetof(ScePadData, count)];
        info[12] = 1;
        info[13] = 0;
        info[14] = 0;
        info[15] = 0;
        info[16] = 0;
        info[17] = 0;
        info[18] = 0;
        info[19] = 0;
        if (result != 0) {
            info[28] = 0x01;
            for (unsigned byte = 29; byte < 48; ++byte)
                info[byte] = 0;
        }
        (void)__atomic_fetch_add(
            &args->controller_info_spoofs, 1, __ATOMIC_RELAXED);
        if (result != 0)
            (void)__atomic_fetch_add(
                &args->controller_info_result_overrides, 1,
                __ATOMIC_RELAXED);
        result = 0;
    }
    if (args && args->magic == POORDS4_GAME_BRIDGE_MAGIC) {
        uint8_t flags = (matched ? 0 : POORDS4_EVT_FLAG_MISMATCH);
        game_bridge_record_event(args, POORDS4_EVT_KIND_EXT_CONTROLLER_INFO, handle, result, NULL, flags);
    }
    return result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_get_trigger_effect_state_stub(int32_t handle,void *out,GamePadBridgeArgs *args)
{
    if(!args || args->magic!=POORDS4_GAME_BRIDGE_MAGIC)return -1;
    GamePadBridgeTriggerArgs *transport=(void*)args;
    if(transport->magic!=POORDS4_TRIGGER_ARGS_MAGIC)return -1;
    int32_t (*native)(int32_t,void*)=(void*)transport->fp_get_trigger_effect_state;
    if((uintptr_t)native<0x10000 || (uintptr_t)native>=UINT64_C(0x0000800000000000))return -1;
    (void)__atomic_fetch_add(&transport->calls,1,__ATOMIC_RELAXED);
    int result=native(handle,out);
    __atomic_store_n(&transport->last_native_result,result,__ATOMIC_RELAXED);
    /* Preserve successful native states and every non-DS4 route. A DS4 has
     * no adaptive motor effect; report neutral only for its live bridged
     * input. The PS5 caller buffer is eight bytes, not the Windows two-byte
     * state and not the 120-byte effect-command record. */
    if(!out || (uintptr_t)out<0x10000 ||
       (uintptr_t)out>UINT64_C(0x0000800000000000)-8u)return result;
    int slot=game_bridge_find_slot(args,handle);
    if(slot>=0 && !args->slots[slot].is_dualsense && !args->slots[slot].is_simulated &&
       transport->effect_magic==POORDS4_EFFECT_ARGS_MAGIC) {
        PoorDS4TriggerParam requested;
        if(poords4_trigger_snapshot(&transport->effects[slot],&requested) &&
           requested.mask && game_bridge_direct_available_slot(args,(unsigned)slot)) {
            ScePadData pad;
            uint32_t *seq=slot==0 && !args->slots[0].direct_packets?
                (uint32_t*)&args->direct_seq:(uint32_t*)&args->slots[slot].direct_seq;
            unsigned stable=0;
            for(unsigned attempt=0;attempt<3;attempt++) {
                uint32_t before=__atomic_load_n(seq,__ATOMIC_ACQUIRE);
                const volatile uint8_t *source=slot==0 && !args->slots[0].direct_packets?
                    args->direct_pad_data[before&1u]:args->slots[slot].direct_pad_data[before&1u];
                uint8_t *destination=(uint8_t*)&pad;
                for(unsigned byte=0;byte<sizeof pad;byte++)destination[byte]=source[byte];
                if(before==__atomic_load_n(seq,__ATOMIC_ACQUIRE)){stable=1;break;}
            }
            if(stable && pad.connected) {
                uint8_t travel[2]={pad.analogButtons.l2,pad.analogButtons.r2};
                if(!travel[0] && (pad.buttons&0x100u))travel[0]=255;
                if(!travel[1] && (pad.buttons&0x200u))travel[1]=255;
                int32_t state[2];
                for(unsigned side=0;side<2;side++)state[side]=requested.mask&(1u<<side)?
                    poords4_trigger_state(&requested.commands[side],travel[side]):0;
                volatile uint8_t *destination=out;const uint8_t *source=(const uint8_t*)state;
                for(unsigned byte=0;byte<sizeof state;byte++)destination[byte]=source[byte];
                (void)__atomic_fetch_add(&transport->effect_state_overrides,1,__ATOMIC_RELAXED);
                return 0;
            }
        }
    }
    if(result==0)return result;
    if(slot<0 || args->slots[slot].is_dualsense || args->slots[slot].is_simulated ||
       !game_bridge_direct_available_slot(args,(unsigned)slot))return result;
    volatile uint8_t *bytes=out;
    for(unsigned byte=0;byte<8;byte++)bytes[byte]=0;
    (void)__atomic_fetch_add(&transport->neutral_overrides,1,__ATOMIC_RELAXED);
    return 0;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_set_trigger_effect_stub(int32_t handle,const void *input,GamePadBridgeArgs *args)
{
    if(!args || args->magic!=POORDS4_GAME_BRIDGE_MAGIC)return -1;
    GamePadBridgeTriggerArgs *transport=(void*)args;
    if(transport->magic!=POORDS4_TRIGGER_ARGS_MAGIC ||
       transport->effect_magic!=POORDS4_EFFECT_ARGS_MAGIC)return -1;
    int32_t (*native)(int32_t,const void*)=(void*)transport->fp_set_trigger_effect;
    if((uintptr_t)native<0x10000 || (uintptr_t)native>=UINT64_C(0x0000800000000000))return -1;
    /* Exactly the command the game already sent; no new source/motor command. */
    int32_t result=native(handle,input);
    (void)__atomic_fetch_add(&transport->effect_set_calls,1,__ATOMIC_RELAXED);
    __atomic_store_n(&transport->effect_native_result,result,__ATOMIC_RELAXED);
    int slot=game_bridge_find_slot(args,handle);
    uint32_t reason=0, mask=UINT32_MAX, left=UINT32_MAX, right=UINT32_MAX;
    if(slot<0)reason=2;
    else if(args->slots[slot].is_dualsense)reason=3;
    else if(args->slots[slot].is_simulated)reason=4;
    else if(!input || (uintptr_t)input<0x10000 ||
       (uintptr_t)input>UINT64_C(0x0000800000000000)-sizeof(PoorDS4TriggerParam))reason=5;
    if(reason)goto finished;
    /* Desired game commands survive startup before the first physical packet.
     * The legacy receiver flag is unrelated to queued per-slot publication. */
    int live=game_bridge_direct_available_slot(args,(unsigned)slot);
    PoorDS4TriggerParam requested;
    const volatile uint8_t *source=input;uint8_t *destination=(uint8_t*)&requested;
    for(unsigned byte=0;byte<sizeof requested;byte++)destination[byte]=source[byte];
    mask=requested.mask;left=requested.commands[0].mode;right=requested.commands[1].mode;
    if(!poords4_trigger_param_valid(&requested)){reason=7;goto finished;}
    if(poords4_trigger_publish(&transport->effects[slot],&requested)){
        reason=8;if(live)result=-1;goto finished;
    }
    (void)__atomic_fetch_add(&transport->effect_set_accepts,1,__ATOMIC_RELAXED);
    if(live){
        result=0;
        (void)__atomic_fetch_add(&transport->effect_compat_successes,1,__ATOMIC_RELAXED);
    }else{
        reason=6;
        (void)__atomic_fetch_add(&transport->effect_cached_unavailable,1,__ATOMIC_RELAXED);
    }
finished:
    if(transport->effect_diag_magic==POORDS4_EFFECT_DIAG_MAGIC){
        (void)__atomic_fetch_add(&transport->effect_skip_counts[reason],1,__ATOMIC_RELAXED);
        __atomic_store_n(&transport->effect_last_skip,reason,__ATOMIC_RELAXED);
        __atomic_store_n(&transport->effect_last_slot,slot,__ATOMIC_RELAXED);
        __atomic_store_n(&transport->effect_last_call_handle,handle,__ATOMIC_RELAXED);
        __atomic_store_n(&transport->effect_last_mask,mask,__ATOMIC_RELAXED);
        __atomic_store_n(&transport->effect_last_left_mode,left,__ATOMIC_RELAXED);
        __atomic_store_n(&transport->effect_last_right_mode,right,__ATOMIC_RELAXED);
    }
    return result;
}

static __attribute__((always_inline)) inline PoorDS4GameOutput *
game_bridge_rumble_output(GamePadBridgeArgs *args)
{
    if(!args || args->magic!=POORDS4_GAME_BRIDGE_MAGIC)return 0;
    GamePadBridgeTriggerArgs *transport=(void*)args;
    if(transport->magic!=POORDS4_TRIGGER_ARGS_MAGIC)return 0;
    PoorDS4GameOutput *output=&transport->rumble;
    return output->magic==POORDS4_GAME_OUTPUT_MAGIC && output->version==1 &&
        output->size==sizeof *output && output->slots==4?output:0;
}

/* Native fallback is -2; accepted mailbox state is 0, not a motor ACK. */
static __attribute__((always_inline)) inline int
game_bridge_publish_rumble(GamePadBridgeArgs *args,int32_t handle,const void *input,int mode_only)
{
    PoorDS4GameOutput *output=game_bridge_rumble_output(args);
    if(!output || !args->active)return -2;
    int slot=game_bridge_find_slot(args,handle);
    if(slot<0 || args->slots[slot].is_dualsense || args->slots[slot].is_simulated ||
       !game_bridge_direct_available_slot(args,(unsigned)slot))return -2;
    PoorDS4GameOutputSlot *channel=&output->channels[slot];
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&channel->lock,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return -1;
    uint32_t route_seq=__atomic_load_n(&channel->route.sequence,__ATOMIC_ACQUIRE);
    int result=-2;
    if(!(route_seq&1) && channel->route.enabled && channel->route.generation &&
       channel->route.session && channel->route.epoch && channel->route.handle>=0 &&
       channel->route.user>=0) {
        if(mode_only)result=0;
        else if(input && (uintptr_t)input>=0x10000 &&
                (uintptr_t)input<=UINT64_C(0x0000800000000000)-2u) {
            uint64_t published=__atomic_load_n(&channel->published,__ATOMIC_RELAXED),now;
            if(published==UINT64_MAX || poords4_output_time(output->fp_clock,&now))result=-1;
            else {
                const volatile uint8_t *motors=input;
                PoorDS4GameOutputCommand command={.generation=channel->route.generation,
                    .session=channel->route.session,.epoch=channel->route.epoch,
                    .sequence=published+1,.created_at=now,.handle=channel->route.handle,
                    .user=channel->route.user,.vibration={motors[0],motors[1]}};
                if(route_seq==__atomic_load_n(&channel->route.sequence,__ATOMIC_ACQUIRE)) {
                    volatile uint8_t *destination=(volatile uint8_t*)&channel->commands[command.sequence&1u];
                    const uint8_t *source=(const uint8_t*)&command;
                    for(unsigned byte=0;byte<sizeof command;byte++)destination[byte]=source[byte];
                    __atomic_store_n(&channel->published,command.sequence,__ATOMIC_RELEASE);result=0;
                    (void)__atomic_fetch_add(&output->publications,1,__ATOMIC_RELAXED);
                }
            }
        }
        if(route_seq!=__atomic_load_n(&channel->route.sequence,__ATOMIC_ACQUIRE))result=-2;
    }
    __atomic_store_n(&channel->lock,0,__ATOMIC_RELEASE);return result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_set_vibration_stub(int32_t handle,const void *input,GamePadBridgeArgs *args)
{
    PoorDS4GameOutput *output=game_bridge_rumble_output(args);
    if(output) {
        (void)__atomic_fetch_add(&output->vibration_calls,1,__ATOMIC_RELAXED);
        __atomic_store_n(&output->last_caller,handle,__ATOMIC_RELAXED);
    }
    int result=game_bridge_publish_rumble(args,handle,input,0);
    if(result!=-2) {
        if(result && output)(void)__atomic_fetch_add(&output->failed_publications,1,__ATOMIC_RELAXED);
        return result;
    }
    int32_t (*native)(int32_t,const void*)=output?(void*)output->fp_vibration:0;
    int native_result=poords4_output_pointer((intptr_t)native)?native(handle,input):-1;
    if(output) {
        (void)__atomic_fetch_add(&output->native_calls,1,__ATOMIC_RELAXED);
        __atomic_store_n(&output->last_native_result,native_result,__ATOMIC_RELAXED);
    }
    return native_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_set_vibration_mode_stub(int32_t handle,int32_t mode,GamePadBridgeArgs *args)
{
    PoorDS4GameOutput *output=game_bridge_rumble_output(args);
    if(output) {
        (void)__atomic_fetch_add(&output->mode_calls,1,__ATOMIC_RELAXED);
        __atomic_store_n(&output->last_caller,handle,__ATOMIC_RELAXED);
        __atomic_store_n(&output->last_mode,mode,__ATOMIC_RELAXED);
    }
    int result=mode==2?game_bridge_publish_rumble(args,handle,0,1):-2;
    if(result!=-2)return result;
    int32_t (*native)(int32_t,int32_t)=output?(void*)output->fp_mode:0;
    int native_result=poords4_output_pointer((intptr_t)native)?native(handle,mode):-1;
    if(output) {
        (void)__atomic_fetch_add(&output->native_calls,1,__ATOMIC_RELAXED);
        __atomic_store_n(&output->last_native_result,native_result,__ATOMIC_RELAXED);
    }
    return native_result;
}

__attribute__((noinline, used, section(".text.ds4gamebridge")))
int32_t
game_pad_device_class_info_stub(int32_t handle, void *out, GamePadBridgeArgs *args)
{
    if (!args || args->magic != POORDS4_GAME_BRIDGE_MAGIC)
        return (int32_t)0x80920001u;
    GamePadBridgeTriggerArgs *transport = (void *)args;
    if (transport->device_class_diag_magic != POORDS4_DEVICE_CLASS_DIAG_MAGIC)
        return (int32_t)0x80920001u;
    int32_t (*native)(int32_t, void *) = (void *)transport->fp_device_class_info;
    int32_t result = poords4_output_pointer((intptr_t)native)
        ? native(handle, out) : (int32_t)0x80920001u;
    int slot = game_bridge_find_slot(args, handle);
    (void)__atomic_fetch_add(&transport->device_class_calls, 1, __ATOMIC_RELAXED);
    if (result < 0)
        (void)__atomic_fetch_add(&transport->device_class_errors, 1, __ATOMIC_RELAXED);
    if (slot >= 0)
        (void)__atomic_fetch_add(&transport->device_class_matched_calls, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&transport->device_class_last_result, result, __ATOMIC_RELAXED);
    __atomic_store_n(&transport->device_class_last_handle, handle, __ATOMIC_RELAXED);
    __atomic_store_n(&transport->device_class_last_slot, slot, __ATOMIC_RELAXED);
    if (result == 0 && out) {
        const volatile uint8_t *bytes = out;
        uint32_t value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
            ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
        __atomic_store_n(&transport->device_class_last_value, (int32_t)value, __ATOMIC_RELAXED);
    }
    /* Observation only: do not override Sony's result, buffer or lifecycle. */
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
    if (wireless_ds4_game_bridge_image_state(pid) != 0)
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
    if (wireless_ds4_game_bridge_image_state(pid) != 0)
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

/* Current supervisor's installed game mapping only. Routes are published
 * with a seqlock without changing command/lock ownership in the game. */
#if defined(POORDS4_GAME_RUMBLE_QUALIFY)
static GamePadBridgeTriggerArgs g_rumble_qualification_game;
static int g_rumble_qualification_active;
#endif
static int
game_rumble_transfer(pid_t pid,intptr_t address,void *data,size_t size,int write)
{
#if defined(POORDS4_GAME_RUMBLE_QUALIFY)
    if(g_rumble_qualification_active && pid==getpid()) {
        uintptr_t base=(uintptr_t)&g_rumble_qualification_game;
        if((uintptr_t)address<base || (uintptr_t)address-base>sizeof g_rumble_qualification_game ||
           size>sizeof g_rumble_qualification_game-((uintptr_t)address-base))return -1;
        if(write)memcpy((void*)address,data,size);else memcpy(data,(void*)address,size);
        return 0;
    }
#endif
    return write?game_bridge_process_write(pid,address,data,size):
                 game_bridge_process_read(pid,address,data,size);
}

static int
game_rumble_header(pid_t pid,intptr_t address,PoorDS4GameOutput *header)
{
    if(pid!=g_game_bridge_direct_pid || address!=g_game_bridge_direct_args ||
       pid<=0 || address<=0 || !header)return -1;
    GamePadBridgeArgs prefix;
    if(game_rumble_transfer(pid,address,&prefix,sizeof prefix,0) || prefix.magic!=POORDS4_GAME_BRIDGE_MAGIC ||
       prefix.reserved1!=POORDS4_GAME_BRIDGE_LAYOUT_V1)return -1;
    intptr_t output=address+offsetof(GamePadBridgeTriggerArgs,rumble);
    if(game_rumble_transfer(pid,output,header,offsetof(PoorDS4GameOutput,channels),0) ||
       header->magic!=POORDS4_GAME_OUTPUT_MAGIC || header->version!=1 ||
       header->size!=sizeof *header || header->slots!=4)return -1;
    return 0;
}

int
wireless_ds4_game_rumble_route(pid_t pid,intptr_t address,unsigned slot,
    uint64_t generation,const PoorDS4RumbleStatus *source,int enabled)
{
    PoorDS4GameOutput header;
    if(slot>=4 || game_rumble_header(pid,address,&header))return -1;
    if(enabled && (!source || !source->connected || !source->epoch || !source->session ||
        source->quarantined || !generation || !poords4_output_pointer(header.fp_clock) ||
        !poords4_output_pointer(header.fp_vibration)))return -1;
    intptr_t route_address=address+offsetof(GamePadBridgeTriggerArgs,rumble)+
        offsetof(PoorDS4GameOutput,channels)+slot*sizeof(PoorDS4GameOutputSlot);
    PoorDS4GameOutputRoute old;
    if(game_rumble_transfer(pid,route_address,&old,sizeof old,0) ||
       (old.sequence&1u) || old.sequence>UINT32_MAX-2u)return -1;
    PoorDS4GameOutputRoute next={.sequence=old.sequence+1,.enabled=!!enabled,
        .generation=generation,.session=enabled?source->session:0,
        .epoch=enabled?source->epoch:0,.user=enabled?source->user:-1,
        .handle=enabled?source->handle:-1};
    if(game_rumble_transfer(pid,route_address,&next,sizeof next,1))return -1;
    uint32_t published=old.sequence+2;
    return game_rumble_transfer(pid,route_address,&published,sizeof published,1);
}

int
wireless_ds4_game_rumble_request(pid_t pid,intptr_t address,unsigned slot,
    PoorDS4GameOutputCommand *command,uint64_t *published)
{
    PoorDS4GameOutput header;
    if(!command || !published || slot>=4 || game_rumble_header(pid,address,&header))return -1;
    intptr_t channel=address+offsetof(GamePadBridgeTriggerArgs,rumble)+
        offsetof(PoorDS4GameOutput,channels)+slot*sizeof(PoorDS4GameOutputSlot);
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint64_t before,after;
        if(game_rumble_transfer(pid,channel+offsetof(PoorDS4GameOutputSlot,published),&before,sizeof before,0))return -1;
        if(!before){*published=0;return 0;}
        if(game_rumble_transfer(pid,channel+offsetof(PoorDS4GameOutputSlot,commands)+
            (before&1u)*sizeof *command,command,sizeof *command,0) ||
           game_rumble_transfer(pid,channel+offsetof(PoorDS4GameOutputSlot,published),&after,sizeof after,0))return -1;
        if(before==after && command->sequence==before){*published=before;return 0;}
    }
    return -1;
}

#if defined(POORDS4_GAME_RUMBLE_QUALIFY)
#include "game_rumble_relay.h"
static int qualification_native_vibration(int32_t handle,const void *value)
{(void)handle;(void)value;return -1;}
static int qualification_native_mode(int32_t handle,int32_t mode)
{(void)handle;(void)mode;return -1;}

/* Ordinary SDK consumer of production callbacks against owned local memory,
 * then the real source channel. No game input, foreign callback/thread call,
 * paging probe or native-app staging. Compiled only for qualification. */
int
wireless_ds4_game_rumble_native_qualification(pid_t reader_pid,intptr_t reader_args)
{
    if(g_game_bridge_direct_pid>0 || g_game_bridge_direct_args)return -1;
    PoorDS4RumbleStatus source;
    if(wireless_ds4_reader_rumble_status(reader_pid,reader_args,&source) ||
       !source.connected || source.effect_active)return -1;
    GamePadBridgeTriggerArgs *fixture=&g_rumble_qualification_game;
    memset(fixture,0,sizeof *fixture);
    GamePadBridgeArgs *input=&fixture->input;
    input->magic=POORDS4_GAME_BRIDGE_MAGIC;input->reserved1=POORDS4_GAME_BRIDGE_LAYOUT_V1;
    input->active=1;input->pad_handle=source.handle;input->direct_active=1;input->direct_lease=1200;
    for(unsigned slot=0;slot<4;slot++){input->slots[slot].pad_handle=-1;input->slots[slot].pad_index=-1;}
    input->slots[0].pad_handle=source.handle;input->slots[0].pad_index=0;
    input->slots[0].active=1;input->slots[0].direct_lease=1200;
    fixture->magic=POORDS4_TRIGGER_ARGS_MAGIC;
    fixture->rumble.magic=POORDS4_GAME_OUTPUT_MAGIC;fixture->rumble.version=1;
    fixture->rumble.size=sizeof fixture->rumble;fixture->rumble.slots=4;
    fixture->rumble.fp_clock=(intptr_t)clock_gettime;
    fixture->rumble.fp_vibration=(intptr_t)qualification_native_vibration;
    fixture->rumble.fp_mode=(intptr_t)qualification_native_mode;
    g_rumble_qualification_active=1;
    g_game_bridge_direct_pid=getpid();g_game_bridge_direct_args=(intptr_t)fixture;
    PoorDS4GameRumbleRelay relay;
    poords4_game_rumble_open(&relay,reader_pid,reader_args,getpid(),(intptr_t)fixture,0,1);
    int result=-1;
    if(!relay.enabled || game_pad_set_vibration_mode_stub(source.handle,2,input))goto done;
    for(unsigned step=0;step<3;step++) {
        PoorDS4Vibration motors={step==1?32:0,step==1?32:0};
        if(game_pad_set_vibration_stub(source.handle,&motors,input))goto done;
        uint64_t expected=relay.acknowledgments+1;
        for(unsigned attempt=0;attempt<50 && relay.acknowledgments<expected;attempt++) {
            uint64_t now=0;
            if(poords4_output_time((intptr_t)clock_gettime,&now))goto done;
            poords4_game_rumble_tick(&relay,now,1,0);
            if(relay.failed)goto done;
            usleep(10000);
        }
        if(relay.acknowledgments!=expected)goto done;
        klog_printf("[PoorDS4] GAME RUMBLE QUALIFY step=%u published=%llu sent=%llu native_ack=%llu\n",
            step,(unsigned long long)fixture->rumble.publications,
            (unsigned long long)relay.submissions,(unsigned long long)relay.acknowledgments);
        if(step==1)usleep(180000);
    }
    result=0;
done:
    if(poords4_game_rumble_close(&relay,1))result=-1;
    klog_printf("[PoorDS4] GAME RUMBLE QUALIFY end result=%d consumer=owned-sdk callbacks=%llu publications=%llu native_ack=%llu\n",
        result,(unsigned long long)fixture->rumble.vibration_calls,
        (unsigned long long)fixture->rumble.publications,(unsigned long long)relay.acknowledgments);
    g_rumble_qualification_active=0;g_game_bridge_direct_pid=-1;g_game_bridge_direct_args=0;
    return result;
}
#endif

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
    intptr_t fn_closepad = 0;
    int selected_opened_here = 0;
    unsigned ds4_count = 0;
    int32_t multi_handles[POORDS4_MAX_SLOTS] = {-1, -1, -1, -1};
    int multi_opened[POORDS4_MAX_SLOTS] = {0, 0, 0, 0};
    int32_t multi_indices[POORDS4_MAX_SLOTS] = {-1, -1, -1, -1};
    int output_local=-1,output_remote=-1;
    intptr_t output_close=0;
    struct sockaddr_in output_address;
    memset(&output_address,0,sizeof output_address);

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
    fn_closepad = libpad_h
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
    intptr_t fn_setloginuser = libpad_h
        ? resolve_sym(target, libpad_h, "scePadSetLoginUserNumber") : 0;
    intptr_t fn_setfocus = libpad_h
        ? resolve_sym(target, libpad_h, "scePadSetProcessFocus") : 0;
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
                "kill=0x%lx malloc=0x%lx setpriv=0x%lx "
                "setlogin=0x%lx setfocus=0x%lx "
                "trap=0x%lx base=0x%lx fw=0x%08x\n",
                fn_gethandle, fn_open, fn_closepad, fn_isds4,
                fn_getdeviceid, fn_getdeviceinfo,
                fn_readstate, fn_usleep,
                fn_pthread_create, fn_pthread_detach, fn_kill,
                fn_malloc, fn_setpriv, fn_setloginuser,
                fn_setfocus, trap_mem,
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
                "poords4_version=%s\nreport_schema=9\n"
                "firmware=0x%08x\npid=%d\nlibpad_base=0x%lx\n",
                POORDS4_VERSION, firmware, target,
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

    int64_t setpriv_result = INT64_MIN;
    int64_t setlogin_result = INT64_MIN;
    int64_t setfocus_result = INT64_MIN;
    if (fn_setpriv)
        setpriv_result = pt_call(target, fn_setpriv, trap_mem,
                                 1, 0, 0, 0, 0, 0);
    /* Older libScePad revisions do not necessarily give a privileged client
     * input focus merely because scePadOpen succeeds. Ghostpad's recovered
     * client setup establishes all three process-local routing values before
     * opening a pad. Perform the calls on RemotePlay's stopped main thread,
     * never concurrently from the injected reader thread. These calls select
     * one already logged-in user context; they do not create a virtual pad or
     * display the controller-assignment UI. */
    if (fn_setloginuser)
        setlogin_result = pt_call(target, fn_setloginuser, trap_mem,
                                  1, 0, 0, 0, 0, 0);
    if (fn_setfocus)
        setfocus_result = pt_call(target, fn_setfocus, trap_mem,
                                  1, 0, 0, 0, 0, 0);
    klog_printf(
        "[PoorDS4] reader context fw=0x%08x privilege=0x%llx "
        "login_user=0x%llx focus=0x%llx\n",
        firmware,
        (unsigned long long)(uint64_t)setpriv_result,
        (unsigned long long)(uint64_t)setlogin_result,
        (unsigned long long)(uint64_t)setfocus_result);
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
            "reader_context setpriv_symbol=0x%lx result=0x%llx "
            "setlogin_symbol=0x%lx result=0x%llx "
            "setfocus_symbol=0x%lx result=0x%llx\n",
            (unsigned long)fn_setpriv,
            (unsigned long long)(uint64_t)setpriv_result,
            (unsigned long)fn_setloginuser,
            (unsigned long long)(uint64_t)setlogin_result,
            (unsigned long)fn_setfocus,
            (unsigned long long)(uint64_t)setfocus_result);
        if (source_report_fd >= 0)
            close(source_report_fd);
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
    uint16_t selected_device_vendor=0,selected_device_product=0;
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        g_reader_users[s] = -1;
        g_game_reader_slots[s] = -1;
    }
    uint16_t multi_device_vendor[POORDS4_MAX_SLOTS] = {0};
    uint16_t multi_device_product[POORDS4_MAX_SLOTS] = {0};
    for (uint32_t user_slot = 0; user_slot < user_count; ++user_slot) {
        int32_t candidate_user = user_ids[user_slot];
        if (candidate_user < 0)
            continue;
        for (int32_t pad_index = 0; pad_index < 10; ++pad_index) {
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
            int32_t event_result = -1;
            int32_t event_connected = 0;
            ScePadData event_data;
            memset(&event_data, 0, sizeof(event_data));
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
                if (fn_read) {
                    memset(&event_data, 0, sizeof(event_data));
                    if (pt_io_write(
                            target, probe_data_addr,
                            &event_data, sizeof(event_data)) == 0) {
                        event_result = (int32_t)pt_call(
                            target, fn_read, trap_mem,
                            (uint32_t)candidate_handle,
                            (uint64_t)probe_data_addr, 1, 0, 0, 0);
                        if (event_result > 0 &&
                            mdbg_copyout(
                                target, probe_data_addr, &event_data,
                                sizeof(event_data)) == 0)
                            event_connected = event_data.connected != 0;
                    }
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
                remote_pad_identity(
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
                device_info_copy == 0 &&
                (read_connected || event_connected) &&
                remote_pad_is_known_ds4(
                    1, device_vendor, device_product);
            /* On retail 11.60 this API returns a positive handle-derived
             * value (for example 0x030d0301), not the literal boolean 1. */
            int api_identity = is_ds4 > 0;
            klog_printf(
                "[PoorDS4] source candidate user=0x%08x index=%d "
                "handle=0x%08x is_ds4=0x%08x opened=%d "
                "connected=%d vid=0x%04x pid=0x%04x "
                "state=0x%08x state_connected=%d "
                "queued=0x%08x queued_connected=%d "
                "queued_buttons=0x%08x queued_sticks=%u,%u,%u,%u "
                "queued_triggers=%u,%u queued_timestamp=%llu "
                "queued_count=%u "
                "device_id_call=0x%08x device_id=0x%08x "
                "device_info=0x%08x copy=%d device_vid=0x%04x "
                "device_pid=0x%04x prefix64=",
                (uint32_t)candidate_user, pad_index,
                (uint32_t)candidate_handle, (uint32_t)is_ds4,
                opened_here, table_connected, table_vendor, table_product,
                (uint32_t)read_result, read_connected,
                (uint32_t)event_result, event_connected,
                event_data.buttons,
                event_data.leftStick.x, event_data.leftStick.y,
                event_data.rightStick.x, event_data.rightStick.y,
                event_data.analogButtons.l2,
                event_data.analogButtons.r2,
                (unsigned long long)event_data.timestamp,
                event_data.count,
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
                if (ds4_count < POORDS4_MAX_SLOTS) {
                    multi_handles[ds4_count] = candidate_handle;
                    if(public_identity) {
                        multi_device_vendor[ds4_count]=device_vendor;
                        multi_device_product[ds4_count]=device_product;
                    }
                    g_reader_users[ds4_count] = candidate_user;
                    multi_indices[ds4_count] = pad_index;
                    multi_opened[ds4_count] = opened_here;
                    if (ds4_count == 0) {
                        handle = candidate_handle;
                        selected_user = candidate_user;
                        selected_index = pad_index;
                        selected_is_ds4 = 1;
                        if(public_identity) {
                            selected_device_vendor=device_vendor;
                            selected_device_product=device_product;
                        }
                        selected_opened_here = opened_here;
                        klog_printf(
                            "[PoorDS4] source match method=%s\n",
                            public_identity ? "public-device-info" :
                            (api_identity ? "api" : "fw1160-id"));
                    }
                    ds4_count++;
                } else if (opened_here) {
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
        (args_off + sizeof(RemotePadReaderTransport) + 15) & ~(size_t)15;
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

    RemotePadReaderTransport transport;
    memset(&transport,0,sizeof transport);
    transport.socket_fd=-1;
    RemotePadReaderArgs args;
    memset(&args, 0, sizeof(args));
    args.fp_readstate = fn_readstate;
    args.fp_read = fn_read;
    args.fp_usleep = fn_usleep;
    args.pad_handle = handle;
    args.interval_us = 8333;
    args.last_result = -1;
    args.last_read_result = -1;
    args.fp_kill = fn_kill;
    args.fp_closepad = fn_closepad;
    args.owner_pid = owner_pid;
    args.close_pad_on_exit = selected_opened_here;
    args.owner_check_interval = 120;
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        args.slot_handles[s] = (s < ds4_count) ? multi_handles[s] : -1;
        if (s > 0 && s < ds4_count && multi_opened[s])
            transport.owned_secondary_pad_slots |= 1u << s;
        args.slot_seqs[s] = 0;
        args.slot_results[s] = -1;
        memset(args.slot_data[s], 0, sizeof(args.slot_data[s]));
    }
    /* Optional output setup uses the same one-time source bootstrap. Missing
     * APIs/channel setup leave input working and explicitly disable rumble. */
    intptr_t out_socket=libkernel_h?resolve_sym(target,libkernel_h,"socket"):0;
    intptr_t out_connect=libkernel_h?resolve_sym(target,libkernel_h,"connect"):0;
    intptr_t out_name=libkernel_h?resolve_sym(target,libkernel_h,"getsockname"):0;
    intptr_t out_recv=libkernel_h?resolve_sym(target,libkernel_h,"recvfrom"):0;
    intptr_t out_clock=libkernel_h?resolve_sym(target,libkernel_h,"clock_gettime"):0;
    output_close=libkernel_h?resolve_sym(target,libkernel_h,"close"):0;
    intptr_t out_vibrate=libpad_h?resolve_sym(target,libpad_h,"scePadSetVibration"):0;
    intptr_t out_mode=libpad_h?resolve_sym(target,libpad_h,"scePadSetVibrationMode"):0;
    int output_ready=0,output_stage=0,output_error=0;
    int64_t output_result=0;
    /* The old probe buffers have already been released. Use the allocated
     * reader argument area as bootstrap scratch, before publishing transport
     * and starting the thread. No scratch pointer outlives this setup. */
    intptr_t output_scratch=args_addr;
    if(g_reader_rumble_fd<0 && out_socket && out_connect && out_name && out_recv &&
       out_clock && output_close && out_vibrate && out_mode && output_scratch>0) {
        output_stage=1;
        output_local=socket(AF_INET,SOCK_DGRAM,0);
        output_address.sin_len=sizeof output_address;output_address.sin_family=AF_INET;
        output_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        socklen_t length=sizeof output_address;
        if(output_local<0){output_error=errno;goto output_setup_done;}
        output_stage=2;
        if(bind(output_local,(struct sockaddr*)&output_address,sizeof output_address)) {
            output_error=errno;goto output_setup_done;
        }
        output_stage=3;
        if(getsockname(output_local,(struct sockaddr*)&output_address,&length)) {
            output_error=errno;goto output_setup_done;
        }
        if(length!=sizeof output_address)goto output_setup_done;
        output_stage=4;
        output_result=pt_io_write(target,output_scratch,&output_address,sizeof output_address);
        if(output_result)goto output_setup_done;
        output_stage=5;
        output_result=pt_call(target,out_socket,trap_mem,AF_INET,SOCK_DGRAM,0,0,0,0);
        if(output_result<0 || output_result>INT_MAX)goto output_setup_done;
        output_remote=(int)output_result;
        output_stage=6;
        output_result=pt_call(target,out_connect,trap_mem,(uint32_t)output_remote,
            output_scratch,sizeof output_address,0,0,0);
        if(output_result)goto output_setup_done;
        output_stage=7;
        output_result=pt_io_write(target,output_scratch+64,&length,sizeof length);
        if(output_result)goto output_setup_done;
        output_stage=8;
        output_result=pt_call(target,out_name,trap_mem,(uint32_t)output_remote,output_scratch,
            output_scratch+64,0,0,0);
        if(output_result)goto output_setup_done;
        struct sockaddr_in peer;memset(&peer,0,sizeof peer);
        output_stage=9;
        output_result=mdbg_copyout(target,output_scratch,&peer,sizeof peer);
        if(output_result)goto output_setup_done;
        output_stage=10;
        if(peer.sin_len!=sizeof peer || peer.sin_family!=AF_INET || !peer.sin_port ||
           peer.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))goto output_setup_done;
        output_stage=11;
        if(connect(output_local,(struct sockaddr*)&peer,sizeof peer)) {
            output_error=errno;goto output_setup_done;
        }
        output_stage=12;output_ready=1;output_result=0;
    }
output_setup_done:
    klog_printf("[PoorDS4] reader rumble channel ready=%d symbols=%d stage=%d result=0x%llx errno=%d\n",output_ready,
        !!(out_socket && out_connect && out_name && out_recv && out_clock && output_close && out_vibrate && out_mode),
        output_stage,(unsigned long long)(uint64_t)output_result,output_error);
    if(output_ready) {
        PoorDS4ReaderOutput *output=&transport.output;
        output->magic=POORDS4_READER_OUTPUT_MAGIC;output->version=1;
        output->size=sizeof *output;output->slots=4;
        arc4random_buf(&output->session,sizeof output->session);
        if(!output->session)output->session=1;
        output->fp_vibration=out_vibrate;output->fp_mode=out_mode;
        output->fp_clock=out_clock;output->fp_is_ds4=fn_isds4;
        output->fp_device_info=fn_getdeviceinfo;
        output->accepted_vendor=selected_device_vendor;
        output->accepted_product=selected_device_product;
        for(unsigned slot=0;slot<4;slot++) {
            output->channels[slot].state.handle=-1;output->channels[slot].state.user=-1;
            if(slot<ds4_count) {
                output->channels[slot].state.handle=multi_handles[slot];
                output->channels[slot].state.user=g_reader_users[slot];
                output->channels[slot].state.reserved=(uint32_t)multi_device_vendor[slot] |
                    ((uint32_t)multi_device_product[slot]<<16);
            }
        }
        transport.socket_fd=output_remote;transport.fp_recvfrom=out_recv;
        transport.fp_socket_close=output_close;
    } else {
        if(output_remote>=0 && output_close)pt_call(target,output_close,trap_mem,(uint32_t)output_remote,0,0,0,0,0);
        output_remote=-1;
        if(output_local>=0)close(output_local);
        output_local=-1;
    }
    transport.input=args;
    transport.discovery.magic = POORDS4_DISCOVERY_MAGIC;
    transport.discovery.fp_gethandle = fn_gethandle;
    transport.discovery.fp_open = fn_open;
    transport.discovery.fp_getdeviceid = fn_getdeviceid;
    transport.discovery.fp_getdeviceinfo = fn_getdeviceinfo;
    transport.discovery.user_count = user_count;
    transport.discovery.admitted_count = ds4_count;
    transport.discovery.interval_frames = 16;
    transport.discovery.pending_user = -1;
    transport.discovery.pending_index = -1;
    transport.discovery.pending_device = -1;
    transport.discovery.probe_handle = -1;
    for (unsigned u = 0; u < user_count; ++u)
        transport.discovery.users[u] = user_ids[u];
    for (unsigned s = 0; s < ds4_count; ++s) {
        transport.discovery.admitted_users[s] = g_reader_users[s];
        transport.discovery.admitted_indices[s] = multi_indices[s];
    }
    klog_printf("[PoorDS4] reader handle ownership primary_opened=%d "
                "secondary_mask=0x%02x admitted=%u\n",
                args.close_pad_on_exit,
                transport.owned_secondary_pad_slots, ds4_count);
    if (pt_io_write(target, args_addr, &transport, sizeof transport) != 0)
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
    if(!launched) {
        if(output_remote>=0 && output_close && trap_mem)
            pt_call(target,output_close,trap_mem,(uint32_t)output_remote,0,0,0,0,0);
        if(output_local>=0)close(output_local);
    } else if(output_local>=0) {
        g_reader_rumble_fd=output_local;g_reader_rumble_pid=target;
        g_reader_rumble_args=args_addr;memset(g_reader_rumble_sent,0,sizeof g_reader_rumble_sent);
    }
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
    if (!launched && ds4_count > 1 && fn_closepad && trap_mem) {
        for (unsigned s = 1; s < ds4_count; ++s) {
            if (!multi_opened[s] || multi_handles[s] < 0 ||
                multi_handles[s] == handle)
                continue;
            int already_closed = 0;
            for (unsigned previous = 1; previous < s; ++previous)
                already_closed |= multi_opened[previous] &&
                    multi_handles[previous] == multi_handles[s];
            if (!already_closed)
                (void)pt_call(target, fn_closepad, trap_mem,
                              (uint32_t)multi_handles[s], 0, 0, 0, 0, 0);
        }
    }
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
            int32_t connected = 0;
            uint16_t vendor = 0;
            uint16_t product = 0;
            if (remote_pad_identity(
                    target, libpad_base, handle,
                    &connected, &vendor, &product) == 0) {
                identity_ok = remote_pad_is_known_ds4(connected, vendor, product);
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
            klog_printf(
                "[PoorDS4] reader ready pid=%d args=0x%lx "
                "mode=%u queued=0x%08x success=%u empty=%u "
                "errors=%u state_fallback=%u\n",
                target, args_addr, snapshot.reader_mode,
                (uint32_t)snapshot.last_read_result,
                snapshot.read_success_frames,
                snapshot.read_empty_frames,
                snapshot.read_error_frames,
                snapshot.state_fallback_frames);
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
        uint32_t seq_before = 0;
        uint32_t seq_after = 0;
        /* A bulk VM copy has no field-read ordering guarantee. Sample the
         * sequence before it so a new header cannot certify data copied
         * before a producer update. */
        if (remote_reader_copyout(
                pid,
                args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, seq),
                &seq_before, sizeof(seq_before)) != 0)
            return -1;
        if (seq_before == 0 || (seq_before & 1u) != 0)
            continue;
        if (remote_reader_copyout(pid, args_kaddr, &snapshot,
                                  sizeof(snapshot)) != 0)
            return -1;
        if (remote_reader_copyout(
                pid,
                args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, seq),
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        if (snapshot.seq == seq_before && seq_before == seq_after &&
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
wireless_ds4_remote_reader_read_slot(pid_t pid, intptr_t args_kaddr,
                                     unsigned slot,
                                     void *pad_data, uint32_t pad_data_len,
                                     uint32_t *out_seq)
{
#if !defined(__PROSPERO__)
    (void)pid; (void)args_kaddr; (void)slot; (void)pad_data;
    (void)pad_data_len; (void)out_seq;
    return -1;
#else
    if (!pad_data || !args_kaddr || slot >= POORDS4_MAX_SLOTS)
        return -1;
    if (slot == 0)
        return wireless_ds4_remote_reader_read(
            pid, args_kaddr, pad_data, pad_data_len, out_seq);

    if (pad_data_len > POORDS4_REMOTE_PAD_CAPACITY)
        pad_data_len = POORDS4_REMOTE_PAD_CAPACITY;

    for (unsigned attempt = 0; attempt < 3; attempt++) {
        RemotePadReaderArgs snapshot;
        uint32_t seq_before = 0;
        uint32_t seq_after = 0;
        /* Take the opening sequence outside the unordered bulk copy. */
        if (remote_reader_copyout(
                pid,
                args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, slot_seqs[slot]),
                &seq_before, sizeof(seq_before)) != 0)
            return -1;
        if (seq_before == 0 || (seq_before & 1u) != 0)
            continue;
        if (remote_reader_copyout(pid, args_kaddr, &snapshot,
                                  sizeof(snapshot)) != 0)
            return -1;
        if (remote_reader_copyout(
                pid,
                args_kaddr + (intptr_t)offsetof(RemotePadReaderArgs, slot_seqs[slot]),
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        uint32_t s_seq = snapshot.slot_seqs[slot];
        if (s_seq == seq_before && seq_before == seq_after &&
            snapshot.ready == 1 &&
            snapshot.slot_handles[slot] > 0 &&
            snapshot.slot_results[slot] == 0 && s_seq != 0) {
            memcpy(pad_data, snapshot.slot_data[slot], pad_data_len);
            if (out_seq) *out_seq = s_seq;
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
    out_status->last_read_result = snapshot.last_read_result;
    out_status->read_success_frames = snapshot.read_success_frames;
    out_status->read_empty_frames = snapshot.read_empty_frames;
    out_status->read_error_frames = snapshot.read_error_frames;
    out_status->state_fallback_frames = snapshot.state_fallback_frames;
    out_status->reader_mode = snapshot.reader_mode;
    {
        const ScePadData *pad =
            (const ScePadData *)(const void *)snapshot.pad_data;
        out_status->buttons = pad->buttons;
        out_status->left_x = pad->leftStick.x;
        out_status->left_y = pad->leftStick.y;
        out_status->right_x = pad->rightStick.x;
        out_status->right_y = pad->rightStick.y;
        out_status->left_trigger = pad->analogButtons.l2;
        out_status->right_trigger = pad->analogButtons.r2;
        out_status->count = pad->count;
        out_status->timestamp = pad->timestamp;
    }
    out_status->reset_combo_ticks = (uint32_t)snapshot.reset_combo_ticks;
    out_status->reset_requested = (uint32_t)snapshot.reset_requested;
    uint64_t discovery_magic = 0;
    intptr_t discovery_address = args_kaddr +
        (intptr_t)offsetof(RemotePadReaderTransport, discovery);
    if (remote_reader_copyout(pid, discovery_address, &discovery_magic,
                             sizeof discovery_magic) != 0)
        return -1;
    if (discovery_magic == POORDS4_DISCOVERY_MAGIC ||
        discovery_magic == POORDS4_DISCOVERY_MAGIC_V1) {
        PoorDS4SourceDiscovery discovery = {0};
        size_t discovery_size = discovery_magic == POORDS4_DISCOVERY_MAGIC_V1 ?
            offsetof(PoorDS4SourceDiscovery, probe_handle) : sizeof discovery;
        uint32_t refresh_before = 0;
        if (remote_reader_copyout(pid, discovery_address +
                (intptr_t)offsetof(PoorDS4SourceDiscovery, refresh_requested),
                &refresh_before, sizeof refresh_before) != 0 ||
            remote_reader_copyout(pid, discovery_address, &discovery,
                                 discovery_size) != 0)
            return -1;
        if (discovery.magic == discovery_magic) {
            out_status->discovery_available = 1;
            out_status->discovery_queries = discovery.queries;
            out_status->discovery_open_calls = discovery.open_calls;
            out_status->discovery_opened = discovery.opened;
            out_status->discovery_closed = discovery.closed;
            out_status->discovery_close_errors = discovery.close_errors;
            out_status->discovery_disabled = discovery.disabled;
            out_status->discovery_refresh_requested =
                refresh_before == 1 && discovery.refresh_requested == 1;
            out_status->discovery_user = discovery.refresh_user;
            out_status->discovery_index = discovery.refresh_index;
            out_status->discovery_last_lookup_result = discovery.last_lookup_result;
            out_status->discovery_last_open_result = discovery.last_open_result;
            if (discovery_magic == POORDS4_DISCOVERY_MAGIC) {
                out_status->discovery_probe_held = discovery.probe_held;
                out_status->discovery_probe_polls = discovery.probe_polls;
                out_status->discovery_probe_waits = discovery.probe_waits;
                out_status->discovery_probe_timeouts = discovery.probe_timeouts;
                out_status->discovery_last_device_result = discovery.last_device_result;
                out_status->discovery_last_info_result = discovery.last_info_result;
                out_status->discovery_last_state_result = discovery.last_state_result;
            }
        }
    }
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
                                  sizeof(ready)) == 0 && ready == 2) {
            RemotePadReaderArgs final_snapshot;
            memset(&final_snapshot, 0, sizeof(final_snapshot));
            if (remote_reader_copyout(
                    pid, args_kaddr, &final_snapshot,
                    sizeof(final_snapshot)) == 0)
                klog_printf(
                    "[PoorDS4] reader final mode=%u queued=0x%08x "
                    "success=%u empty=%u errors=%u state_fallback=%u\n",
                    final_snapshot.reader_mode,
                    (uint32_t)final_snapshot.last_read_result,
                    final_snapshot.read_success_frames,
                    final_snapshot.read_empty_frames,
                    final_snapshot.read_error_frames,
                    final_snapshot.state_fallback_frames);
            if(g_reader_rumble_pid==pid && g_reader_rumble_args==args_kaddr) {
                if(g_reader_rumble_fd>=0)close(g_reader_rumble_fd);
                g_reader_rumble_fd=-1;g_reader_rumble_pid=-1;
                g_reader_rumble_args=0;memset(g_reader_rumble_sent,0,sizeof g_reader_rumble_sent);
            }
            PoorDS4RemoteReaderStatus discovery_status;
            int discovery_result = wireless_ds4_remote_reader_status(
                pid, args_kaddr, &discovery_status);
            if (discovery_result == 0 && discovery_status.discovery_available)
                klog_printf("[PoorDS4] reader discovery final opened=%u closed=%u held=%u waits=%u timeouts=%u close_errors=%u\n",
                    discovery_status.discovery_opened, discovery_status.discovery_closed,
                    discovery_status.discovery_probe_held, discovery_status.discovery_probe_waits,
                    discovery_status.discovery_probe_timeouts, discovery_status.discovery_close_errors);
            if (discovery_result != 0 ||
                discovery_status.discovery_close_errors ||
                discovery_status.discovery_probe_held) {
                klog_printf("[PoorDS4] reader stopped with discovery close uncertainty; refusing replacement\n");
                return -1;
            }
            return 0;
        }
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

int
wireless_ds4_reader_rumble_status_slot(pid_t pid,intptr_t address,unsigned slot,PoorDS4RumbleStatus *out)
{
#if !defined(__PROSPERO__)
    (void)pid;(void)address;(void)slot;(void)out;return -1;
#else
    if(slot>=POORDS4_MAX_SLOTS || !out || g_reader_rumble_fd<0 || pid!=g_reader_rumble_pid ||
       address!=g_reader_rumble_args)return -3;
    RemotePadReaderArgs prefix;
    if(remote_reader_copyout(pid,address,&prefix,sizeof prefix))return -2;
    if(prefix.ready!=1 || prefix.stop || prefix.owner_pid!=getpid())return -1;
    intptr_t output_address=address+offsetof(RemotePadReaderTransport,output);
    PoorDS4ReaderOutput header;
    memset(&header,0,sizeof header);
    if(remote_reader_copyout(pid,output_address,&header,offsetof(PoorDS4ReaderOutput,channels)))return -2;
    if(!poords4_reader_output_valid(&header))return -1;
    intptr_t channel_address=output_address+offsetof(PoorDS4ReaderOutput,channels)+
        slot*sizeof(PoorDS4ReaderOutputSlot);
    int32_t handle=slot?prefix.slot_handles[slot]:prefix.pad_handle;
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint64_t before,after;
        PoorDS4ReaderOutputSlot channel;
        if(remote_reader_copyout(pid,channel_address,&before,sizeof before) || (before&1))continue;
        if(remote_reader_copyout(pid,channel_address,&channel,sizeof channel) ||
           remote_reader_copyout(pid,channel_address,&after,sizeof after))return -2;
        if(before!=after || channel.state.sequence!=before)continue;
        if(handle<0 || channel.state.handle!=handle || channel.state.user<0)return -1;
        *out=(PoorDS4RumbleStatus){.session=header.session,.epoch=channel.state.epoch,
            .published=channel.published,.observed=channel.state.observed_sequence,
            .applied=channel.state.applied_sequence,.now=channel.state.now,
            .handle=channel.state.handle,.user=channel.state.user,
            .mode_result=channel.state.mode_result,.command_result=channel.state.command_result,
            .stop_result=channel.state.stop_result,.connected=channel.state.connected,
            .identity_result=channel.state.identity_result,.device_info_result=channel.state.device_info_result,
            .device_vendor=channel.state.device_vendor,.device_product=channel.state.device_product,
            .identity_path=channel.state.identity_path,
            .effect_active=channel.state.effect_active,.quarantined=channel.state.quarantined,
            .rejection=channel.state.rejection};
        return 0;
    }
    return -2;
#endif
}

int
wireless_ds4_reader_rumble_status(pid_t pid,intptr_t address,PoorDS4RumbleStatus *out)
{
    return wireless_ds4_reader_rumble_status_slot(pid,address,0,out);
}

int
wireless_ds4_reader_rumble_submit_slot(pid_t pid,intptr_t address,unsigned slot,uint8_t large,
                                  uint8_t small,const PoorDS4RumbleStatus *binding,
                                  uint64_t *sequence)
{
#if !defined(__PROSPERO__)
    (void)pid;(void)address;(void)slot;(void)large;(void)small;(void)binding;(void)sequence;return -1;
#else
    PoorDS4RumbleStatus state;
    if(!sequence || !binding)return -1;
    int snapshot_result=wireless_ds4_reader_rumble_status_slot(pid,address,slot,&state);
    if(snapshot_result)return snapshot_result==-2?-2:-1;
    if(state.quarantined || state.published==UINT64_MAX ||
       (g_reader_rumble_sent[slot] && state.observed<g_reader_rumble_sent[slot]))return -1;
    /* No datagram/sequence mutation on binding change. The caller may suspend
     * and rebind; an attempted send remains a different, uncertain failure. */
    if(!state.connected || !state.epoch || state.session!=binding->session ||
       state.epoch!=binding->epoch || state.handle!=binding->handle ||
       state.user!=binding->user)return -4;
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec<0 || now.tv_nsec<0 ||
       now.tv_nsec>=1000000000 || (uint64_t)now.tv_sec>(UINT64_MAX-POORDS4_OUTPUT_TIMEOUT_NS-
           (uint64_t)now.tv_nsec)/UINT64_C(1000000000))return -1;
    uint64_t current=(uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
    PoorDS4OutputCommand command={.session=state.session,.epoch=state.epoch,
        .sequence=state.published+1,.deadline=current+POORDS4_OUTPUT_TIMEOUT_NS,
        .handle=state.handle,.user=state.user,.vibration={large,small}};
    /* Record uncertainty before send; never resubmit an unacknowledged command. */
    g_reader_rumble_sent[slot]=command.sequence;
    ssize_t sent=send(g_reader_rumble_fd,&command,sizeof command,MSG_DONTWAIT);
    if(sent!=(ssize_t)sizeof command)return -1;
    *sequence=command.sequence;
    return 0;
#endif
}

int
wireless_ds4_reader_rumble_submit(pid_t pid,intptr_t address,uint8_t large,
    uint8_t small,const PoorDS4RumbleStatus *binding,uint64_t *sequence)
{
    return wireless_ds4_reader_rumble_submit_slot(pid,address,0,large,small,binding,sequence);
}

#define POORDS4_READ_STATE_OFFSET_0602 UINT32_C(0x000028a0)
#define POORDS4_READ_STATE_FNV256_0602 UINT64_C(0x87273539fe448cfc)
#define POORDS4_READ_OFFSET_0602 UINT32_C(0x000028b0)
#define POORDS4_READ_FNV256_0602 UINT64_C(0x12240dad3fb2a508)
#define POORDS4_READ_STATE_EXT_OFFSET_0602 UINT32_C(0x000028c0)
#define POORDS4_READ_STATE_EXT_FNV256_0602 UINT64_C(0x268b8b5413078c59)
#define POORDS4_READ_EXT_OFFSET_0602 UINT32_C(0x000028d0)
#define POORDS4_READ_EXT_FNV256_0602 UINT64_C(0xcaf81821813e484f)
#define POORDS4_DATA_INTERNAL_OFFSET_0602 UINT32_C(0x00000de0)
#define POORDS4_DATA_INTERNAL_FNV256_0602 UINT64_C(0x41474651c5a524d6)
#define POORDS4_CONTROLLER_INFO_OFFSET_0602 UINT32_C(0x00004330)
#define POORDS4_CONTROLLER_INFO_FNV256_0602 UINT64_C(0x33fa61cc0bb04d2a)

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

#define POORDS4_READ_STATE_OFFSET_1001 UINT32_C(0x00002960)
#define POORDS4_READ_STATE_FNV256_1001 UINT64_C(0x71da09841b4cd6af)
#define POORDS4_READ_OFFSET_1001 UINT32_C(0x00002970)
#define POORDS4_READ_FNV256_1001 UINT64_C(0x318bfe94e1803d6e)
#define POORDS4_READ_STATE_EXT_OFFSET_1001 UINT32_C(0x00002980)
#define POORDS4_READ_STATE_EXT_FNV256_1001 UINT64_C(0xa2a0ba833c1c4734)
#define POORDS4_READ_EXT_OFFSET_1001 UINT32_C(0x00002990)
#define POORDS4_READ_EXT_FNV256_1001 UINT64_C(0x90d246033d34f9b9)
#define POORDS4_DATA_INTERNAL_OFFSET_1001 UINT32_C(0x00000df0)
#define POORDS4_DATA_INTERNAL_FNV256_1001 UINT64_C(0x18a406c869932aae)
#define POORDS4_CONTROLLER_INFO_OFFSET_1001 UINT32_C(0x00004820)
#define POORDS4_CONTROLLER_INFO_FNV256_1001 UINT64_C(0xc56da126f8d9c1f3)

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
    POORDS4_MANIFEST_0602 = 602,
    POORDS4_MANIFEST_0860 = 860,
    POORDS4_MANIFEST_1001 = 1001,
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
game_bridge_target_protection(pid_t pid, intptr_t target)
{
    for (unsigned attempt = 0; attempt < 3u; ++attempt) {
        int protection = kernel_get_vmem_protection(pid, target, 1);
        if (protection >= 0)
            return protection;
    }
    return -1;
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
    char line[256];
    int prefix_length = snprintf(line, sizeof(line), "%s=", name);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(line))
        return;
    static const char digits[] = "0123456789abcdef";
    size_t cursor = (size_t)prefix_length;
    for (size_t index = 0;
         index < count && cursor + 2u < sizeof(line); ++index) {
        line[cursor++] = digits[code[index] >> 4u];
        line[cursor++] = digits[code[index] & 0x0fu];
    }
    line[cursor] = '\0';
    report_printf(fd, "%s\n", line);
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
    uint64_t rela;
    uint64_t rela_size;
} GameBridgeDynlibSectionPrefix;
_Static_assert(offsetof(GameBridgeDynlibSectionPrefix, rela) == 0x58,
               "SDK ordinary relocation pointer");
_Static_assert(offsetof(GameBridgeDynlibSectionPrefix, rela_size) == 0x60,
               "SDK ordinary relocation size");


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
    pid_t target, intptr_t libpad_base, const intptr_t originals[POORDS4_GAME_BRIDGE_API_COUNT],
    GameBridgeImportHook *hooks, uint32_t *out_count, int report_fd,
    int allow_nonoriginal, int include_modules);
static int game_cache_find_libpad_object(
    pid_t target, intptr_t base,
    GameBridgeDynlibObjectPrefix *out_object);
static int game_cache_find_table(
    pid_t target, intptr_t base, uint64_t mapsize,
    intptr_t read_state, int32_t source_user_id,
    int32_t source_pad_index, int report_fd,
    intptr_t *out_table, uint32_t *out_stride);

static int
game_bridge_pointer_in_loaded_object(pid_t target, uint64_t pointer)
{
#if !defined(__PROSPERO__)
    (void)target; (void)pointer;
    return -1;
#else
    if (target <= 0 || pointer == 0)
        return -1;
    uint64_t proc = (uint64_t)kernel_get_proc(target);
    uint64_t shared_object = 0;
    uint64_t object_address = 0;
    if (proc == 0 || kernel_copyout(
            proc + 0x3e8, &shared_object, sizeof(shared_object)) != 0 ||
        shared_object == 0 || kernel_copyout(
            shared_object, &object_address, sizeof(object_address)) != 0)
        return -1;
    for (unsigned count = 0;
         object_address != 0 && count < 512u; ++count) {
        GameBridgeDynlibObjectPrefix object;
        memset(&object, 0, sizeof(object));
        if (kernel_copyout(
                (intptr_t)object_address, &object,
                sizeof(object)) != 0)
            return -1;
        if (object.mapbase != 0 && object.mapsize != 0 &&
            object.mapsize <= UINT64_MAX - object.mapbase &&
            pointer >= object.mapbase &&
            pointer < object.mapbase + object.mapsize)
            return 1;
        object_address = object.next;
    }
    return 0;
#endif
}

static void
game_bridge_report_libpad_layout(pid_t target, intptr_t base, int report_fd)
{
#if defined(__PROSPERO__)
    GameBridgeDynlibObjectPrefix object;
    memset(&object, 0, sizeof(object));
    if (game_cache_find_libpad_object(target, base, &object) != 0) {
        report_printf(report_fd, "libpad_layout_error=object\n");
        return;
    }
    report_printf(
        report_fd,
        "libpad_layout mapbase=0x%llx mapsize=0x%llx sections=%llu "
        "refcount=%u\n",
        (unsigned long long)object.mapbase,
        (unsigned long long)object.mapsize,
        (unsigned long long)object.section_count, object.refcount);
    uint64_t section_count = object.section_count;
    if (!object.sections || section_count > 32u) {
        report_printf(report_fd, "libpad_layout_error=section_bounds\n");
        return;
    }
    for (uint64_t index = 0; index < section_count; ++index) {
        GameBridgeSharedLibSection section;
        memset(&section, 0, sizeof(section));
        intptr_t section_address = (intptr_t)(object.sections +
            index * sizeof(section));
        if (kernel_copyout(
                section_address, &section, sizeof(section)) != 0) {
            report_printf(
                report_fd,
                "libpad_section_error index=%llu address=0x%lx\n",
                (unsigned long long)index,
                (unsigned long)section_address);
            continue;
        }
        int protection = section.address && section.length
            ? game_bridge_target_protection(
                target, (intptr_t)section.address) : -1;
        report_printf(
            report_fd,
            "libpad_section index=%llu descriptor=0x%llx "
            "address=0x%llx length=0x%llx protection=0x%x\n",
            (unsigned long long)index,
            (unsigned long long)section.descriptor,
            (unsigned long long)section.address,
            (unsigned long long)section.length, protection);
    }
#else
    (void)target; (void)base; (void)report_fd;
#endif
}

/* Read the same bounded rtld metadata prefixes used by the SDK's
 * kernel_dynlib helpers. Every followed size, index, pointer, and slot is
 * validated before a caller-owned import is changed. */
static int
game_bridge_metadata_span_valid(uint64_t address,uint64_t length)
{
    /* rtld tables/string buffers may be kernel-private. Game import slots are
     * still validated separately as target user addresses. Keep the complete
     * span in one canonical 48-bit address region without unsigned wrap. */
    if (!length || address < UINT64_C(0x10000))
        return 0;
    if (address < UINT64_C(0x0000800000000000))
        return length <= UINT64_C(0x0000800000000000) - address;
    return address >= UINT64_C(0xffff800000000000) &&
        length <= UINT64_MAX - address;
}

static int
game_bridge_read_import_name(pid_t target,intptr_t address,uint64_t remaining,char name[64])
{
    if(!name)return -1;
    memset(name,0,64);
    size_t length=remaining<63u?(size_t)remaining:63u;
    if(!game_bridge_metadata_span_valid((uint64_t)address,length))return -1;
    int read=kernel_copyout(address,name,length);
    if(read!=0) {
        /* Preserve the existing bounded metadata transport while reading the
         * identity delimiter too. This preserves the baseline transport; it is not kernel fault containment. */
        read=0;
        for(size_t byte=0;byte<length;byte++) {
            if(kernel_copyout((intptr_t)((uint64_t)address+byte),name+byte,1u)!=0) {
                read=-1;break;
            }
        }
    }
    if(read!=0)read=game_bridge_process_read(target,address,name,length);
    if(read!=0)return -1;
    /* A short string-table tail must contain its real terminator. Never invent
     * one immediately after an eleven-byte NID. A full window may truncate
     * qualifiers, but every supported identity and its boundary fit inside it. */
    if(length<63u && !memchr(name,0,length))return -1;
    return 0;
}

static int
game_bridge_symbol_kind(const char *name, const char nids[POORDS4_GAME_BRIDGE_API_COUNT][12], const char *const names[POORDS4_GAME_BRIDGE_API_COUNT])
{
    if (!name)
        return -1;
    size_t length=strlen(name);
    if (nids) {
        for (unsigned kind = 0; kind < POORDS4_GAME_BRIDGE_API_COUNT; ++kind) {
            if (length>=11 && (length==11 || name[11]=='#') &&
                memcmp(name, nids[kind], 11) == 0)
                return (int)kind;
        }
    }
    if (names) {
        for (unsigned kind = 0; kind < POORDS4_GAME_BRIDGE_API_COUNT; ++kind) {
            size_t api_length=strlen(names[kind]);
            if (length>=api_length && (length==api_length || name[api_length]=='#') &&
                memcmp(name, names[kind], api_length) == 0)
                return (int)kind;
        }
    }
    return -1;
}

static void
game_bridge_dump_modules_report(
    pid_t target, intptr_t libpad_base,
    const GameBridgeImportHook *hooks, uint32_t hook_count)
{
#if defined(__PROSPERO__)
    if (target <= 0)
        return;
    (void)mkdir(POORDS4_DATA_DIR, 0755);
    (void)mkdir(POORDS4_DATA_DIR "/reports", 0755);
    uint32_t fw = kernel_get_fw_version();
    char mod_archive[160];
    snprintf(mod_archive, sizeof(mod_archive),
             POORDS4_DATA_DIR "/reports/modules-fw-%08x-pid-%d.txt",
             fw, target);
    FILE *f1 = fopen(mod_archive, "w");
    FILE *f2 = fopen(POORDS4_DATA_DIR "/modules-last.txt", "w");
    if (!f1 && !f2)
        return;

#define MOD_PRINT(...) do { \
    if (f1) fprintf(f1, __VA_ARGS__); \
    if (f2) fprintf(f2, __VA_ARGS__); \
} while (0)

    MOD_PRINT("================================================================================\n");
    MOD_PRINT("PoorDS4 Game Bridge Loaded Modules Report\n");
    MOD_PRINT("================================================================================\n");
    MOD_PRINT("PID:         %d\n", target);
    MOD_PRINT("Firmware:    0x%08x\n", fw);
    MOD_PRINT("libScePad:   0x%lx\n", (unsigned long)libpad_base);
    MOD_PRINT("--------------------------------------------------------------------------------\n");
    MOD_PRINT("%-4s %-8s %-18s %-12s %-8s %s\n", "Idx", "Handle", "MapBase", "MapSize", "Scope", "Path");
    MOD_PRINT("--------------------------------------------------------------------------------\n");

    uint64_t proc = (uint64_t)kernel_get_proc(target);
    uint64_t shared_object = 0;
    uint64_t object_address = 0;
    if (proc && kernel_copyout(proc + 0x3e8, &shared_object, sizeof(shared_object)) == 0 &&
        shared_object && kernel_copyout(shared_object, &object_address, sizeof(object_address)) == 0) {
        unsigned object_count = 0;
        while (object_address != 0 && object_count++ < 512u) {
            GameBridgeDynlibObjectPrefix object;
            memset(&object, 0, sizeof(object));
            if (kernel_copyout((intptr_t)object_address, &object, sizeof(object)) != 0)
                break;
            object_address = object.next;
            char object_path[192];
            memset(object_path, 0, sizeof(object_path));
            if (object.path)
                (void)kernel_copyout((intptr_t)object.path, object_path, sizeof(object_path) - 1u);
            int is_fakelib = (object_path[0] != '\0' && strstr(object_path, "fakelib") != NULL);
            int is_system_module = (object_path[0] != '\0' && (
                strstr(object_path, "/system/") != NULL ||
                strstr(object_path, "/system_ex/") != NULL ||
                strstr(object_path, "/common/") != NULL));
            int is_app_module = (object.handle == 0) ||
                (!is_fakelib && !is_system_module && object_path[0] != '\0' && (
                    strncmp(object_path, "/app0/", 6) == 0 ||
                    strncmp(object_path, "/app0_patch/", 12) == 0 ||
                    strstr(object_path, "app0/") != NULL ||
                    strstr(object_path, "shadowmnt/") != NULL));
            const char *scope = is_app_module ? "APP" : (is_fakelib ? "FAKELIB" : (is_system_module ? "SYSTEM" : "OTHER"));
            MOD_PRINT("%-4u 0x%-6llx 0x%-16llx 0x%-10llx %-8s %s\n",
                      object_count - 1u, (unsigned long long)object.handle,
                      (unsigned long long)object.mapbase,
                      (unsigned long long)object.mapsize,
                      scope, object_path);
        }
    }

    if (hooks && hook_count > 0) {
        static const char *const names[POORDS4_GAME_BRIDGE_API_COUNT] = {
            "scePadReadState", "scePadReadStateExt", "scePadRead",
            "scePadReadExt", "scePadGetDataInternal",
            "scePadGetControllerInformation",
            "scePadGetExtControllerInformation", "scePadGetTriggerEffectState", "scePadSetVibration", "scePadSetVibrationMode", "scePadSetTriggerEffect", "scePadDeviceClassGetExtendedInformation"};
        MOD_PRINT("\n--------------------------------------------------------------------------------\n");
        MOD_PRINT("Hooked Imports (%u hooks found):\n", hook_count);
        MOD_PRINT("--------------------------------------------------------------------------------\n");
        MOD_PRINT("%-4s %-28s %-18s %-18s %-6s\n", "Idx", "API", "Slot", "Original", "Prot");
        MOD_PRINT("--------------------------------------------------------------------------------\n");
        for (uint32_t hi = 0; hi < hook_count; ++hi) {
            const char *api_name = hooks[hi].kind < POORDS4_GAME_BRIDGE_API_COUNT ? names[hooks[hi].kind] : "unknown";
            MOD_PRINT("%-4u %-28s 0x%-16lx 0x%-16lx 0x%-4x\n",
                      hi, api_name,
                      (unsigned long)hooks[hi].slot,
                      (unsigned long)hooks[hi].original,
                      hooks[hi].protection);
        }
    }
    MOD_PRINT("================================================================================\n");

#undef MOD_PRINT
    if (f1) fclose(f1);
    if (f2) fclose(f2);
#else
    (void)target; (void)libpad_base; (void)hooks; (void)hook_count;
#endif
}

static int
game_bridge_collect_import_hooks(
    pid_t target, intptr_t libpad_base, const intptr_t *originals,
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
    char nids[POORDS4_GAME_BRIDGE_API_COUNT][12];
    static const char *const names[POORDS4_GAME_BRIDGE_API_COUNT] = {
        "scePadReadState", "scePadReadStateExt", "scePadRead",
        "scePadReadExt", "scePadGetDataInternal",
        "scePadGetControllerInformation",
        "scePadGetExtControllerInformation", "scePadGetTriggerEffectState", "scePadSetVibration", "scePadSetVibrationMode", "scePadSetTriggerEffect", "scePadDeviceClassGetExtendedInformation"};
    for (unsigned kind = 0; kind < POORDS4_GAME_BRIDGE_API_COUNT; ++kind) {
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
    unsigned skipped_layout = 0, skipped_scope = 0, skipped_symbols = 0;
    unsigned skipped_tables = 0, eligible_objects = 0;
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
            object.mapbase == (uint64_t)libpad_base) {
            skipped_layout++;
            continue;
        }
        char object_path[192];
        memset(object_path, 0, sizeof(object_path));
        if (object.path)
            (void)kernel_copyout(
                (intptr_t)object.path, object_path,
                sizeof(object_path) - 1u);
        /* Only the eboot and game-local /app0/ modules own the lifecycle we track.
         * Patching imports in a backported/fakelib SPRX would couple PoorDS4 to ShadowMount's
         * union overlay and can leave a library hook alive past game cleanup. */
        int is_fakelib = (object_path[0] != '\0' && strstr(object_path, "fakelib") != NULL);
        int is_system_module = (object_path[0] != '\0' && (
            strstr(object_path, "/system/") != NULL ||
            strstr(object_path, "/system_ex/") != NULL ||
            strstr(object_path, "/common/") != NULL));
        int is_app_module = (object.handle == 0) ||
            (!is_fakelib && !is_system_module && object_path[0] != '\0' && (
                strncmp(object_path, "/app0/", 6) == 0 ||
                strncmp(object_path, "/app0_patch/", 12) == 0 ||
                strstr(object_path, "app0/") != NULL ||
                strstr(object_path, "shadowmnt/") != NULL));
        if (!include_modules && !is_app_module) {
            skipped_scope++;
            continue;
        }
        eligible_objects++;
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
        report_printf(report_fd,
            "import_metadata object=%u handle=0x%llx path=%s "
            "symtab=0x%llx symbytes=0x%llx strtab=0x%llx strbytes=0x%llx "
            "plt=0x%llx pltbytes=0x%llx rela=0x%llx relabytes=0x%llx\n",
            object_count - 1u, (unsigned long long)object.handle, object_path,
            (unsigned long long)section.symtab, (unsigned long long)section.symtab_size,
            (unsigned long long)section.strtab, (unsigned long long)section.strtab_size,
            (unsigned long long)section.plt_rela, (unsigned long long)section.plt_rela_size,
            (unsigned long long)section.rela, (unsigned long long)section.rela_size);
        if (section.symtab_size < sizeof(Elf64_Sym) || section.strtab_size == 0 ||
            section.symtab_size > UINT64_C(0x02000000) ||
            section.strtab_size > UINT64_C(0x02000000) ||
            !game_bridge_metadata_span_valid(section.symtab,section.symtab_size) ||
            !game_bridge_metadata_span_valid(section.strtab,section.strtab_size)) {
            skipped_symbols++;
            continue;
        }
        const uint64_t tables[2]={section.plt_rela,section.rela};
        uint64_t sizes[2]={section.plt_rela_size,section.rela_size};
        /* An optional table must not suppress valid imports in the other.
         * Validate independently; keep mixed-table apps when both are valid. */
        for(unsigned table=0;table<2;table++) {
            if(sizes[table] && (sizes[table]>UINT64_C(0x02000000) ||
                sizes[table]%sizeof(Elf64_Rela) ||
                !game_bridge_metadata_span_valid(tables[table],sizes[table]))) {
                report_printf(report_fd,
                    "import_table_skipped object=%u table=%s reason=layout\n",
                    object_count-1u, table==0 ? "plt" : "ordinary");
                sizes[table]=0;
                skipped_tables++;
            }
        }
        if(!sizes[0] && !sizes[1]) {
            continue;
        }
        /* dynsec describes a byte extent, not an ELF section's sh_size.
         * The SDK resolver consumes only its complete symbols. Trailing bytes
         * never authorize another entry and are never read as a symbol. */
        uint64_t symbol_count = section.symtab_size / sizeof(Elf64_Sym);
        for(unsigned table=0;table<2;table++) {
        uint64_t rela_count = sizes[table] / sizeof(Elf64_Rela);
        relocation_count = rela_count>UINT_MAX-relocation_count
            ? UINT_MAX : relocation_count+(unsigned)rela_count;
        Elf64_Rela window[64];
        for (uint64_t index = 0; index < rela_count; ++index) {
            if(index%64u==0) {
                uint64_t count=rela_count-index;
                if(count>64u)count=64u;
                size_t bytes=(size_t)count*sizeof(Elf64_Rela);
                intptr_t address=(intptr_t)(tables[table]+index*sizeof(Elf64_Rela));
                if(kernel_copyout(address,window,bytes)!=0 &&
                    game_bridge_process_read(target,address,window,bytes)!=0) {
                report_printf(
                    report_fd,
                    "import_scan_error=rela object=%u table=%u index=%llu\n",
                    object_count - 1u,table,(unsigned long long)index);
                return -1;
                }
            }
            Elf64_Rela rela=window[index%64u];
            /* Only loader-established callable import slots: never reinterpret
             * RELATIVE/TLS/absolute/data relocations as pad function pointers. */
            unsigned type=ELF64_R_TYPE(rela.r_info);
            if(type!=(table==0?R_X86_64_JMP_SLOT:R_X86_64_GLOB_DAT))continue;
            uint64_t symbol_index = ELF64_R_SYM(rela.r_info);
            if (symbol_index >= symbol_count)
                continue;
            Elf64_Sym symbol;
            intptr_t symbol_address = (intptr_t)(section.symtab +
                symbol_index * sizeof(symbol));
            if (kernel_copyout(
                    symbol_address, &symbol, sizeof(symbol)) != 0 &&
                game_bridge_process_read(
                    target, symbol_address,
                    &symbol, sizeof(symbol)) != 0) {
                report_printf(
                    report_fd,
                    "import_scan_error=symbol object=%u index=%llu\n",
                    object_count - 1u,
                    (unsigned long long)symbol_index);
                return -1;
            }
            if (symbol.st_shndx!=SHN_UNDEF ||
                (ELF64_ST_TYPE(symbol.st_info)!=STT_FUNC &&
                 ELF64_ST_TYPE(symbol.st_info)!=STT_NOTYPE))continue;
            /* The complete string-table span was validated above in its
             * metadata address domain. An in-range offset cannot wrap it;
             * a signed user-address ceiling would reject kernel-private names. */
            if ((uint64_t)symbol.st_name >= section.strtab_size)
                continue;
            char symbol_name[64];
            intptr_t name_address = (intptr_t)(
                section.strtab + symbol.st_name);
            int name_read=game_bridge_read_import_name(target,name_address,
                section.strtab_size-symbol.st_name,symbol_name);
            int kind = name_read == 0
                ? game_bridge_symbol_kind(symbol_name, nids, names) : -1;
            if (name_read == 0 && kind < 0 &&
                rela.r_offset <= object.mapsize - sizeof(uint64_t)) {
                intptr_t check_slot = (intptr_t)(
                    object.mapbase + rela.r_offset);
                uint64_t check_val = 0;
                if (check_slot > 0 && remote_reader_copyout(
                        target, check_slot, &check_val, sizeof(check_val)) == 0) {
                    for (unsigned candidate_kind = 0;
                         candidate_kind < POORDS4_GAME_BRIDGE_API_COUNT; ++candidate_kind) {
                        if (originals[candidate_kind]>0 && check_val == (uint64_t)originals[candidate_kind]) {
                            kind = (int)candidate_kind;
                            break;
                        }
                    }
                }
            }
            if (name_read != 0) {
                if (rela.r_offset >
                        object.mapsize - sizeof(uint64_t))
                    continue;
                intptr_t fallback_slot = (intptr_t)(
                    object.mapbase + rela.r_offset);
                uint64_t fallback_current = 0;
                if (fallback_slot <= 0 || remote_reader_copyout(
                        target, fallback_slot, &fallback_current,
                        sizeof(fallback_current)) != 0) {
                    report_printf(
                        report_fd,
                        "import_scan_error=name_slot object=%u "
                        "index=%llu slot=0x%lx\n",
                        object_count - 1u,
                        (unsigned long long)symbol_index,
                        (unsigned long)fallback_slot);
                    return -1;
                }
                for (unsigned candidate_kind = 0;
                     candidate_kind < POORDS4_GAME_BRIDGE_API_COUNT; ++candidate_kind) {
                    if (originals[candidate_kind]>0 && fallback_current ==
                            (uint64_t)originals[candidate_kind]) {
                        kind = (int)candidate_kind;
                        break;
                    }
                }
                if (kind >= 0) {
                    report_printf(
                        report_fd,
                        "import_name_fallback object=%u index=%llu "
                        "slot=0x%lx kind=%d current=0x%llx\n",
                        object_count - 1u,
                        (unsigned long long)symbol_index,
                        (unsigned long)fallback_slot, kind,
                        (unsigned long long)fallback_current);
                } else if (game_bridge_pointer_in_loaded_object(
                               target, fallback_current) == 1) {
                    report_printf(
                        report_fd,
                        "import_name_skipped object=%u index=%llu "
                        "slot=0x%lx current=0x%llx "
                        "reason=known-module-target\n",
                        object_count - 1u,
                        (unsigned long long)symbol_index,
                        (unsigned long)fallback_slot,
                        (unsigned long long)fallback_current);
                    continue;
                } else {
                    report_printf(
                        report_fd,
                        "import_scan_error=name_unproven object=%u "
                        "index=%llu slot=0x%lx current=0x%llx\n",
                        object_count - 1u,
                        (unsigned long long)symbol_index,
                        (unsigned long)fallback_slot,
                        (unsigned long long)fallback_current);
                    return -1;
                }
            }
            if (kind < 0 || originals[kind] <= 0 ||
                rela.r_offset > object.mapsize - sizeof(uint64_t))
                continue;
            intptr_t slot = (intptr_t)(object.mapbase + rela.r_offset);
            uint64_t current = 0;
            if (slot <= 0 || remote_reader_copyout(
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
    }
    report_printf(
        report_fd,
        "import_scan_objects=%u relocations=%u hooks=%u\n",
        object_count, relocation_count, *out_count);
    report_printf(report_fd,
        "import_scan_skips layout=%u scope=%u symbols=%u tables=%u eligible=%u\n",
        skipped_layout, skipped_scope, skipped_symbols, skipped_tables, eligible_objects);
    if (*out_count == 0) {
        game_bridge_dump_modules_report(target, libpad_base, NULL, 0);
        report_printf(report_fd, "import_scan_error=no_supported_hooks\n");
        return -1;
    }
    game_bridge_dump_modules_report(target, libpad_base, hooks, *out_count);
    return 0;
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
    pid_t target, intptr_t libpad_base, const intptr_t originals[POORDS4_GAME_BRIDGE_API_COUNT],
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
            report_fd, 1, 0) != 0) {
        /* Hook discovery is also the install-time hook inventory. Never
         * continue with a partial list after an unreadable relocation or
         * other scan failure. */
        *out_hook_count = 0;
        report_printf(report_fd, "stale_recovery_error=import_scan\n");
        return -2;
    }
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
                "stale_recovery_warning=orphaned_import index=%u "
                "slot=0x%lx current=0x%lx restoring_original=0x%lx\n",
                index, (unsigned long)hooks[index].slot,
                (unsigned long)current, (unsigned long)hooks[index].original);
            (void)game_bridge_process_write(
                target, hooks[index].slot, &hooks[index].original,
                sizeof(hooks[index].original));
            continue;
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
            "stale_recovery_warning=args_validation args=0x%lx "
            "unexpected=%u hooks=%u saved=%u; restoring all hooks\n",
            (unsigned long)args_address, unexpected, hook_count,
            args.import_hook_count);
        for (uint32_t i = 0; i < hook_count; ++i) {
            intptr_t cur = 0;
            if (game_bridge_process_read(target, hooks[i].slot, &cur, sizeof(cur)) == 0 &&
                cur != hooks[i].original) {
                (void)game_bridge_process_write(
                    target, hooks[i].slot, &hooks[i].original, sizeof(intptr_t));
            }
        }
        return 1;
    }

    unsigned saved_unexpected = 0;
    for (uint32_t index = 0; index < args.import_hook_count; ++index) {
        uint32_t kind = args.import_hook_kinds[index];
        intptr_t slot = args.import_hook_slots[index];
        intptr_t original = args.import_hook_originals[index];
        intptr_t gateway_address = args.import_hook_gateways[index];
        if (kind >= POORDS4_GAME_BRIDGE_API_COUNT || slot <= 0 || original != originals[kind] ||
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

static void
game_bridge_dump_client_table_report(
    pid_t target, intptr_t table, uint32_t table_stride,
    int32_t source_user_id, int32_t source_pad_index,
    const char *method, int32_t selected_handle, int32_t selected_index,
    unsigned active_count, unsigned ds4_count, unsigned inactive_count,
    unsigned identity_count)
{
#if defined(__PROSPERO__)
    if (target <= 0 || table <= 0)
        return;
    (void)mkdir(POORDS4_DATA_DIR, 0755);
    (void)mkdir(POORDS4_DATA_DIR "/reports", 0755);
    uint32_t fw = kernel_get_fw_version();
    char tbl_archive[160];
    snprintf(tbl_archive, sizeof(tbl_archive),
             POORDS4_DATA_DIR "/reports/client-table-fw-%08x-pid-%d.txt",
             fw, target);
    FILE *f1 = fopen(tbl_archive, "w");
    FILE *f2 = fopen(POORDS4_DATA_DIR "/client-table-last.txt", "w");
    if (!f1 && !f2)
        return;

#define TBL_PRINT(...) do { \
    if (f1) fprintf(f1, __VA_ARGS__); \
    if (f2) fprintf(f2, __VA_ARGS__); \
} while (0)

    TBL_PRINT("================================================================================\n");
    TBL_PRINT("PoorDS4 Game Bridge Internal Pad Client Table Report\n");
    TBL_PRINT("================================================================================\n");
    TBL_PRINT("PID:              %d\n", target);
    TBL_PRINT("Firmware:         0x%08x\n", fw);
    TBL_PRINT("Source User ID:   0x%08x\n", (uint32_t)source_user_id);
    TBL_PRINT("Source Pad Index: %d\n", source_pad_index);
    TBL_PRINT("Table Address:    0x%lx\n", (unsigned long)table);
    TBL_PRINT("Table Stride:     0x%x\n", table_stride);
    TBL_PRINT("--------------------------------------------------------------------------------\n");
    TBL_PRINT("%-4s %-10s %-10s %-5s %-5s %-5s %-11s %-12s %s\n",
              "Slot", "Handle", "UserID", "Idx", "Conn", "Val", "VID:PID", "Type", "Notes");
    TBL_PRINT("--------------------------------------------------------------------------------\n");

    for (unsigned slot = 0; slot < 24u; ++slot) {
        intptr_t entry = table + (intptr_t)slot * table_stride;
        int32_t connected = 0;
        int32_t handle = 0;
        int32_t user_id = -1;
        int32_t valid = 0;
        uint16_t vendor = 0;
        uint16_t product = 0;
        if (game_bridge_process_read(target, entry + POORDS4_PAD_CLIENT_CONNECTED_1160, &connected, sizeof(connected)) != 0 ||
            game_bridge_process_read(target, entry + POORDS4_PAD_CLIENT_HANDLE_1160, &handle, sizeof(handle)) != 0 ||
            game_bridge_process_read(target, entry + POORDS4_PAD_CLIENT_USER_ID_1160, &user_id, sizeof(user_id)) != 0 ||
            game_bridge_process_read(target, entry + 0x38, &valid, sizeof(valid)) != 0 ||
            game_bridge_process_read(target, entry + POORDS4_PAD_CLIENT_VENDOR_1160, &vendor, sizeof(vendor)) != 0 ||
            game_bridge_process_read(target, entry + POORDS4_PAD_CLIENT_PRODUCT_1160, &product, sizeof(product)) != 0) {
            TBL_PRINT("%-4u [READ_ERROR]\n", slot);
            continue;
        }
        if (handle <= 0) {
            TBL_PRINT("%-4u %-10s %-10s %-5s %-5s %-5s %-11s %-12s %s\n",
                      slot, "-", "-", "-", "-", "-", "-", "EMPTY", "");
            continue;
        }
        int32_t inferred_index = handle & 0xff;
        int is_ds4 = remote_pad_is_known_ds4(connected, vendor, product);
        int is_dualsense = (vendor == UINT16_C(0x054c) &&
                            (product == UINT16_C(0x0ce6) || product == UINT16_C(0x0df2)));
        const char *ctrl_type = is_ds4 ? "DS4" : (is_dualsense ? "DualSense" : (connected ? "Other" : "Inactive"));
        int matches_user = (user_id == source_user_id);
        int matches_index = (inferred_index == source_pad_index);
        char notes[64];
        notes[0] = '\0';
        if (handle == selected_handle) {
            snprintf(notes, sizeof(notes), "--> SELECTED <--");
        } else if (matches_user && matches_index) {
            snprintf(notes, sizeof(notes), "MATCH_USER+IDX");
        } else if (matches_user) {
            snprintf(notes, sizeof(notes), "MATCH_USER");
        } else if (matches_index) {
            snprintf(notes, sizeof(notes), "MATCH_IDX");
        }

        TBL_PRINT("%-4u 0x%-8x 0x%-8x %-5d %-5d %-5d 0x%04x:0x%04x %-12s %s\n",
                  slot, (uint32_t)handle, (uint32_t)user_id, inferred_index,
                  connected, valid, vendor, product, ctrl_type, notes);
    }

    TBL_PRINT("--------------------------------------------------------------------------------\n");
    TBL_PRINT("Selection Decision:\n");
    TBL_PRINT("  Method:          %s\n", method ? method : "none");
    TBL_PRINT("  Selected Handle: 0x%08x\n", (uint32_t)selected_handle);
    TBL_PRINT("  Selected Index:  %d\n", selected_index);
    TBL_PRINT("  Active Slots:    %u\n", active_count);
    TBL_PRINT("  DS4 Slots:       %u\n", ds4_count);
    TBL_PRINT("  Inactive Slots:  %u\n", inactive_count);
    TBL_PRINT("  Identity Count:  %u\n", identity_count);
    TBL_PRINT("================================================================================\n");

#undef TBL_PRINT
    if (f1) fclose(f1);
    if (f2) fclose(f2);
#else
    (void)target; (void)table; (void)table_stride; (void)source_user_id;
    (void)source_pad_index; (void)method; (void)selected_handle;
    (void)selected_index; (void)active_count; (void)ds4_count;
    (void)inactive_count; (void)identity_count;
#endif
}

/* Issue #29's supplied correction: only an unambiguous user identity maps a
 * secondary reader to a game slot; reader zero stays on the selected primary. */
static void
game_bridge_map_reader_slots(const int32_t *users, const uint32_t *eligible,
                             int32_t primary_index)
{
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        g_game_reader_slots[s] = -1;
        if (!eligible[s] || users[s] < 0 || (int32_t)s == primary_index)
            continue;
        unsigned readers = 0, games = 0;
        int reader = -1;
        for (unsigned r = 0; r < POORDS4_MAX_SLOTS; ++r) {
            if (g_reader_users[r] == users[s]) {
                readers++;
                reader = (int)r;
            }
            if (eligible[r] && users[r] == users[s])
                games++;
        }
        if (readers == 1 && games == 1 && reader > 0)
            g_game_reader_slots[s] = reader;
    }
}

int
wireless_ds4_game_bridge_reader_slot(unsigned game_slot)
{
    if(game_slot>=POORDS4_MAX_SLOTS || g_game_bridge_direct_pid<=0 ||
       g_game_bridge_direct_args<=0 || g_game_reader_slots[game_slot]<=0 ||
       g_game_reader_slots[game_slot]>=(int)POORDS4_MAX_SLOTS)return -1;
    return g_game_reader_slots[game_slot];
}

int
wireless_ds4_remote_reader_read_game_slot(pid_t pid, intptr_t args_address,
                                         unsigned slot, void *pad_data,
                                         uint32_t pad_data_len, uint32_t *out_seq)
{
    if (slot >= POORDS4_MAX_SLOTS || g_game_reader_slots[slot] <= 0)
        return -1;
    return wireless_ds4_remote_reader_read_slot(
        pid, args_address, (unsigned)g_game_reader_slots[slot],
        pad_data, pad_data_len, out_seq);
}

typedef struct {
    int32_t handle, index, user;
    uint32_t eligible;
} GameBridgeReaderClient;

/* Four bridge slots represent attributed feeds, not Sony table ordinals. */
static void
game_bridge_collect_reader_clients(const GameBridgeReaderClient *clients,
                                  unsigned count, int32_t primary_handle,
                                  int32_t primary_index, int32_t source_user,
                                  int32_t *handles, uint32_t *native,
                                  uint32_t *matches, int32_t *users,
                                  uint32_t *eligible)
{
    if (!handles || primary_handle <= 0 || primary_index < 0)
        return;
    unsigned primary = primary_index < (int32_t)POORDS4_MAX_SLOTS
        ? (unsigned)primary_index : 0u;
    int selected[POORDS4_MAX_SLOTS] = {-1,-1,-1,-1};
    for (unsigned reader = 1; reader < POORDS4_MAX_SLOTS; ++reader) {
        int32_t user = g_reader_users[reader];
        unsigned owners = 0;
        for (unsigned r = 0; r < POORDS4_MAX_SLOTS; ++r)
            owners += user >= 0 && g_reader_users[r] == user;
        if (user < 0 || user == source_user || owners != 1)
            continue;
        for (unsigned c = 0; c < count; ++c) {
            if (!clients[c].eligible || clients[c].user != user)
                continue;
            if (selected[reader] < 0 ||
                clients[c].index < clients[selected[reader]].index)
                selected[reader] = (int)c;
        }
    }
    /* Drop unowned extra handles of an attributed user; Sony still handles
     * those calls normally. Never turn them into copies of another feed. */
    for (unsigned slot = 0; slot < POORDS4_MAX_SLOTS; ++slot) {
        if (slot == primary || !eligible[slot] || (native && native[slot]))
            continue;
        int extra = users[slot] == source_user;
        for (unsigned reader = 1; reader < POORDS4_MAX_SLOTS; ++reader)
            if (selected[reader] >= 0 &&
                users[slot] == clients[selected[reader]].user &&
                handles[slot] != clients[selected[reader]].handle)
                extra = 1;
        if (!extra)
            continue;
        handles[slot] = -1; users[slot] = -1; eligible[slot] = 0;
        if (native) native[slot] = 0;
        if (matches) matches[slot] = 0;
    }
    handles[primary] = primary_handle; users[primary] = source_user;
    eligible[primary] = 1;
    if (native) native[primary] = 0;
    if (matches) matches[primary] = 1;
    for (unsigned reader = 1; reader < POORDS4_MAX_SLOTS; ++reader) {
        if (selected[reader] < 0)
            continue;
        const GameBridgeReaderClient *client = &clients[selected[reader]];
        int slot = -1;
        for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s)
            if (s != primary && handles[s] == client->handle) slot = (int)s;
        for (unsigned s = 0; slot < 0 && s < POORDS4_MAX_SLOTS; ++s)
            if (s != primary && handles[s] <= 0) slot = (int)s;
        for (unsigned s = 0; slot < 0 && s < POORDS4_MAX_SLOTS; ++s) {
            int owned = 0;
            for (unsigned r = 0; r < POORDS4_MAX_SLOTS; ++r)
                owned |= users[s] >= 0 && users[s] == g_reader_users[r];
            if (s != primary && (!native || !native[s]) && !owned)
                slot = (int)s;
        }
        if (slot < 0)
            continue;
        handles[slot] = client->handle; users[slot] = client->user;
        eligible[slot] = 1;
        if (native) native[slot] = 0;
        if (matches) matches[slot] = client->user == source_user;
    }
    game_bridge_map_reader_slots(users, eligible, (int32_t)primary);
}

/* Reserve one publication endpoint per independently observed physical user.
 * Sony may not have opened that user's game client yet. Its real handle is
 * bound synchronously by game_bridge_find_slot when the client appears. */
static int
game_bridge_prepare_client_bindings(int32_t source_user, int32_t *handle,
    int32_t *index, int32_t *handles, uint32_t *native, uint32_t *matches,
    int32_t *users)
{
    if (source_user < 0 || !handle || !index || !handles || !native || !matches || !users)
        return -1;
    int previous[POORDS4_MAX_SLOTS];
    for (unsigned slot = 0; slot < POORDS4_MAX_SLOTS; ++slot) {
        previous[slot] = g_game_reader_slots[slot];
        g_game_reader_slots[slot] = -1;
        users[slot] = -1;
    }
    int primary = *handle > 0 ? (*index >= 0 && *index < (int32_t)POORDS4_MAX_SLOTS ? *index : 0) : -1;
    for (unsigned slot = 0; primary < 0 && slot < POORDS4_MAX_SLOTS; ++slot)
        if (handles[slot] <= 0) primary = (int)slot;
    if (primary < 0) return -1;
    if (*handle <= 0) *index = primary;
    users[primary] = source_user;
    native[primary] = 0;
    matches[primary] = 1;
    for (unsigned reader = 1; reader < POORDS4_MAX_SLOTS; ++reader) {
        int32_t user = g_reader_users[reader];
        unsigned owners = 0;
        for (unsigned r = 0; r < POORDS4_MAX_SLOTS; ++r)
            owners += user >= 0 && g_reader_users[r] == user;
        if (user < 0 || user == source_user || owners != 1) continue;
        int target = -1;
        for (unsigned slot = 0; slot < POORDS4_MAX_SLOTS; ++slot)
            if (slot != (unsigned)primary && previous[slot] == (int)reader && !native[slot])
                target = (int)slot;
        for (unsigned slot = 0; target < 0 && slot < POORDS4_MAX_SLOTS; ++slot)
            if (users[slot] < 0 && !native[slot]) target = (int)slot;
        if (target < 0) continue;
        users[target] = user;
        g_game_reader_slots[target] = (int)reader;
    }
    return primary;
}

static int
game_bridge_select_pad_handle(
    pid_t target, intptr_t libpad_base, intptr_t read_state,
    int32_t source_user_id, int32_t source_pad_index, int report_fd,
    int32_t *out_handle, int32_t *out_index,
    int32_t *out_slot_handles, uint32_t *out_slot_dualsense,
    uint32_t *out_slot_user_matches)
{
    if (target <= 0 || libpad_base <= 0 || read_state <= 0 ||
        source_user_id < 0 || source_pad_index < 0 ||
        source_pad_index >= 8 ||
        !out_handle || !out_index)
        return -1;

    *out_handle = -1;
    *out_index = -1;
    int32_t slot_users[POORDS4_MAX_SLOTS] = {-1, -1, -1, -1};
    uint32_t slot_eligible[POORDS4_MAX_SLOTS] = {0};
    GameBridgeReaderClient clients[24];
    unsigned client_count = 0;
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s)
        g_game_reader_slots[s] = -1;
    if (out_slot_handles) {
        for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
            out_slot_handles[s] = -1;
            if (out_slot_dualsense)
                out_slot_dualsense[s] = 0;
            if (out_slot_user_matches)
                out_slot_user_matches[s] = 0;
        }
    }
    GameBridgeDynlibObjectPrefix object;
    memset(&object, 0, sizeof(object));
    intptr_t table = 0;
    uint32_t table_stride = POORDS4_PAD_CLIENT_STRIDE_1160;
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
            read_state, source_user_id, source_pad_index,
            report_fd, &table, &table_stride) != 0) {
        report_printf(report_fd, "error=pad_client_table_discovery\n");
        return -1;
    }

    int32_t sole_handle = -1;
    int32_t sole_index = -1;
    uint16_t sole_vendor = 0;
    uint16_t sole_product = 0;
    int32_t unique_ds4_handle = -1;
    int32_t unique_ds4_index = -1;
    int32_t unique_inactive_handle = -1;
    int32_t unique_inactive_index = -1;
    int32_t identity_inactive_handle = -1;
    int32_t identity_inactive_index = -1;
    int32_t source_user_inactive_handle = -1;
    int32_t source_user_inactive_index = -1;
    int32_t source_user_inactive_first_handle = -1;
    int32_t source_user_inactive_first_index = -1;
    int32_t global_inactive_first_handle = -1;
    int32_t global_inactive_first_index = -1;
    int32_t source_index_inactive_handle = -1;
    int32_t source_index_inactive_index = -1;
    unsigned active_count = 0;
    unsigned indexed_count = 0;
    unsigned indexed_ds4_count = 0;
    unsigned indexed_inactive_count = 0;
    unsigned identity_count = 0;
    unsigned identity_inactive_count = 0;
    unsigned source_user_inactive_count = 0;
    unsigned ds4_count = 0;
    unsigned inactive_count = 0;
    report_printf(
        report_fd, "pad_client_table=0x%lx stride=0x%x mapsize=0x%llx\n",
        (unsigned long)table, table_stride, (unsigned long long)object.mapsize);

    for (unsigned slot = 0; slot < 24u; ++slot) {
        intptr_t entry = table + (intptr_t)slot * table_stride;
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
        int is_dualsense = (vendor == UINT16_C(0x054c) &&
                            (product == UINT16_C(0x0ce6) || product == UINT16_C(0x0df2)));
        int is_active_dualsense = (connected && is_dualsense);
        int is_inactive = !is_active_dualsense && (!connected || is_ds4);
        int matches_user = (user_id == source_user_id);
        int matches_index = (inferred_index == source_pad_index);
        clients[client_count++] = (GameBridgeReaderClient){
            handle, inferred_index, user_id, is_inactive ? 1u : 0u};
        report_printf(
            report_fd,
            "pad_client slot=%u entry=0x%lx handle=0x%08x "
            "user_id=0x%08x inferred_index=%d connected=%d valid=%d "
            "vid=0x%04x pid=0x%04x ds4=%d\n",
            slot, (unsigned long)entry, (uint32_t)handle,
            (uint32_t)user_id,
            inferred_index, connected, valid, vendor, product, is_ds4);

        if (out_slot_handles && inferred_index >= 0 && inferred_index < (int32_t)POORDS4_MAX_SLOTS) {
            if (out_slot_handles[inferred_index] <= 0 || is_active_dualsense) {
                out_slot_handles[inferred_index] = handle;
                slot_users[inferred_index] = user_id;
                slot_eligible[inferred_index] = is_inactive ? 1u : 0u;
                if (out_slot_dualsense)
                    out_slot_dualsense[inferred_index] = is_active_dualsense ? 1u : 0u;
                if (out_slot_user_matches)
                    out_slot_user_matches[inferred_index] = matches_user ? 1u : 0u;
            }
        }

        active_count++;
        sole_handle = handle;
        sole_index = inferred_index;
        sole_vendor = vendor;
        sole_product = product;
        if (is_ds4) {
            ds4_count++;
            unique_ds4_handle = handle;
            unique_ds4_index = inferred_index;
        }
        if (is_inactive) {
            inactive_count++;
            unique_inactive_handle = handle;
            unique_inactive_index = inferred_index;
            if (global_inactive_first_handle < 0 ||
                inferred_index < global_inactive_first_index) {
                global_inactive_first_handle = handle;
                global_inactive_first_index = inferred_index;
            }
        }
        if (matches_index) {
            indexed_count++;
            if (is_ds4)
                indexed_ds4_count++;
            if (is_inactive) {
                indexed_inactive_count++;
                source_index_inactive_handle = handle;
                source_index_inactive_index = inferred_index;
            }
        }
        if (matches_user) {
            if (is_inactive) {
                source_user_inactive_count++;
                source_user_inactive_handle = handle;
                source_user_inactive_index = inferred_index;
                if (source_user_inactive_first_handle < 0 ||
                    inferred_index < source_user_inactive_first_index) {
                    source_user_inactive_first_handle = handle;
                    source_user_inactive_first_index = inferred_index;
                }
            }
            if (matches_index) {
                identity_count++;
                if (is_inactive) {
                    identity_inactive_count++;
                    identity_inactive_handle = handle;
                    identity_inactive_index = inferred_index;
                }
            }
        }
    }

    const char *method = "none";
    /* Multi-slot and multi-controller selection hierarchy:
     * 1. Exact DS4 identified in game table (e.g. PS4 mode).
     * 2. Source user + source index inactive match (standard DS4 1st).
     * 3. Source user unique inactive match (DS4 2nd alongside DualSense or preallocated).
     * 4. Source user multiple inactive pick first available inactive slot (e.g. FC 26 P2).
     *    A global index match must not replace an established user match.
     * 5. Global unique inactive match (single inactive slot across table).
     * 6. Global inactive disambiguated by index.
     * 7. Global multiple inactive pick first available inactive slot (e.g. FC 26 P2 multi-user).
     * 8. Sole active entry bound to identity. */
    if (ds4_count == 1u) {
        *out_handle = unique_ds4_handle;
        *out_index = unique_ds4_index;
        method = "global-exact-ds4";
    } else if (ds4_count == 0u && identity_count == 1u &&
               identity_inactive_count == 1u) {
        *out_handle = identity_inactive_handle;
        *out_index = identity_inactive_index;
        method = "source-user-index-inactive";
    } else if (ds4_count == 0u && source_user_inactive_count == 1u) {
        *out_handle = source_user_inactive_handle;
        *out_index = source_user_inactive_index;
        method = "source-user-inactive-unique";
    } else if (ds4_count == 0u && source_user_inactive_count > 1u &&
               source_user_inactive_first_handle >= 0) {
        *out_handle = source_user_inactive_first_handle;
        *out_index = source_user_inactive_first_index;
        method = "source-user-inactive-first";
    } else if (ds4_count == 0u && inactive_count == 1u) {
        *out_handle = unique_inactive_handle;
        *out_index = unique_inactive_index;
        method = "global-inactive-unique";
    } else if (ds4_count == 0u && inactive_count > 1u &&
               indexed_inactive_count == 1u) {
        *out_handle = source_index_inactive_handle;
        *out_index = source_index_inactive_index;
        method = "global-index-inactive-unique";
    } else if (ds4_count == 0u && inactive_count > 1u &&
               global_inactive_first_handle >= 0) {
        *out_handle = global_inactive_first_handle;
        *out_index = global_inactive_first_index;
        method = "global-inactive-first";
    } else if (ds4_count == 0u && inactive_count == 0u &&
               active_count == 1u && identity_count == 1u &&
               (sole_vendor != UINT16_C(0x054c) ||
                (sole_product != UINT16_C(0x0ce6) &&
                 sole_product != UINT16_C(0x0df2)))) {
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
    klog_printf(
        "[PoorDS4] pad selection: method=%s handle=0x%08x index=%d "
        "active=%u ds4=%u inactive=%u identity=%u\n",
        method, (uint32_t)*out_handle, *out_index,
        active_count, ds4_count, inactive_count, identity_count);
    game_bridge_dump_client_table_report(
        target, table, table_stride, source_user_id, source_pad_index,
        method, *out_handle, *out_index, active_count, ds4_count,
        inactive_count, identity_count);
    game_bridge_collect_reader_clients(clients, client_count, *out_handle,
        *out_index, source_user_id, out_slot_handles, out_slot_dualsense,
        out_slot_user_matches, slot_users, slot_eligible);
    return *out_handle >= 0 ? 0 : -1;
}

/* Same application-info ABI used by the supervisor's title report. Only a
 * successful, complete CUSA title identifies a PS4 game; unknown metadata
 * retains the established PS5/homebrew discovery path. */
typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} GameBridgeAppInfo;
extern int sceKernelGetAppInfo(pid_t pid, GameBridgeAppInfo *info);

static int
game_bridge_is_native_ps4(pid_t pid)
{
    GameBridgeAppInfo info;
    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(pid, &info) != 0 ||
        memcmp(info.title_id, "CUSA", 4) != 0 || info.title_id[9] != '\0')
        return 0;
    for (unsigned byte = 4; byte < 9; ++byte)
        if (info.title_id[byte] < '0' || info.title_id[byte] > '9')
            return 0;
    return 1;
}

int
wireless_ds4_game_bridge_rebind_readers(pid_t pid, intptr_t args_address,
                                       const PoorDS4PadSource *source)
{
#if !defined(__PROSPERO__)
    (void)pid;(void)args_address;(void)source;return -1;
#else
    /* Discovery ordinals can reorder after Bluetooth/user-service recovery.
     * Re-read the existing game table; never retain or guess an old ordinal. */
    for (unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)
        g_game_reader_slots[slot]=-1;
    GamePadBridgeArgs args;
    if (!source || source->user_id<0 || source->pad_index<0 ||
        pid!=g_game_bridge_direct_pid || args_address!=g_game_bridge_direct_args ||
        game_bridge_copy_args(pid,args_address,&args) ||
        args.magic!=POORDS4_GAME_BRIDGE_MAGIC ||
        args.reserved1!=POORDS4_GAME_BRIDGE_LAYOUT_V1) {
        poords4_log("[PoorDS4] READER ROUTE reinstall reason=owner-or-prefix\n");
        return 1;
    }
    uint32_t library=0;
    if (get_lib_quiet(pid,"libScePad",&library)) {
        poords4_log("[PoorDS4] READER ROUTE reinstall reason=library\n");return 1;
    }
    intptr_t base=kernel_dynlib_mapbase_addr(pid,library);
    intptr_t read_state=resolve_sym(pid,library,"scePadReadState");
    /* Despite its historical name, RC51 stores originals[0] here: the public
     * wrapper, not its internal tail-jump target. Match the real installer. */
    if (base<=0 || read_state<=0 || read_state!=args.fp_state_internal) {
        poords4_log("[PoorDS4] READER ROUTE reinstall reason=read-state-export\n");return 1;
    }
    if (args.slots[0].reserved[2] == POORDS4_CLIENT_BINDING_MAGIC) {
        GamePadBridgeTriggerArgs extension;
        const size_t offset=offsetof(GamePadBridgeTriggerArgs,client_binding_magic);
        if(remote_reader_copyout(pid,args_address+(intptr_t)offset,
                (uint8_t *)&extension+offset,sizeof(extension)-offset) ||
           extension.client_binding_magic!=POORDS4_CLIENT_BINDING_MAGIC ||
           extension.client_primary_slot>=POORDS4_MAX_SLOTS ||
           extension.client_users[extension.client_primary_slot]!=source->user_id ||
           !args.slots[extension.client_primary_slot].active)
            return 1;
        int32_t users[POORDS4_MAX_SLOTS];
        unsigned primary=extension.client_primary_slot;
        for(unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)users[slot]=-1;
        users[primary]=source->user_id;
        /* Keep existing physical users at their logical endpoints. A Sony
         * native client joining is not a reason to remove any game hook. */
        for(unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++) {
            if(slot==primary)continue;
            int32_t user=extension.client_users[slot];
            unsigned owners=0;int reader=-1;
            for(unsigned r=1;r<POORDS4_MAX_SLOTS;r++)
                if(user>=0 && g_reader_users[r]==user){owners++;reader=(int)r;}
            if(user!=source->user_id && owners==1){users[slot]=user;g_game_reader_slots[slot]=reader;}
        }
        for(unsigned r=1;r<POORDS4_MAX_SLOTS;r++) {
            int32_t user=g_reader_users[r];unsigned owners=0;int assigned=0;
            for(unsigned other=0;other<POORDS4_MAX_SLOTS;other++)owners+=user>=0 && g_reader_users[other]==user;
            for(unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)assigned|=users[slot]==user;
            if(user<0 || owners!=1 || assigned)continue;
            for(unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)if(users[slot]<0) {
                users[slot]=user;g_game_reader_slots[slot]=(int)r;break;
            }
        }
        for(unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++) {
            if(users[slot]==extension.client_users[slot])continue;
            if(wireless_ds4_game_bridge_deactivate_slot(pid,args_address,slot) ||
               game_bridge_process_write(pid,args_address+(intptr_t)offsetof(GamePadBridgeTriggerArgs,client_users)+
                    (intptr_t)slot*sizeof(int32_t),&users[slot],sizeof(users[slot])))
                return 1;
        }
        poords4_log("[PoorDS4] READER ROUTE refreshed live users primary=%u hooks=retained\n",primary);
        return 0;
    }
    if(!args.direct_active)return 1;
    int32_t handle=-1,index=-1,handles[POORDS4_MAX_SLOTS];
    uint32_t native[POORDS4_MAX_SLOTS],matches[POORDS4_MAX_SLOTS];
    int result=game_bridge_select_pad_handle(pid,base,read_state,
        source->user_id,source->pad_index,-1,&handle,&index,handles,native,matches);
    if (!result && index==(int32_t)args.reserved0 && handle==args.pad_handle) {
        for (unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)
            if (handles[slot]!=args.slots[slot].pad_handle ||
                native[slot]!=args.slots[slot].is_dualsense ||
                matches[slot]!=args.slots[slot].reserved[0])result=1;
        if (!result) {
            poords4_log("[PoorDS4] READER ROUTE refreshed primary=%d\n",index);
            return 0;
        }
    }
    for (unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)
        g_game_reader_slots[slot]=-1;
    poords4_log("[PoorDS4] READER ROUTE reinstall reason=selection-or-slot-identity\n");
    return 1;
#endif
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

    /* Recheck at installation, including a title change after discovery.
     * PS4 already accepts DS4: do not inspect its pad layout or patch imports. */
    if (game_bridge_is_native_ps4(target))
        return POORDS4_GAME_BRIDGE_NATIVE_PS4;

    int process_watch_result = poords4_game_process_watch_open(&g_game_process_watch, target);
    klog_printf("[PoorDS4] PROC WATCH available=%d error=%d pid=%d\n",
                process_watch_result == 0, process_watch_result == 0 ? 0 : errno, target);

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
        "poords4_version=%s\nreport_schema=9\n"
        "install_mode=passive-target-anonymous\nptrace_calls=0\n"
        "remote_game_calls=0\ngame_heap_allocations=0\n",
        firmware, target, (uint32_t)source->user_id,
        source->pad_index, (uint32_t)source->pad_handle,
        (uint32_t)source->ds4_connected, POORDS4_VERSION);

    uint32_t libpad_handle = 0;
    if (get_lib(target, "libScePad", &libpad_handle) != 0) {
        report_printf(report_fd, "error=libpad\n");
        goto done;
    }
    intptr_t base = kernel_dynlib_mapbase_addr(target, libpad_handle);
    intptr_t originals[POORDS4_GAME_BRIDGE_API_COUNT] = {
        resolve_sym(target, libpad_handle, "scePadReadState"),
        resolve_sym(target, libpad_handle, "scePadReadStateExt"),
        resolve_sym(target, libpad_handle, "scePadRead"),
        resolve_sym(target, libpad_handle, "scePadReadExt"),
        resolve_sym(target, libpad_handle, "scePadGetDataInternal"),
        resolve_sym(target, libpad_handle,
                    "scePadGetControllerInformation"),
        resolve_sym(target, libpad_handle,
                    "scePadGetExtControllerInformation"),
        resolve_sym(target, libpad_handle, "scePadGetTriggerEffectState"),
        resolve_sym(target, libpad_handle, "scePadSetVibration"),
        resolve_sym(target, libpad_handle, "scePadSetVibrationMode"),
        resolve_sym(target, libpad_handle, "scePadSetTriggerEffect"),
        resolve_sym(target, libpad_handle, "scePadDeviceClassGetExtendedInformation")
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
    game_bridge_report_libpad_layout(target, base, report_fd);
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
    const uint32_t offsets_1001[6] = {
        POORDS4_READ_STATE_OFFSET_1001,
        POORDS4_READ_STATE_EXT_OFFSET_1001,
        POORDS4_READ_OFFSET_1001,
        POORDS4_READ_EXT_OFFSET_1001,
        POORDS4_DATA_INTERNAL_OFFSET_1001,
        POORDS4_CONTROLLER_INFO_OFFSET_1001
    };
    const uint64_t hashes_1001[6] = {
        POORDS4_READ_STATE_FNV256_1001,
        POORDS4_READ_STATE_EXT_FNV256_1001,
        POORDS4_READ_FNV256_1001,
        POORDS4_READ_EXT_FNV256_1001,
        POORDS4_DATA_INTERNAL_FNV256_1001,
        POORDS4_CONTROLLER_INFO_FNV256_1001
    };
    const uint32_t offsets_0602[6] = {
        POORDS4_READ_STATE_OFFSET_0602,
        POORDS4_READ_STATE_EXT_OFFSET_0602,
        POORDS4_READ_OFFSET_0602,
        POORDS4_READ_EXT_OFFSET_0602,
        POORDS4_DATA_INTERNAL_OFFSET_0602,
        POORDS4_CONTROLLER_INFO_OFFSET_0602
    };
    const uint64_t hashes_0602[6] = {
        POORDS4_READ_STATE_FNV256_0602,
        POORDS4_READ_STATE_EXT_FNV256_0602,
        POORDS4_READ_FNV256_0602,
        POORDS4_READ_EXT_FNV256_0602,
        POORDS4_DATA_INTERNAL_FNV256_0602,
        POORDS4_CONTROLLER_INFO_FNV256_0602
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
        firmware == POORDS4_GAME_BRIDGE_FW_0602 ? offsets_0602 :
        firmware == POORDS4_GAME_BRIDGE_FW_0860 ? offsets_0860 :
        firmware == POORDS4_GAME_BRIDGE_FW_1001 ? offsets_1001 :
        firmware == POORDS4_GAME_BRIDGE_FW_1160 ? offsets_1160 :
        firmware == POORDS4_GAME_BRIDGE_FW_1240 ? offsets_1240 : NULL;
    const uint64_t *expected_hashes =
        firmware == POORDS4_GAME_BRIDGE_FW_0602 ? hashes_0602 :
        firmware == POORDS4_GAME_BRIDGE_FW_0860 ? hashes_0860 :
        firmware == POORDS4_GAME_BRIDGE_FW_1001 ? hashes_1001 :
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
    int state_target_protection = common_state_target
        ? game_bridge_target_protection(target, wrapper_targets[0]) : -1;
    int read_target_protection = common_read_target
        ? game_bridge_target_protection(target, wrapper_targets[2]) : -1;
    int data_target_protection = wrapper_targets_ok[4]
        ? game_bridge_target_protection(target, wrapper_targets[4]) : -1;
    int state_target_exec = (state_target_protection > 0 &&
        (state_target_protection & PROT_EXEC) != 0) ||
        (state_target_protection <= 0 && wrapper_targets_ok[0]);
    int read_target_exec = (read_target_protection > 0 &&
        (read_target_protection & PROT_EXEC) != 0) ||
        (read_target_protection <= 0 && wrapper_targets_ok[2]);
    int data_target_exec = (data_target_protection > 0 &&
        (data_target_protection & PROT_EXEC) != 0) ||
        (data_target_protection <= 0 && wrapper_targets_ok[4]);
    wrapper_targets_exec[0] = wrapper_targets_exec[1] =
        state_target_exec;
    wrapper_targets_exec[2] = wrapper_targets_exec[3] =
        read_target_exec;
    wrapper_targets_exec[4] = data_target_exec;
    int controller_info_prefix_ok =
        memcmp(fingerprints[5], controller_info_prologue,
               sizeof(controller_info_prologue)) == 0;
    int controller_info_protection =
        game_bridge_target_protection(target, originals[5]);
    int controller_info_exec = (controller_info_protection > 0 &&
        (controller_info_protection & PROT_EXEC) != 0) ||
        (controller_info_protection <= 0 && originals[5] > 0);
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
    int structural_nonmapping_ok = common_state_target &&
        common_read_target && controller_info_abi_ok;
    for (unsigned index = 0; index < 5; ++index) {
        if (!wrapper_shapes_ok[index] || !wrapper_targets_ok[index])
            structural_nonmapping_ok = 0;
    }
    int executable_mapping_wait = exact_manifest &&
        structural_nonmapping_ok &&
        (!state_target_exec || !read_target_exec ||
         !data_target_exec || !controller_info_exec);
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
            ? (firmware == POORDS4_GAME_BRIDGE_FW_0602
                ? "6.02-exact"
                : firmware == POORDS4_GAME_BRIDGE_FW_0860
                    ? "8.60-exact"
                    : firmware == POORDS4_GAME_BRIDGE_FW_1001
                        ? "10.01-exact"
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
    report_printf(
        report_fd,
        "target_protection state=0x%x read=0x%x data=0x%x "
        "controller_info=0x%x\n",
        state_target_protection, read_target_protection,
        data_target_protection, controller_info_protection);
    if (!structural_abi) {
        report_printf(
            report_fd,
            "error=%s\n",
            executable_mapping_wait
                ? "waiting_for_executable_mapping"
                : "unsupported_firmware_abi");
        result = executable_mapping_wait ? -4 : 0;
        goto done;
    }

    int32_t game_pad_handle = -1;
    int32_t game_pad_index = -1;
    int32_t slot_handles[POORDS4_MAX_SLOTS] = {-1, -1, -1, -1};
    uint32_t slot_dualsense[POORDS4_MAX_SLOTS] = {0, 0, 0, 0};
    uint32_t slot_user_matches[POORDS4_MAX_SLOTS] = {0, 0, 0, 0};
    int selection = game_bridge_select_pad_handle(
            target, base, originals[0], source->user_id,
            source->pad_index,
            report_fd, &game_pad_handle, &game_pad_index,
            slot_handles, slot_dualsense, slot_user_matches);
    GameBridgeDynlibObjectPrefix client_object = {0};
    intptr_t client_table = 0;
    uint32_t client_stride = 0;
    if (game_cache_find_libpad_object(target, base, &client_object) ||
        game_cache_find_table(target, base, client_object.mapsize, originals[0],
            source->user_id, source->pad_index, report_fd, &client_table, &client_stride)) {
        report_printf(report_fd, "state=waiting_for_game_client_table\n");
        result = -4;
        goto done;
    }
    int32_t client_users[POORDS4_MAX_SLOTS];
    int primary_endpoint = game_bridge_prepare_client_bindings(source->user_id,
        &game_pad_handle, &game_pad_index, slot_handles, slot_dualsense,
        slot_user_matches, client_users);
    if (primary_endpoint < 0) {
        report_printf(report_fd, "state=waiting_for_game_endpoint\n");
        result = -4;
        goto done;
    }
    report_printf(report_fd,
        "client_binding_mode=live-user-table selection=%d primary_endpoint=%d deferred=%d\n",
        selection, primary_endpoint, game_pad_handle <= 0);

    memset(hooks, 0, sizeof(hooks));
    int stale_result = game_bridge_recover_stale_v1(
        target, base, originals, hooks, &hook_count, report_fd);
    if (stale_result != 0) {
        /* A successful recovery asks the supervisor to retry once after the
         * original slots are visible. An unowned unexpected import fails
         * closed and must not be overwritten. A transient scan failure (e.g.
         * modules still loading) returns -4 to retry during launch grace. */
        result = stale_result > 0 ? -5 : (stale_result == -2 ? -4 : -6);
        goto done;
    }


    uintptr_t local_stub_base = (uintptr_t)game_pad_read_state_stub;
    uintptr_t local_stub_end =
        (uintptr_t)game_pad_bridge_stub_end;
    const uintptr_t local_stubs[POORDS4_GAME_BRIDGE_API_COUNT] = {
        (uintptr_t)game_pad_read_state_stub,
        (uintptr_t)game_pad_read_state_ext_stub,
        (uintptr_t)game_pad_read_stub,
        (uintptr_t)game_pad_read_ext_stub,
        (uintptr_t)game_pad_get_data_internal_stub,
        (uintptr_t)game_pad_get_controller_info_stub,
        (uintptr_t)game_pad_get_ext_controller_info_stub,
        (uintptr_t)game_pad_get_trigger_effect_state_stub,
        (uintptr_t)game_pad_set_vibration_stub,
        (uintptr_t)game_pad_set_vibration_mode_stub,
        (uintptr_t)game_pad_set_trigger_effect_stub,
        (uintptr_t)game_pad_device_class_info_stub
    };
    if (local_stub_end <= local_stub_base)
        goto done;
    for (unsigned index = 0; index < POORDS4_GAME_BRIDGE_API_COUNT; ++index) {
        if (local_stubs[index] < local_stub_base ||
            local_stubs[index] >= local_stub_end) {
            report_printf(report_fd, "error=stub_layout index=%u\n", index);
            goto done;
        }
    }
    size_t stub_size = (size_t)(local_stub_end - local_stub_base);
    size_t gateway_offset = (stub_size + 15u) & ~(size_t)15u;
    size_t code_size = gateway_offset + POORDS4_GAME_BRIDGE_API_COUNT * 16u;
    intptr_t code_address = 0;
    intptr_t args_address = 0;
    size_t code_mapping_size =
        (code_size + POORDS4_TARGET_PAGE_SIZE - 1u) &
        ~(size_t)(POORDS4_TARGET_PAGE_SIZE - 1u);
    size_t args_mapping_size =
        (sizeof(GamePadBridgeTriggerArgs) + POORDS4_TARGET_PAGE_SIZE - 1u) &
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
    intptr_t remote_stubs[POORDS4_GAME_BRIDGE_API_COUNT];
    static const uint8_t args_in_rcx[POORDS4_GAME_BRIDGE_API_COUNT] = {0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned index = 0; index < POORDS4_GAME_BRIDGE_API_COUNT; ++index) {
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
    args.legacy_receiver_port = POORDS4_EMPTY_READ_DIAG_MAGIC;
    args.last_caller_handle = -1;
    args.last_mismatched_handle = -1;
    for (unsigned i = 0; i < 4u; ++i) {
        args.observed_handles[i] = -1;
        args.observed_handle_calls[i] = 0;
    }
    unsigned ds4_slot = (game_pad_index >= 0 && game_pad_index < (int32_t)POORDS4_MAX_SLOTS)
        ? (unsigned)game_pad_index : 0u;
    for (unsigned i = 0; i < POORDS4_MAX_SLOTS; ++i) {
        if (slot_handles[i] > 0) {
            args.slots[i].pad_handle = slot_handles[i];
            args.slots[i].pad_index = slot_handles[i] & 0xff;
            args.slots[i].is_dualsense = slot_dualsense[i] ? 1u : 0u;
            args.slots[i].reserved[0] = slot_user_matches[i] ? 1u : 0u;
        } else {
            args.slots[i].pad_handle = -1;
            args.slots[i].pad_index = -1;
            args.slots[i].is_dualsense = 0;
            args.slots[i].reserved[0] = 0;
        }
        args.slots[i].active = 0;
        args.slots[i].is_simulated = 0;
        args.slots[i].direct_seq = 0;
        args.slots[i].direct_lease = 0;
        args.slots[i].direct_packets = 0;
        args.slots[i].last_read_seq = 0;
    }
    args.slots[ds4_slot].pad_handle = game_pad_handle;
    args.slots[ds4_slot].pad_index = (int32_t)game_pad_index;
    args.slots[ds4_slot].is_dualsense = 0;
    args.slots[ds4_slot].reserved[0] = 1u;
    args.fp_state_internal = originals[0];
    args.fp_read_internal = originals[2];
    args.fp_data_internal = originals[4];
    args.fp_get_controller_info_trampoline = originals[5];
    args.fp_get_ext_controller_info_trampoline = originals[6];
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
    GamePadBridgeTriggerArgs trigger_args={.input=args,.magic=POORDS4_TRIGGER_ARGS_MAGIC,
        .fp_get_trigger_effect_state=originals[7],
        .fp_set_trigger_effect=originals[10],.effect_magic=POORDS4_EFFECT_ARGS_MAGIC,
        .effect_diag_magic=POORDS4_EFFECT_DIAG_MAGIC,
        .fp_device_class_info=originals[11],
        .device_class_diag_magic=POORDS4_DEVICE_CLASS_DIAG_MAGIC};
    trigger_args.client_binding_magic = POORDS4_CLIENT_BINDING_MAGIC;
    trigger_args.input.slots[0].reserved[2] = POORDS4_CLIENT_BINDING_MAGIC;
    trigger_args.client_table = client_table;
    trigger_args.client_stride = client_stride;
    trigger_args.client_primary_slot = (uint32_t)primary_endpoint;
    for (unsigned slot = 0; slot < POORDS4_MAX_SLOTS; ++slot)
        trigger_args.client_users[slot] = client_users[slot];
    PoorDS4GameOutput *game_output=&trigger_args.rumble;
    game_output->magic=POORDS4_GAME_OUTPUT_MAGIC;game_output->version=1;
    game_output->size=sizeof *game_output;game_output->slots=4;
    game_output->fp_vibration=originals[8];game_output->fp_mode=originals[9];
    uint32_t game_kernel_handle=0;
    const char *game_kernel_names[]={"libkernel_sys","libkernel_web","libkernel"};
    for(unsigned index=0;index<3;index++) {
        if(!get_lib(target,game_kernel_names[index],&game_kernel_handle)) {
            game_output->fp_clock=resolve_sym(target,game_kernel_handle,"clock_gettime");
            if(game_output->fp_clock)break;
        }
    }
    if (game_bridge_process_write(
            target, args_address, &trigger_args, sizeof(trigger_args)) != 0) {
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
    while (patched_count > 0 && wireless_ds4_game_bridge_image_state(target) == 0) {
        uint32_t index = --patched_count;
        int64_t restore_stages[9];
        if (poords4_remote_cow_write(
                target, hooks[index].slot, &hooks[index].original,
                sizeof(hooks[index].original),
                (int)hooks[index].protection, restore_stages) != 0) {
            (void)game_bridge_process_write(
                target, hooks[index].slot, &hooks[index].original,
                sizeof(hooks[index].original));
        }
    }
done:
    if (wireless_ds4_game_bridge_image_state(target) > 0)
        result = POORDS4_GAME_BRIDGE_IMAGE_CHANGED;
    if (result != 1)
        report_printf(report_fd, "result=%d\n", result);
    if (report_fd >= 0)
        close(report_fd);
    if (g_report_archive_fd >= 0) {
        close(g_report_archive_fd);
        g_report_archive_fd = -1;
    }
    if (result != 1) {
        if (remote_mapping > 0 && remote_mapping_size != 0 &&
            wireless_ds4_game_bridge_image_state(target) == 0)
            (void)poords4_remote_unmap(
                target, remote_mapping, remote_mapping_size);
        poords4_game_process_watch_close(&g_game_process_watch);
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
    if (game_bridge_is_native_ps4(target))
        return POORDS4_GAME_BRIDGE_NATIVE_PS4;
    uint32_t libpad = 0;
    uint32_t libc = 0;
    uint32_t libkernel = 0;
    if (get_lib_quiet(target, "libScePad", &libpad) != 0 ||
        get_lib_quiet(target, "libSceLibcInternal", &libc) != 0 ||
        (get_lib_quiet(target, "libkernel_sys", &libkernel) != 0 &&
         get_lib_quiet(target, "libkernel", &libkernel) != 0 &&
         get_lib_quiet(target, "libkernel_web", &libkernel) != 0))
        return -4;
    return 1;
#endif
}

int
wireless_ds4_game_bridge_image_state(pid_t game_pid)
{
    return poords4_game_process_watch_poll(&g_game_process_watch, game_pid);
}

int
wireless_ds4_game_bridge_abandon(void)
{
#if !defined(__PROSPERO__)
    return -1;
#else
    poords4_game_process_watch_close(&g_game_process_watch);
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
    if (wireless_ds4_game_bridge_image_state(game_pid) > 0)
        return wireless_ds4_game_bridge_abandon();
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
    poords4_game_process_watch_close(&g_game_process_watch);
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
wireless_ds4_game_bridge_update_slot(pid_t game_pid, intptr_t args_kaddr,
                                     unsigned slot_idx, const void *pad_data,
                                     uint32_t pad_data_len, int is_simulated,
                                     int is_dualsense)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)slot_idx; (void)pad_data;
    (void)pad_data_len; (void)is_simulated; (void)is_dualsense;
    return -1;
#else
    if (game_pid <= 0 || !args_kaddr || !pad_data ||
        pad_data_len < POORDS4_GAME_BRIDGE_PAD_SIZE ||
        slot_idx >= POORDS4_MAX_SLOTS)
        return -1;
    if (game_pid != g_game_bridge_direct_pid ||
        args_kaddr != g_game_bridge_direct_args) {
        g_game_bridge_direct_pid = game_pid;
        g_game_bridge_direct_args = args_kaddr;
        g_game_bridge_direct_seq = 0;
        g_game_bridge_direct_packets = 0;
        for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
            g_game_bridge_slot_seq[s] = 0;
            g_game_bridge_slot_packets[s] = 0;
        }
        g_game_bridge_direct_cr3 = 0;
        g_game_bridge_direct_dmap = 0;
    }
    uint32_t next_seq = g_game_bridge_slot_seq[slot_idx] + 1u;
    if (next_seq == 0)
        next_seq = 1;
    uint32_t buffer_idx = next_seq & 1u;

    intptr_t slot_base = args_kaddr + (intptr_t)offsetof(GamePadBridgeArgs, slots) +
        (intptr_t)slot_idx * (intptr_t)sizeof(GamePadBridgeSlot);
    intptr_t frame_address = slot_base +
        (intptr_t)offsetof(GamePadBridgeSlot, direct_pad_data) +
        (intptr_t)buffer_idx * POORDS4_GAME_BRIDGE_PAD_SIZE;

    if (game_bridge_process_write(
            game_pid, frame_address, pad_data,
            POORDS4_GAME_BRIDGE_PAD_SIZE) != 0)
        return -1;

    struct {
        uint32_t active;
        uint32_t is_dualsense;
        uint32_t is_simulated;
        uint32_t direct_seq;
        uint32_t direct_lease;
        uint32_t direct_packets;
    } slot_pub;
    slot_pub.active = 1u;
    slot_pub.is_dualsense = (uint32_t)(is_dualsense ? 1 : 0);
    slot_pub.is_simulated = (uint32_t)(is_simulated ? 1 : 0);
    slot_pub.direct_seq = next_seq;
    slot_pub.direct_lease = POORDS4_GAME_BRIDGE_DIRECT_LEASE;
    slot_pub.direct_packets = g_game_bridge_slot_packets[slot_idx] + 1u;

    if (game_bridge_process_write(
            game_pid,
            slot_base + (intptr_t)offsetof(GamePadBridgeSlot, active),
            &slot_pub, sizeof(slot_pub)) != 0)
        return -1;

    g_game_bridge_slot_seq[slot_idx] = next_seq;
    g_game_bridge_slot_packets[slot_idx] = slot_pub.direct_packets;

    if (slot_idx == 0) {
        intptr_t legacy_frame = args_kaddr +
            (intptr_t)offsetof(GamePadBridgeArgs, direct_pad_data) +
            (intptr_t)buffer_idx * POORDS4_GAME_BRIDGE_PAD_SIZE;
        (void)game_bridge_process_write(
            game_pid, legacy_frame, pad_data,
            POORDS4_GAME_BRIDGE_PAD_SIZE);
        struct {
            uint32_t seq;
            uint32_t lease;
            uint32_t active;
            uint32_t packets;
        } publication;
        publication.seq = next_seq;
        publication.lease = POORDS4_GAME_BRIDGE_DIRECT_LEASE;
        publication.active = 1;
        publication.packets = slot_pub.direct_packets;
        (void)game_bridge_process_write(
            game_pid,
            args_kaddr + (intptr_t)offsetof(
                GamePadBridgeArgs, direct_seq), &publication,
            sizeof(publication));
        g_game_bridge_direct_seq = next_seq;
        g_game_bridge_direct_packets = publication.packets;
    }
    return 0;
#endif
}

int
wireless_ds4_game_bridge_deactivate_slot(pid_t game_pid, intptr_t args_kaddr,
                                        uint32_t slot_idx)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)slot_idx;
    return -1;
#else
    if (game_pid <= 0 || !args_kaddr || slot_idx >= POORDS4_MAX_SLOTS)
        return -1;
    intptr_t slot_base = args_kaddr + (intptr_t)offsetof(GamePadBridgeArgs, slots) +
        (intptr_t)slot_idx * (intptr_t)sizeof(GamePadBridgeSlot);
    struct {
        uint32_t active;
        uint32_t is_dualsense;
        uint32_t is_simulated;
        uint32_t direct_seq;
        uint32_t direct_lease;
    } slot_pub;
    slot_pub.active = 0u;
    slot_pub.is_dualsense = 0u;
    slot_pub.is_simulated = 0u;
    slot_pub.direct_seq = 0u;
    slot_pub.direct_lease = 0u;
    return game_bridge_process_write(
        game_pid, slot_base + (intptr_t)offsetof(GamePadBridgeSlot, active),
        &slot_pub, sizeof(slot_pub));
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
    /* Recovery neutralizes the installed physical primary. Discovery order
     * does not authorize changing slot zero, which may be a native DS5. */
    if (!pad_data || pad_data_len != sizeof(ScePadData) ||
        game_pid != g_game_bridge_direct_pid ||
        args_kaddr != g_game_bridge_direct_args ||
        wireless_ds4_game_bridge_image_state(game_pid) != 0)
        return -1;
    GamePadBridgeArgs args;
    if (game_bridge_copy_args(game_pid, args_kaddr, &args) != 0 ||
        args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
        args.reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1 ||
        args.pad_size != POORDS4_GAME_BRIDGE_PAD_SIZE)
        return -1;
    unsigned primary = args.reserved0;
    if (args.slots[0].reserved[2] == POORDS4_CLIENT_BINDING_MAGIC) {
        primary = POORDS4_MAX_SLOTS;
        for (unsigned slot = 0; slot < POORDS4_MAX_SLOTS; ++slot)
            if (args.pad_handle > 0 && args.slots[slot].pad_handle == args.pad_handle) {
                if (primary != POORDS4_MAX_SLOTS) return -1;
                primary = slot;
            }
    }
    if (primary >= POORDS4_MAX_SLOTS) return -1;
    if (args.pad_handle <= 0 || args.slots[primary].pad_handle != args.pad_handle ||
        args.slots[primary].is_dualsense || args.slots[primary].is_simulated)
        return -1;

    /* Grace/recovery frames release held controls without announcing a new
     * connection generation or rewinding the established sample timestamp.
     * This supervisor is the sole direct-frame writer. The validated argument
     * snapshot therefore contains its last published primary buffer. Real
     * source input uses update_slot and adopts its own new generation. */
    ScePadData neutral;
    memcpy(&neutral, pad_data, sizeof(neutral));
    const GamePadBridgeSlot *slot = &args.slots[primary];
    if (slot->direct_seq != 0) {
        ScePadData previous;
        memcpy(&previous, slot->direct_pad_data[slot->direct_seq & 1u],
               sizeof(previous));
        if (previous.connected) {
            neutral.count = previous.count;
            neutral.timestamp = previous.timestamp;
        }
    }
    return wireless_ds4_game_bridge_update_slot(
        game_pid, args_kaddr, primary, &neutral, sizeof(neutral), 0, 0);
#endif
}

int
wireless_ds4_game_bridge_status(pid_t game_pid, intptr_t args_kaddr,
                                PoorDS4GameBridgeStatus *out_status)
{
    if (out_status) memset(out_status, 0, sizeof(*out_status));
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_kaddr; (void)out_status;
    return -1;
#else
    if (game_pid <= 0 || !args_kaddr || !out_status)
        return -1;
    if (wireless_ds4_game_bridge_image_state(game_pid) != 0)
        return -1;
    GamePadBridgeArgs args;
    memset(&args, 0, sizeof(args));
    int stable_snapshot = 0;
    int primary_slot = -1;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint32_t seq_after = 0;
        if (game_bridge_copy_args(
                game_pid, args_kaddr, &args) != 0)
            return -1;
        if (args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
            args.reserved1 != POORDS4_GAME_BRIDGE_LAYOUT_V1)
            return -1;
        primary_slot=-1;
        for(unsigned slot=0;slot<POORDS4_MAX_SLOTS;slot++)
            if((args.pad_handle>0 && args.slots[slot].pad_handle==args.pad_handle) ||
               (args.slots[slot].reserved[0] && !args.slots[slot].is_dualsense)) {
                primary_slot=(int)slot;break;
            }
        int slot_publication=primary_slot>=0 && args.slots[primary_slot].direct_packets!=0;
        intptr_t seq_address = args_kaddr + (slot_publication
            ? (intptr_t)offsetof(GamePadBridgeArgs,slots)+(intptr_t)primary_slot*sizeof(GamePadBridgeSlot)+
                (intptr_t)offsetof(GamePadBridgeSlot,direct_seq)
            : (intptr_t)offsetof(GamePadBridgeArgs,direct_seq));
        if (remote_reader_copyout(
                game_pid, seq_address,
                &seq_after, sizeof(seq_after)) != 0)
            return -1;
        if ((slot_publication?args.slots[primary_slot].direct_seq:args.direct_seq) == seq_after) {
            stable_snapshot = 1;
            break;
        }
    }
    (void)stable_snapshot;
    if (args.magic != POORDS4_GAME_BRIDGE_MAGIC ||
        args.pad_size != POORDS4_GAME_BRIDGE_PAD_SIZE ||
        args.remote_block <= 0 || args.remote_block_size == 0)
        return -1;
    ScePadData pad;
    int slot_publication=primary_slot>=0 && args.slots[primary_slot].direct_packets!=0;
    memcpy(&pad, slot_publication
           ? args.slots[primary_slot].direct_pad_data[args.slots[primary_slot].direct_seq&1u]
           : args.direct_pad_data[args.direct_seq & 1u],
           sizeof(pad));
    int has_trigger_import=0;
    int has_device_class_import=0;
    int has_effect_set_import=0;
    for(unsigned index=0;index<args.import_hook_count &&
        index<POORDS4_GAME_BRIDGE_MAX_IMPORT_HOOKS;index++)
        {
            if(args.import_hook_kinds[index]==7u)has_trigger_import=1;
            if(args.import_hook_kinds[index]==11u)has_device_class_import=1;
            if(args.import_hook_kinds[index]==10u)has_effect_set_import=1;
        }
    uint64_t mapping_start=(uint64_t)args.remote_block;
    uint64_t mapping_end=mapping_start+args.remote_block_size;
    uint64_t argument=(uint64_t)args_kaddr;
    if (args.slots[0].reserved[2] == POORDS4_CLIENT_BINDING_MAGIC &&
        mapping_end >= mapping_start && argument >= mapping_start &&
        argument <= mapping_end && sizeof(GamePadBridgeTriggerArgs) <= mapping_end-argument) {
        GamePadBridgeTriggerArgs extension;
        const size_t offset = offsetof(GamePadBridgeTriggerArgs, client_binding_magic);
        if (!remote_reader_copyout(game_pid, args_kaddr+(intptr_t)offset,
                (uint8_t *)&extension+offset, sizeof(extension)-offset) &&
            extension.client_binding_magic == POORDS4_CLIENT_BINDING_MAGIC) {
            out_status->client_binding_available = 1;
            out_status->client_bindings = extension.client_bindings;
            out_status->client_native_rejects = extension.client_native_rejects;
            out_status->client_missing = extension.client_missing;
            out_status->consumer_samples = extension.consumer_samples;
            out_status->consumer_disconnected = extension.consumer_disconnected;
            out_status->consumer_output = extension.consumer_output;
            out_status->consumer_handle = extension.consumer_handle;
            out_status->consumer_result = extension.consumer_result;
            out_status->consumer_connected = extension.consumer_connected;
            out_status->consumer_count = extension.consumer_count;
        }
    }
    if(has_trigger_import && mapping_end>=mapping_start && argument>=mapping_start &&
       argument<=mapping_end && sizeof(GamePadBridgeTriggerArgs)<=mapping_end-argument) {
        GamePadBridgeTriggerArgs extension;
        memset(&extension,0,sizeof extension);
        const size_t offset=offsetof(GamePadBridgeTriggerArgs,magic);
        if(!remote_reader_copyout(game_pid,args_kaddr+(intptr_t)offset,
            (uint8_t*)&extension+offset,offsetof(GamePadBridgeTriggerArgs,rumble)-offset) &&
           extension.magic==POORDS4_TRIGGER_ARGS_MAGIC) {
            out_status->trigger_state_available=1;
            out_status->trigger_state_calls=extension.calls;
            out_status->trigger_state_overrides=extension.neutral_overrides;
            out_status->trigger_state_native_result=extension.last_native_result;
        }
    }
    if(has_device_class_import && mapping_end>=mapping_start && argument>=mapping_start &&
       argument<=mapping_end && sizeof(GamePadBridgeTriggerArgs)<=mapping_end-argument) {
        GamePadBridgeTriggerArgs extension;
        const size_t offset=offsetof(GamePadBridgeTriggerArgs,device_class_diag_magic);
        if(!remote_reader_copyout(game_pid,args_kaddr+(intptr_t)offset,
            (uint8_t*)&extension+offset,sizeof extension-offset) &&
            extension.device_class_diag_magic==POORDS4_DEVICE_CLASS_DIAG_MAGIC) {
            out_status->device_class_available=1;
            out_status->device_class_calls=extension.device_class_calls;
            out_status->device_class_errors=extension.device_class_errors;
            out_status->device_class_matched_calls=extension.device_class_matched_calls;
            out_status->device_class_last_result=extension.device_class_last_result;
            out_status->device_class_last_handle=extension.device_class_last_handle;
            out_status->device_class_last_slot=extension.device_class_last_slot;
            out_status->device_class_last_value=extension.device_class_last_value;
        }
    }
    out_status->layout_marker = args.reserved1;
    if(has_effect_set_import && mapping_end>=mapping_start && argument>=mapping_start &&
       argument<=mapping_end && sizeof(GamePadBridgeTriggerArgs)<=mapping_end-argument) {
        GamePadBridgeTriggerArgs extension;
        const size_t offset=offsetof(GamePadBridgeTriggerArgs,fp_set_trigger_effect);
        if(!remote_reader_copyout(game_pid,args_kaddr+(intptr_t)offset,
            (uint8_t*)&extension+offset,offsetof(GamePadBridgeTriggerArgs,effects)-offset) &&
            extension.effect_magic==POORDS4_EFFECT_ARGS_MAGIC) {
            out_status->trigger_effect_available=1;
            out_status->trigger_effect_set_calls=extension.effect_set_calls;
            out_status->trigger_effect_set_accepts=extension.effect_set_accepts;
            out_status->trigger_effect_state_overrides=extension.effect_state_overrides;
            out_status->trigger_effect_native_result=extension.effect_native_result;
            const size_t diag=offsetof(GamePadBridgeTriggerArgs,effect_diag_magic);
            if(!remote_reader_copyout(game_pid,args_kaddr+(intptr_t)diag,
                (uint8_t*)&extension+diag,sizeof extension-diag) &&
                extension.effect_diag_magic==POORDS4_EFFECT_DIAG_MAGIC){
                out_status->trigger_effect_diag_available=1;
                out_status->trigger_effect_last_skip=extension.effect_last_skip;
                out_status->trigger_effect_last_slot=extension.effect_last_slot;
                out_status->trigger_effect_last_call_handle=extension.effect_last_call_handle;
                out_status->trigger_effect_last_mask=extension.effect_last_mask;
                out_status->trigger_effect_last_left_mode=extension.effect_last_left_mode;
                out_status->trigger_effect_last_right_mode=extension.effect_last_right_mode;
                out_status->trigger_effect_compat_successes=extension.effect_compat_successes;
                out_status->trigger_effect_cached_unavailable=extension.effect_cached_unavailable;
                out_status->trigger_effect_legacy_receiver_active=args.active;
                for(unsigned reason=0;reason<9;reason++)
                    out_status->trigger_effect_skip_counts[reason]=extension.effect_skip_counts[reason];
            }
        }
    }
    if(mapping_end>=mapping_start && argument>=mapping_start && argument<=mapping_end &&
       sizeof(GamePadBridgeTriggerArgs)<=mapping_end-argument) {
        PoorDS4GameOutput output;
        if(!remote_reader_copyout(game_pid,args_kaddr+offsetof(GamePadBridgeTriggerArgs,rumble),
            &output,sizeof output) && output.magic==POORDS4_GAME_OUTPUT_MAGIC &&
            output.version==1 && output.size==sizeof output && output.slots==4) {
            out_status->vibration_available=1;
            out_status->vibration_calls=output.vibration_calls;
            out_status->vibration_mode_calls=output.mode_calls;
            out_status->vibration_publications=output.publications;
            out_status->vibration_native_calls=output.native_calls;
            out_status->vibration_failures=output.failed_publications;
            out_status->vibration_last_caller=output.last_caller;
            out_status->vibration_native_result=output.last_native_result;
            out_status->vibration_last_mode=output.last_mode;
        }
    }
    out_status->active = slot_publication ? args.slots[primary_slot].active : args.direct_active;
    out_status->seq = slot_publication ? args.slots[primary_slot].direct_seq : args.direct_seq;
    out_status->pad_handle = args.pad_handle;
    out_status->bridge_ready = 1;
    out_status->published_packets = slot_publication ? args.slots[primary_slot].direct_packets : args.direct_packets;
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
    out_status->native_passthrough_frames =
        args.native_passthrough_frames;
    out_status->native_connected_frames = args.native_connected_frames;
    out_status->direct_fallback_frames = args.direct_fallback_frames;
    out_status->direct_active_fallbacks = args.direct_active_fallbacks;
    out_status->last_native_result = args.last_native_result;
    out_status->native_success_frames = args.native_success_frames;
    out_status->native_input_activity_frames =
        args.native_input_activity_frames;
    out_status->last_native_timestamp = args.last_native_timestamp;
    out_status->last_native_buttons = args.last_native_buttons;
    out_status->last_native_lx = args.last_native_lx;
    out_status->last_native_ly = args.last_native_ly;
    out_status->last_native_rx = args.last_native_rx;
    out_status->last_native_ry = args.last_native_ry;
    out_status->last_native_l2 = args.last_native_l2;
    out_status->last_native_r2 = args.last_native_r2;
    out_status->last_native_connected = args.last_native_connected;
    out_status->last_native_count = args.last_native_count;
    out_status->import_hook_count = args.import_hook_count;
    /* The supervisor publishes by bridge slot, not the Sony client index.
     * Higher client indexes can occupy slot zero in the existing initializer. */
    /* This is a supervisor-local snapshot: never dereference its remote table
     * pointer through a copied game callback helper. */
    out_status->game_pad_index = primary_slot;
    out_status->buttons = pad.buttons;
    out_status->connected = pad.connected;
    out_status->reset_combo_ticks = args.reset_combo_ticks;
    out_status->reset_requested = args.reset_requested;
    out_status->last_caller_handle = args.last_caller_handle;
    out_status->last_mismatched_handle = args.last_mismatched_handle;
    out_status->handle_match_calls = args.handle_match_calls;
    out_status->handle_mismatch_calls = args.handle_mismatch_calls;
    out_status->read_zero_returns = args.read_zero_returns;
    out_status->read_nonzero_returns = args.read_nonzero_returns;
    out_status->empty_read_diag_available = args.legacy_receiver_port == POORDS4_EMPTY_READ_DIAG_MAGIC;
    if (out_status->empty_read_diag_available) {
        out_status->empty_read_buffer_restores = args.legacy_receiver_packets;
        out_status->empty_read_native_disconnected = args.legacy_receiver_timeouts;
    }
    out_status->max_read_streak = args.max_read_streak;
    for (unsigned i = 0; i < 4u; ++i) {
        out_status->observed_handles[i] = args.observed_handles[i];
        out_status->observed_handle_calls[i] = args.observed_handle_calls[i];
    }
    out_status->event_ring_head = args.event_ring_head;
    memcpy(out_status->event_ring, (const void *)args.event_ring,
           sizeof(out_status->event_ring));
    for (unsigned s = 0; s < POORDS4_MAX_SLOTS; ++s) {
        out_status->slots[s].pad_handle = args.slots[s].pad_handle;
        out_status->slots[s].pad_index = args.slots[s].pad_index;
        out_status->slots[s].active = args.slots[s].active;
        out_status->slots[s].is_dualsense = args.slots[s].is_dualsense;
        out_status->slots[s].is_simulated = args.slots[s].is_simulated;
        out_status->slots[s].user_matches = (uint8_t)(args.slots[s].reserved[0] & 0xff);
        out_status->slots[s].seq = args.slots[s].direct_seq;
        out_status->slots[s].packets = args.slots[s].direct_packets;
        out_status->slots[s].read_state_calls = args.slots[s].read_state_calls;
        out_status->slots[s].read_calls = args.slots[s].read_calls;
        out_status->slots[s].direct_fallback_frames = args.slots[s].direct_fallback_frames;
        out_status->slots[s].native_passthrough_frames = args.slots[s].native_passthrough_frames;
        ScePadData slot_pad;
        memcpy(&slot_pad, args.slots[s].direct_pad_data[args.slots[s].direct_seq & 1u], sizeof(slot_pad));
        out_status->slots[s].buttons = slot_pad.buttons;
        out_status->slots[s].lx = slot_pad.leftStick.x;
        out_status->slots[s].ly = slot_pad.leftStick.y;
        out_status->slots[s].rx = slot_pad.rightStick.x;
        out_status->slots[s].ry = slot_pad.rightStick.y;
        out_status->slots[s].l2 = slot_pad.analogButtons.l2;
        out_status->slots[s].r2 = slot_pad.analogButtons.r2;
        out_status->slots[s].connected = slot_pad.connected;
    }
    return 0;
#endif
}

int
wireless_ds4_game_bridge_check_reset(pid_t game_pid, intptr_t args_address)
{
#if !defined(__PROSPERO__)
    (void)game_pid; (void)args_address;
    return -1;
#else
    if (game_pid <= 0 || !args_address)
        return -1;
    if (wireless_ds4_game_bridge_image_state(game_pid) != 0)
        return -1;
    uint32_t reset_val = 0;
    intptr_t reset_addr = args_address +
        (intptr_t)offsetof(GamePadBridgeArgs, reset_requested);
    if (remote_reader_copyout(
            game_pid, reset_addr, &reset_val, sizeof(reset_val)) == 0) {
        return reset_val != 0 ? 1 : 0;
    }
    return -1;
#endif
}

int
wireless_ds4_game_bridge_ack_reset(pid_t game_pid, intptr_t args_address)
{
#if !defined(__PROSPERO__)
    (void)game_pid;(void)args_address;return -1;
#else
    GamePadBridgeArgs args;
    if(game_pid!=g_game_bridge_direct_pid || args_address!=g_game_bridge_direct_args ||
       wireless_ds4_game_bridge_image_state(game_pid)!=0 ||
       game_bridge_copy_args(game_pid,args_address,&args) ||
       args.magic!=POORDS4_GAME_BRIDGE_MAGIC ||
       args.reserved1!=POORDS4_GAME_BRIDGE_LAYOUT_V1)return -1;
    const uint32_t zero=0;
    const uint8_t ticks[POORDS4_MAX_SLOTS]={0};
    if(game_bridge_process_write(game_pid,args_address+(intptr_t)offsetof(GamePadBridgeArgs,reset_requested),&zero,sizeof zero) ||
       game_bridge_process_write(game_pid,args_address+(intptr_t)offsetof(GamePadBridgeArgs,reset_combo_ticks),&zero,sizeof zero) ||
       game_bridge_process_write(game_pid,args_address+(intptr_t)offsetof(GamePadBridgeArgs,reset_ticks),ticks,sizeof ticks))return -1;
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
    if (wireless_ds4_game_bridge_image_state(game_pid) > 0)
        return wireless_ds4_game_bridge_abandon();
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
                transport_args.import_hook_kinds[index] >= POORDS4_GAME_BRIDGE_API_COUNT ||
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
        poords4_game_process_watch_close(&g_game_process_watch);
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
remote_pad_identity(pid_t pid, intptr_t libpad_base,
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

    const uint32_t strides[] = {
        POORDS4_PAD_CLIENT_STRIDE_1160,
        POORDS4_PAD_CLIENT_STRIDE_0602
    };
    for (unsigned s = 0; s < 2u; ++s) {
        uint32_t stride = strides[s];
        for (unsigned slot = 0; slot < 16; ++slot) {
            intptr_t entry = libpad_base + POORDS4_PAD_CLIENT_TABLE_1160 +
                (intptr_t)slot * stride;
            uint32_t candidate_handle = 0;
            if (remote_reader_copyout(
                    pid, entry + POORDS4_PAD_CLIENT_HANDLE_1160,
                    &candidate_handle, sizeof(candidate_handle)) != 0)
                break;
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
                break;

            *out_connected = connected;
            *out_vendor = vendor;
            *out_product = product;
            return 0;
        }
    }
    return -1;
#endif
}

static int __attribute__((unused))
remote_pad_identity_1160(pid_t pid, intptr_t libpad_base,
                         int32_t pad_handle, int32_t *out_connected,
                         uint16_t *out_vendor, uint16_t *out_product)
{
    return remote_pad_identity(
        pid, libpad_base, pad_handle, out_connected, out_vendor, out_product);
}

static int
remote_pad_is_known_ds4(int32_t connected, uint16_t vendor,
                        uint16_t product)
{
    if (!connected || vendor != UINT16_C(0x054c))
        return 0;
    /* Explicitly exclude DualSense / DualSense Edge product IDs */
    if (product == UINT16_C(0x0ce6) || product == UINT16_C(0x0df2))
        return 0;
    return product == UINT16_C(0x05c4) ||
           product == UINT16_C(0x09cc) ||
           product == UINT16_C(0x0ba0) ||
           product == 0;
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
game_cache_validate_table_candidate(
    pid_t target, intptr_t base, uint64_t mapsize, intptr_t candidate,
    uint32_t stride,
    int32_t source_user_id, int32_t source_pad_index,
    unsigned *out_entries, unsigned *out_identity)
{
#if !defined(__PROSPERO__)
    (void)target; (void)base; (void)mapsize; (void)candidate; (void)stride;
    (void)source_user_id; (void)source_pad_index;
    (void)out_entries; (void)out_identity;
    return 0;
#else
    if (out_entries) *out_entries = 0;
    if (out_identity) *out_identity = 0;
    if (stride < 0x200u)
        return 0;
    const uint64_t required =
        UINT64_C(23) * stride +
        POORDS4_PAD_CLIENT_PRODUCT_1160 + sizeof(uint16_t);
    if (candidate < base || (uint64_t)(candidate - base) >= mapsize ||
        mapsize - (uint64_t)(candidate - base) < required)
        return 0;

    unsigned entries = 0;
    unsigned identity = 0;
    unsigned user_matches = 0;
    for (unsigned slot = 0; slot < 24u; ++slot) {
        intptr_t entry = candidate + (intptr_t)slot * stride;
        int32_t connected = 0;
        int32_t handle = 0;
        int32_t user_id = -1;
        int32_t valid = 0;
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
                target, entry + 0x38, &valid, sizeof(valid)) != 0)
            return 0;
        if (handle <= 0)
            continue;
        int32_t index = handle & 0xff;
        if (index < 0 || index >= 24 || user_id < 0 ||
            (connected != 0 && connected != 1) ||
            (valid != 0 && valid != 1))
            return 0;
        entries++;
        if (user_id == source_user_id)
            user_matches++;
        if (user_id == source_user_id && index == source_pad_index)
            identity++;
    }
    if (out_entries) *out_entries = entries;
    if (out_identity) *out_identity = identity;
    (void)user_matches;
    /* Two denotes a fully readable empty table, usable only with a code anchor. */
    return entries == 0u ? 2 : entries <= 24u;
#endif
}

static void
game_cache_report_user_hits(
    pid_t target, intptr_t base, uint64_t mapsize,
    int32_t source_user_id, int report_fd)
{
#if defined(__PROSPERO__)
    uint64_t scan_size = mapsize;
    if (scan_size > UINT64_C(0x00100000))
        scan_size = UINT64_C(0x00100000);
    uint8_t block[0x1000];
    unsigned hits = 0;
    unsigned read_failures = 0;
    uint64_t offset = 0;
    while (offset < scan_size && hits < 32u) {
        size_t length = sizeof(block);
        if ((uint64_t)length > scan_size - offset)
            length = (size_t)(scan_size - offset);
        if (mdbg_copyout(
                target, base + (intptr_t)offset,
                block, length) != 0) {
            read_failures++;
        } else {
            for (size_t index = 0; index + sizeof(source_user_id) <= length;
                 ++index) {
                int32_t value = -1;
                memcpy(&value, block + index, sizeof(value));
                if (value != source_user_id)
                    continue;
                intptr_t address = base + (intptr_t)offset +
                    (intptr_t)index;
                report_printf(
                    report_fd,
                    "client_user_hit index=%u address=0x%lx "
                    "offset=0x%lx\n",
                    hits, (unsigned long)address,
                    (unsigned long)(address - base));
                intptr_t context = address - 64;
                if (context < base)
                    context = base;
                uint8_t bytes[128];
                if ((uint64_t)(context - base) + sizeof(bytes) <= mapsize &&
                    mdbg_copyout(
                        target, context, bytes, sizeof(bytes)) == 0) {
                    char name[64];
                    snprintf(name, sizeof(name),
                             "client_user_hit_%u_context_0000", hits);
                    game_bridge_report_prefix(
                        report_fd, name, bytes, 64u);
                    snprintf(name, sizeof(name),
                             "client_user_hit_%u_context_0040", hits);
                    game_bridge_report_prefix(
                        report_fd, name, bytes + 64u, 64u);
                }
                hits++;
                if (hits >= 32u)
                    break;
            }
        }
        if (length <= 3u)
            break;
        offset += length - 3u;
    }
    report_printf(
        report_fd,
        "client_user_scan bytes=0x%llx hits=%u read_failures=%u "
        "truncated=%d\n",
        (unsigned long long)scan_size, hits, read_failures,
        hits >= 32u);
#else
    (void)target; (void)base; (void)mapsize;
    (void)source_user_id; (void)report_fd;
#endif
}

/* Bind a table reference to Sony's actual handle lookup, not merely nearby
 * data that happens to contain a positive integer. The same lookup advances
 * by a client stride until its 24-entry span. */
static int
game_cache_table_reference_stride(const uint8_t *code, size_t size, size_t at)
{
    if (!code || at + 7u > size || code[at] < 0x40 || code[at] > 0x4f ||
        code[at + 1u] != 0x8d || (code[at + 2u] & 0xc7u) != 0x05u)
        return 0;
    unsigned table_reg = ((code[at + 2u] >> 3) & 7u) |
        ((code[at] & 4u) ? 8u : 0u);
    size_t lookup_end = at + 39u < size ? at + 39u : size;
    int handle_lookup = 0;
    for (size_t p = at + 7u; p + 4u <= lookup_end; ++p) {
        size_t op = p;
        uint8_t rex = 0;
        if (code[op] >= 0x40 && code[op] <= 0x4f) rex = code[op++];
        if (op + 4u > lookup_end || code[op] != 0x8b ||
            (code[op + 1u] & 0xc7u) != 0x44u ||
            code[op + 3u] != POORDS4_PAD_CLIENT_HANDLE_1160)
            continue;
        unsigned sib = code[op + 2u];
        unsigned base = (sib & 7u) | ((rex & 1u) ? 8u : 0u);
        unsigned index = ((sib >> 3) & 7u) | ((rex & 2u) ? 8u : 0u);
        if (base == table_reg ||
            ((sib >> 6) == 0u && index != 4u && index == table_reg)) {
            handle_lookup = 1;
            break;
        }
    }
    if (!handle_lookup) return 0;
    const uint32_t strides[] = {POORDS4_PAD_CLIENT_STRIDE_1160,
                                POORDS4_PAD_CLIENT_STRIDE_0602};
    const uint32_t spans[] = {POORDS4_PAD_CLIENT_SPAN_1160,
                              POORDS4_PAD_CLIENT_SPAN_0602};
    size_t end = at + 96u < size ? at + 96u : size;
    for (unsigned profile = 0; profile < 2; ++profile) {
        int stride_seen = 0, span_seen = 0;
        for (size_t p = at + 7u; p + 4u <= end; ++p) {
            uint32_t immediate;
            memcpy(&immediate, code + p, sizeof(immediate));
            stride_seen |= immediate == strides[profile];
            span_seen |= immediate == spans[profile];
        }
        if (stride_seen && span_seen) return (int)strides[profile];
    }
    return 0;
}

static int
game_cache_find_table(
    pid_t target, intptr_t base, uint64_t mapsize,
    intptr_t read_state, int32_t source_user_id,
    int32_t source_pad_index, int report_fd,
    intptr_t *out_table, uint32_t *out_stride)
{
#if !defined(__PROSPERO__)
    (void)target; (void)base; (void)mapsize; (void)read_state;
    (void)source_user_id; (void)source_pad_index; (void)report_fd;
    (void)out_table; (void)out_stride;
    return -1;
#else
    intptr_t internal = 0;
    if (!out_table || game_cache_follow_read_state(
            target, base, mapsize, read_state, &internal) != 0)
        return -1;
    *out_table = 0;
    if (out_stride)
        *out_stride = POORDS4_PAD_CLIENT_STRIDE_1160;
    if (internal < base || (uint64_t)(internal - base) >= mapsize)
        return -1;
    uint8_t code[0x800];
    size_t code_size = sizeof(code);
    uint64_t available = mapsize - (uint64_t)(internal - base);
    if ((uint64_t)code_size > available)
        code_size = (size_t)available;
    if (code_size < 32u ||
        mdbg_copyout(target, internal, code, code_size) != 0)
        return -1;

    unsigned stride_immediates = 0;
    unsigned span_immediates = 0;
    unsigned stride_548_immediates = 0;
    unsigned span_7ec0_immediates = 0;
    for (size_t offset = 0; offset + 4u <= code_size; ++offset) {
        uint32_t immediate = 0;
        memcpy(&immediate, code + offset, sizeof(immediate));
        if (immediate == POORDS4_PAD_CLIENT_STRIDE_1160)
            stride_immediates++;
        if (immediate == POORDS4_PAD_CLIENT_STRIDE_0602)
            stride_548_immediates++;
        if (immediate == POORDS4_PAD_CLIENT_SPAN_1160)
            span_immediates++;
        if (immediate == POORDS4_PAD_CLIENT_SPAN_0602)
            span_7ec0_immediates++;
    }
    report_printf(
        report_fd,
        "client_locator internal=0x%lx offset=0x%lx bytes=0x%zx "
        "fnv=0x%016llx stride_immediates=%u span_immediates=%u "
        "stride_548=%u span_7ec0=%u\n",
        (unsigned long)internal, (unsigned long)(internal - base),
        code_size,
        (unsigned long long)poords4_fnv1a64(code, code_size),
        stride_immediates, span_immediates,
        stride_548_immediates, span_7ec0_immediates);
    size_t dump_size = code_size < 0x400u ? code_size : 0x400u;
    for (size_t offset = 0; offset < dump_size; offset += 64u) {
        char name[64];
        snprintf(name, sizeof(name),
                 "client_locator_code_%04zx", offset);
        size_t length = dump_size - offset;
        if (length > 64u) length = 64u;
        game_bridge_report_prefix(
            report_fd, name, code + offset, length);
    }

    const uint32_t candidate_strides[2] = {
        (stride_548_immediates > 0 || span_7ec0_immediates > 0)
            ? POORDS4_PAD_CLIENT_STRIDE_0602 : POORDS4_PAD_CLIENT_STRIDE_1160,
        (stride_548_immediates > 0 || span_7ec0_immediates > 0)
            ? POORDS4_PAD_CLIENT_STRIDE_1160 : POORDS4_PAD_CLIENT_STRIDE_0602
    };

    static const int32_t adjustments[] = {
        0, -0x08, -0x10, -0x18, -0x1c, -0x20,
        -0x24, -0x28, -0x38, -0x40, -0x60, -0x4b8
    };
    intptr_t matches[16];
    uint32_t match_strides[16];
    unsigned match_count = 0;
    unsigned ref_count = 0;
    unsigned anchored_refs = 0;
    for (size_t offset = 0; offset + 7u <= code_size; ++offset) {
        int stride = game_cache_table_reference_stride(code, code_size, offset);
        if (!stride) continue;
        anchored_refs++;
        int32_t displacement;
        memcpy(&displacement, code + offset + 3u, sizeof(displacement));
        intptr_t table = internal + (intptr_t)offset + 7 + displacement;
        unsigned entries = 0, identity = 0;
        int valid = game_cache_validate_table_candidate(target, base, mapsize,
            table, (uint32_t)stride, source_user_id, source_pad_index,
            &entries, &identity);
        report_printf(report_fd,
            "client_locator_anchor address=0x%lx stride=0x%x entries=%u identity=%u valid=%d\n",
            (unsigned long)table, (unsigned)stride, entries, identity, valid);
        if (!valid) continue;
        int duplicate = 0;
        for (unsigned prior = 0; prior < match_count; ++prior)
            duplicate |= matches[prior] == table && match_strides[prior] == (uint32_t)stride;
        if (!duplicate && match_count < 16u) {
            matches[match_count] = table;
            match_strides[match_count++] = (uint32_t)stride;
        }
    }
    if (anchored_refs) {
        /* A proven empty table can be watched from the game's information
         * callback before its first controller is accepted. */
        if (match_count != 1u) {
            report_printf(report_fd, "client_locator_wait anchored=%u matches=%u\n",
                          anchored_refs, match_count);
            return -1;
        }
        *out_table = matches[0];
        if (out_stride) *out_stride = match_strides[0];
        return 0;
    }
    for (size_t offset = 0; offset + 7u <= code_size; ++offset) {
        uint8_t rex = code[offset];
        uint8_t opcode = code[offset + 1u];
        uint8_t modrm = code[offset + 2u];
        if (rex < 0x40 || rex > 0x4f ||
            (opcode != 0x8d && opcode != 0x8b) ||
            (modrm & 0xc7u) != 0x05u)
            continue;
        int32_t displacement = 0;
        memcpy(&displacement, code + offset + 3u, sizeof(displacement));
        intptr_t reference = internal + (intptr_t)offset + 7 +
            (intptr_t)displacement;
        intptr_t indirect = 0;
        if (opcode == 0x8b) {
            uint64_t pointer = 0;
            if (reference > 0 && mdbg_copyout(
                    target, reference, &pointer, sizeof(pointer)) == 0 &&
                pointer <= (uint64_t)INTPTR_MAX)
                indirect = (intptr_t)pointer;
        }
        if (ref_count < 64u) {
            report_printf(
                report_fd,
                "client_locator_ref index=%u insn=0x%zx opcode=0x%02x "
                "modrm=0x%02x reference=0x%lx offset=0x%lx "
                "indirect=0x%lx\n",
                ref_count, offset, opcode, modrm,
                (unsigned long)reference,
                reference >= base
                    ? (unsigned long)(reference - base) : 0ul,
                (unsigned long)indirect);
        }
        ref_count++;

        intptr_t roots[2] = {reference, indirect};
        for (unsigned root_index = 0; root_index < 2u; ++root_index) {
            if (roots[root_index] <= 0)
                continue;
            for (unsigned adjustment_index = 0;
                 adjustment_index <
                    sizeof(adjustments) / sizeof(adjustments[0]);
                 ++adjustment_index) {
                intptr_t candidate = roots[root_index] +
                    (intptr_t)adjustments[adjustment_index];
                for (unsigned s_idx = 0; s_idx < 2u; ++s_idx) {
                    uint32_t stride_cand = candidate_strides[s_idx];
                    unsigned entries = 0;
                    unsigned identity = 0;
                    int valid = game_cache_validate_table_candidate(
                        target, base, mapsize, candidate, stride_cand,
                        source_user_id, source_pad_index,
                        &entries, &identity);
                    if (entries > 0u || identity > 0u || valid) {
                        report_printf(
                            report_fd,
                            "client_locator_candidate address=0x%lx "
                            "offset=0x%lx stride=0x%x root=%s adjustment=%d "
                            "entries=%u identity=%u valid=%d\n",
                            (unsigned long)candidate,
                            candidate >= base
                                ? (unsigned long)(candidate - base) : 0ul,
                            stride_cand,
                            root_index == 0u ? "direct" : "indirect",
                            adjustments[adjustment_index], entries,
                            identity, valid);
                    }
                    if (valid != 1)
                        continue;
                    int duplicate = 0;
                    for (unsigned prior = 0; prior < match_count; ++prior) {
                        if (matches[prior] == candidate) {
                            duplicate = 1;
                            break;
                        }
                    }
                    if (!duplicate && match_count <
                            sizeof(matches) / sizeof(matches[0])) {
                        matches[match_count] = candidate;
                        match_strides[match_count] = stride_cand;
                        match_count++;
                    }
                    break;
                }
            }
        }
    }
    report_printf(
        report_fd,
        "client_locator_summary refs=%u matches=%u\n",
        ref_count, match_count);
    if (match_count == 1u) {
        *out_table = matches[0];
        if (out_stride)
            *out_stride = match_strides[0];
        return 0;
    }
    if (match_count > 1u) {
        unsigned best_identity = 0;
        int best_idx = 0;
        for (unsigned idx = 0; idx < match_count; ++idx) {
            unsigned entries = 0, ident = 0;
            if (game_cache_validate_table_candidate(
                    target, base, mapsize, matches[idx], match_strides[idx],
                    source_user_id, source_pad_index, &entries, &ident) &&
                ident > best_identity) {
                best_identity = ident;
                best_idx = (int)idx;
            }
        }
        report_printf(
            report_fd,
            "client_locator_disambiguated match=%u best=%d identity=%u\n",
            match_count, best_idx, best_identity);
        *out_table = matches[best_idx];
        if (out_stride)
            *out_stride = match_strides[best_idx];
        return 0;
    }
    report_printf(report_fd, "client_locator_error=no_valid_table\n");
    game_cache_report_user_hits(
        target, base, mapsize, source_user_id, report_fd);
    return -1;
#endif
}
