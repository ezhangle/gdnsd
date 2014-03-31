/* Copyright © 2014 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd.
 *
 * gdnsd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include "dmn.h"

/***********************************************************
***** Defines **********************************************
***********************************************************/

// Since we explicitly lock the output stream,
//   these are merely for performance if they exist...
#if ! HAVE_DECL_FPUTS_UNLOCKED
#  define fputs_unlocked fputs
#endif
#if ! HAVE_DECL_FFLUSH_UNLOCKED
#  define fflush_unlocked fflush
#endif

// These control the growth of the log formatting-buffer space
// These define the buffer count, size of first buffer, and shift
//   value sets how fast the buffer sizes grow
// At these settings (4, 10, 2), the buffer sizes are:
//   1024, 4096, 16384, 65536
#define FMTBUF_CT     4U
#define FMTBUF_START 10U
#define FMTBUF_STEP   2U

/***********************************************************
***** Constants ********************************************
***********************************************************/

// Log message prefixes when using stderr
static const char PFX_DEBUG[] = " debug: ";
static const char PFX_INFO[] = " info: ";
static const char PFX_WARNING[] = " warning: ";
static const char PFX_ERR[] = " error: ";
static const char PFX_CRIT[] = " fatal: ";
static const char PFX_UNKNOWN[] = " ???: ";

// pidbuf len
static const size_t PIDBUF_LEN = 22U;

// Max length of an errno string (for our buffer purposes)
static const size_t DMN_ERRNO_MAXLEN = 256U;

// Standard file-permissions constants
static const mode_t PERMS755   = (S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
static const mode_t PERMS644   = (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
static const mode_t PERMS_MASK = (S_IRWXU|S_IRWXG|S_IRWXO);

// These are phases used to enforce a strict ordering
//   of operations and dependencies between the functions in this file
// PHASE0_UNINIT is the default at library load, and the
//   code only allows forward, serial movement through this list
//   one entry at a time.
// Note that in PHASE0_UNINIT, *nothing* is valid to call except
//   dmn_init1(), including all of the log/assert functions.
typedef enum {
    PHASE0_UNINIT      = 0,
    PHASE1_INIT1,
    PHASE2_INIT2,
    PHASE3_INIT3,
    PHASE4_FORKED,
    PHASE5_SECURED,
    PHASE6_PIDLOCKED,
    PHASE7_FINISHED,
} phase_t;

// the functions which move the state forward
//   to each of the above phases, for use in BUG output
static const char* phase_actor[] = {
    NULL,
    "dmn_init1()",
    "dmn_init2()",
    "dmn_init3()",
    "dmn_fork()",
    "dmn_secure()",
    "dmn_acquire_pidfile()",
    "dmn_finish()",
};


/***********************************************************
***** Static per-thread data *******************************
***********************************************************/

// This is our log-formatting buffer.  It holds multiple buffers
//   of increasing size (see constants) above which are allocated
//   per-thread as-needed, permanently for the life of the thread.
typedef struct {
    unsigned used[FMTBUF_CT];
    char* bufs[FMTBUF_CT];
} fmtbuf_t;

static __thread fmtbuf_t fmtbuf = {{0},{NULL}};

/***********************************************************
***** Static process-global data ***************************
***********************************************************/

typedef struct {
    // directly supplied by caller
    bool  debug;
    bool  foreground;
    bool  stderr_info;
    bool  restart;
    char* name;
    char* username;
    char* chroot;

    // calculated/inferred/discovered
    bool  invoked_as_root; // !geteuid() during init2()
    bool  will_privdrop;   // invoked_as_root && non-null username from init3()
    bool  will_chroot;     // invoked_as_root && non-null chroot from init2(), implies will_privdrop
    bool  need_helper;     // depends on foreground, will_privdrop, and pcall registration - set in _fork
    uid_t uid;             // uid of username from init3()
    gid_t gid;             // gid of username from init3()
    char* pid_dir_pre_chroot;   // depends on chroot + pid_dir
    char* pid_file_pre_chroot;  // depends on chroot + pid_dir + name
    char* pid_file_post_chroot; // depends on chroot + pid_dir + name
} params_t;

static params_t params = {
    .debug           = false,
    .foreground      = false,
    .stderr_info     = true,
    .restart         = false,
    .name            = NULL,
    .username        = NULL,
    .chroot          = NULL,
    .invoked_as_root = false,
    .will_privdrop   = false,
    .will_chroot     = false,
    .need_helper     = false,
    .uid             = 0,
    .gid             = 0,
    .pid_dir_pre_chroot   = NULL,
    .pid_file_pre_chroot  = NULL,
    .pid_file_post_chroot = NULL,
};

typedef struct {
    phase_t phase;
    bool    syslog_alive;
    int     fd_to_helper;
    int     fd_from_helper;
    FILE*   stderr_out;
} state_t;

static state_t state = {
    .phase            = PHASE0_UNINIT,
    .syslog_alive     = false,
    .fd_to_helper     = -1,
    .fd_from_helper   = -1,
    .stderr_out       = NULL,
};

// pcall funcptrs
static dmn_func_vv_t* pcalls = NULL;
static unsigned num_pcalls = 0;

/***********************************************************
***** API usage checks *************************************
***********************************************************/

#define phase_check(_after, _before, _unique) do { \
    if(state.phase == PHASE0_UNINIT) { \
        fprintf(stderr, "BUG: dmn_init1() must be called before any other libdmn function!\n"); \
        abort(); \
    } \
    if(_unique) {\
        static unsigned _call_count = 0; \
        if(++_call_count > 1) \
            dmn_log_fatal("BUG: %s can only be called once and was already called!", __func__); \
    }\
    if(_after && state.phase < _after) \
        dmn_log_fatal("BUG: %s must be called after %s", __func__, phase_actor[_after]); \
    if(_before && state.phase >= _before) \
        dmn_log_fatal("BUG: %s must be called before %s", __func__, phase_actor[_before]); \
} while(0);

/***********************************************************
***** Logging **********************************************
***********************************************************/

// Allocate a chunk from the format buffer
// Allocates the buffer itself on first use per-thread
char* dmn_fmtbuf_alloc(unsigned size) {
    phase_check(0, 0, 0);
    char* rv = NULL;

    unsigned bsize = 1U << FMTBUF_START;
    for(unsigned i = 0; i < FMTBUF_CT; i++) {
        if(!fmtbuf.bufs[i])
            fmtbuf.bufs[i] = malloc(bsize);
        if((bsize - fmtbuf.used[i]) >= size) {
            rv = &fmtbuf.bufs[i][fmtbuf.used[i]];
            fmtbuf.used[i] += size;
            break;
        }
        bsize <<= FMTBUF_STEP;
    }

    if(!rv)
        dmn_log_fatal("BUG: format buffer exhausted");
    return rv;
}

// Reset (free allocations within) the format buffer,
//  but do not trigger initial allocation in the process
void dmn_fmtbuf_reset(void) {
    phase_check(0, 0, 0);

    for(unsigned i = 0; i < FMTBUF_CT; i++)
        fmtbuf.used[i] = 0;
}

// dmn_strerror(), which hides GNU or POSIX strerror_r() thread-safe
//  errno->string translation behind a more strerror()-like interface
//  using dmn_fmtbuf_alloc()
const char* dmn_strerror(const int errnum) {
    phase_check(0, 0, 0);

    char tmpbuf[DMN_ERRNO_MAXLEN];
    const char* tmpbuf_ptr;

#ifdef STRERROR_R_CHAR_P
    // GNU-style
    tmpbuf_ptr = strerror_r(errnum, tmpbuf, DMN_ERRNO_MAXLEN);
#else
    // POSIX style (+ older glibc bug-compat)
    int rv = strerror_r(errnum, tmpbuf, DMN_ERRNO_MAXLEN);
    if(rv) {
        if(rv == EINVAL || (rv < 0 && errno == EINVAL))
            snprintf(tmpbuf, DMN_ERRNO_MAXLEN, "Invalid errno: %i", errnum);
        else
            dmn_log_fatal("strerror_r(,,%zu) failed", DMN_ERRNO_MAXLEN);
    }
    tmpbuf_ptr = tmpbuf;
#endif

    const unsigned len = strlen(tmpbuf_ptr) + 1;
    char* buf = dmn_fmtbuf_alloc(len);
    memcpy(buf, tmpbuf_ptr, len);
    return buf;
}

int dmn_log_get_stderr_out_fd(void) {
    phase_check(0, 0, 0);
    return fileno(state.stderr_out);
}

void dmn_log_set_stderr_out(const int fd) {
    phase_check(0, 0, 0);
    state.stderr_out = fdopen(fd, "w");
}

void dmn_log_close_stderr_out(void) {
    phase_check(0, 0, 0);
    fclose(state.stderr_out);
    state.stderr_out = NULL;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

void dmn_loggerv(int level, const char* fmt, va_list ap) {
    phase_check(0, 0, 0);

    if(state.stderr_out && (level != LOG_INFO || params.stderr_info)) {
        const char* pfx;
        switch(level) {
            case LOG_DEBUG: pfx = PFX_DEBUG; break;
            case LOG_INFO: pfx = PFX_INFO; break;
            case LOG_WARNING: pfx = PFX_WARNING; break;
            case LOG_ERR: pfx = PFX_ERR; break;
            case LOG_CRIT: pfx = PFX_CRIT; break;
            default: pfx = PFX_UNKNOWN; break;
        }

        va_list apcpy;
        va_copy(apcpy, ap);
        flockfile(state.stderr_out);
        fputs_unlocked(pfx, state.stderr_out);
        vfprintf(state.stderr_out, fmt, apcpy);
        va_end(apcpy);
        putc_unlocked('\n', state.stderr_out);
        fflush_unlocked(state.stderr_out);
        funlockfile(state.stderr_out);
    }

    if(state.syslog_alive)
        vsyslog(level, fmt, ap);

    dmn_fmtbuf_reset();
}

void dmn_logger(int level, const char* fmt, ...) {
    phase_check(0, 0, 0);
    va_list ap;
    va_start(ap, fmt);
    dmn_loggerv(level, fmt, ap);
    va_end(ap);
}

#pragma GCC diagnostic pop

bool dmn_get_debug(void) { phase_check(0, 0, 0); return params.debug; }
bool dmn_get_foreground(void) { phase_check(0, 0, 0); return params.foreground; }
const char* dmn_get_username(void) { phase_check(0, 0, 0); return params.username; }

/***********************************************************
***** Private subroutines used by daemonization ************
***********************************************************/

// The terminal signal SIGTERM is sent exactly once, then
//  the status of the daemon is polled repeatedly with timouts.
// Function returns when either the process is dead or
//  our timeouts all expired.  Total timeout is 15s in 100ms
//  increments.
static void terminate_pid_and_wait(pid_t pid) {
    dmn_assert(pid); // don't try to kill (0, ...)

    if(!kill(pid, SIGTERM)) {
        struct timeval tv;
        unsigned tries = 150;
        do {
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            select(0, NULL, NULL, NULL, &tv);
        } while(--tries && !kill(pid, 0));
    }
}

// the helper process executes here and does not return
static void helper_proc(const int readpipe, const int writepipe) {
    dmn_assert(state.phase == PHASE3_INIT3);
    dmn_assert(readpipe >= 0);
    dmn_assert(writepipe >= 0);

    int exitval = 1;
    uint8_t msg;
    int readrv;
    int writerv;

    do {
        do {
            errno = 0;
            readrv = read(readpipe, (char*)&msg, 1);
        } while(errno == EAGAIN);

        if(errno || readrv != 1)
            break; // pipe close or other error
        else if(msg >= 128U)
            break; // high-bit reserved for responses!
        else if(msg == 0U) // daemon success
            exitval = 0;
        else if(msg > 63U) // pcall
            pcalls[msg - 64U]();
        else
            break;
        errno = 0;
        msg |= 128U; // set high-bit for response
        writerv = write(writepipe, (char*)&msg, 1);
        if(errno || writerv != 1)
            break;
    } while(1);

    // _exit avoids any atexit that may have been installed before fork
    _exit(exitval);
}

// this isn't meant to be high-speed or elegant, but it saves
//   some repetitive string-mod code elsewhere.
static char* str_combine_n(const unsigned count, ...) {
    dmn_assert(count > 1);

    struct {
        const char* ptr;
        unsigned len;
    } strs[count];

    unsigned oal = 1; // for terminating NUL
    va_list ap;
    va_start(ap, count);
    for(unsigned i = 0; i < count; i++) {
        const char* s = va_arg(ap, char*);
        const unsigned l = strlen(s);
        strs[i].ptr = s;
        strs[i].len = l;
        oal += l;
    }
    va_end(ap);

    char* out = malloc(oal);
    char* cur = out;
    for(unsigned i = 0; i < count; i++) {
        memcpy(cur, strs[i].ptr, strs[i].len);
        cur += strs[i].len;
    }
    *cur = '\0';

    return out;
}

/***********************************************************
***** Daemonization ****************************************
***********************************************************/

void dmn_init1(bool debug, bool foreground, bool stderr_info, bool use_syslog, const char* name) {
    if(state.phase != PHASE0_UNINIT)
        dmn_log_fatal("BUG: dmn_init1() can only be called once!");

    // This lets us log to normal stderr for now
    state.stderr_out = stderr;

    if(!name)
        dmn_log_fatal("BUG: dmn_init1(): argument 'name' is *required*");

    params.debug = debug;
    params.foreground = foreground;
    params.stderr_info = stderr_info;
    params.name = strdup(name);

    if(!params.foreground) {
        FILE* stderr_copy = fdopen(dup(fileno(stderr)), "w");
        if(!stderr_copy)
            dmn_log_fatal("Failed to fdopen(dup(fileno(stderr))): %s", dmn_logf_errno());
        state.stderr_out = stderr_copy;
    }

    if(use_syslog) {
        openlog(params.name, LOG_NDELAY|LOG_PID, LOG_DAEMON);
        state.syslog_alive = true;
    }

    state.phase = PHASE1_INIT1;
}

void dmn_init2(const char* pid_dir, const char* chroot) {
    phase_check(PHASE1_INIT1, PHASE3_INIT3, 1);

    params.invoked_as_root = !geteuid();

    if(pid_dir && pid_dir[0] != '/')
        dmn_log_fatal("pid directory path must be absolute!");

    if(chroot) {
        if(chroot[0] != '/')
            dmn_log_fatal("chroot() path must be absolute!");
        struct stat st;
        if(lstat(chroot, &st))
            dmn_log_fatal("Cannot lstat(%s): %s", chroot, dmn_strerror(errno));
        if(!S_ISDIR(st.st_mode))
            dmn_log_fatal("chroot() path '%s' is not a directory!", chroot);
        params.chroot = strdup(chroot);
        if(params.invoked_as_root)
            params.will_chroot = true;
        if(pid_dir) {
            params.pid_dir_pre_chroot = str_combine_n(2, chroot, pid_dir);
            params.pid_file_pre_chroot = str_combine_n(5, chroot, pid_dir, "/", params.name, ".pid");
            if(params.invoked_as_root)
                params.pid_file_post_chroot = str_combine_n(4, pid_dir, "/", params.name, ".pid");
            else
                params.pid_file_post_chroot = params.pid_file_pre_chroot;
        }
    }
    else if(pid_dir) {
        params.pid_dir_pre_chroot = strdup(pid_dir);
        params.pid_file_pre_chroot
            = params.pid_file_post_chroot
                = str_combine_n(4, pid_dir, "/", params.name, ".pid");
    }

    state.phase = PHASE2_INIT2;
}

pid_t dmn_status(void) {
    phase_check(PHASE2_INIT2, PHASE6_PIDLOCKED, 0);

    const char* pidfile = state.phase < PHASE5_SECURED
        ? params.pid_file_pre_chroot
        : params.pid_file_post_chroot;

    if(!pidfile)
        return 0;

    const int pidfd = open(pidfile, O_RDONLY);
    if(pidfd == -1) {
        if (errno == ENOENT) return 0;
        else dmn_log_fatal("open() of pidfile '%s' failed: %s", pidfile, dmn_strerror(errno));
    }

    struct flock pidlock_info;
    memset(&pidlock_info, 0, sizeof(struct flock));
    pidlock_info.l_type = F_WRLCK;
    pidlock_info.l_whence = SEEK_SET;

    // should not fail unless something's horribly wrong
    if(fcntl(pidfd, F_GETLK, &pidlock_info))
        dmn_log_fatal("bug: fcntl(%s, F_GETLK) failed: %s", pidfile, dmn_strerror(errno));

    close(pidfd);

    if(pidlock_info.l_type == F_UNLCK) {
        dmn_log_debug("Found stale pidfile at %s, ignoring", pidfile);
        return 0;
    }

    return pidlock_info.l_pid;
}

pid_t dmn_stop(void) {
    phase_check(PHASE2_INIT2, PHASE6_PIDLOCKED, 0);

    const pid_t pid = dmn_status();
    if(!pid) {
        dmn_log_info("Did not find a running daemon to stop!");
        return 0;
    }

    terminate_pid_and_wait(pid);

    if(!kill(pid, 0)) {
        dmn_log_err("Cannot stop daemon at pid %li", (long)pid);
        return pid;
    }

    dmn_log_info("Daemon instance at pid %li stopped", (long)pid);
    return 0;
}

int dmn_signal(int sig) {
    phase_check(PHASE2_INIT2, PHASE6_PIDLOCKED, 0);

    int rv = 1; // error
    const pid_t pid = dmn_status();
    if(!pid) {
        dmn_log_err("Did not find a running daemon to signal!");
    }
    else if(kill(pid, sig)) {
        dmn_log_err("Cannot signal daemon at pid %li", (long)pid);
    }
    else {
        dmn_log_info("Signal %i sent to daemon instance at pid %li", sig, (long)pid);
        rv = 0; // success
    }

    return rv;
}

void dmn_init3(const char* username, const bool restart) {
    phase_check(PHASE2_INIT2, PHASE4_FORKED, 1);

    params.restart = restart;

    if(username)
        params.username = strdup(username);

    if(params.invoked_as_root) {
        if(username) {
            errno = 0;
            struct passwd* p = getpwnam(username);
            if(!p) {
                if(errno)
                    dmn_log_fatal("getpwnam('%s') failed: %s", username, dmn_strerror(errno));
                else
                    dmn_log_fatal("User '%s' does not exist", username);
            }
            if(!p->pw_uid || !p->pw_gid)
                dmn_log_fatal("User '%s' has root's uid and/or gid", username);
            params.uid = p->pw_uid;
            params.gid = p->pw_gid;
            params.will_privdrop = true;
        }
        else if(params.will_chroot) {
            dmn_log_fatal("must set privdrop username if using chroot");
        }
    }

    if(params.pid_dir_pre_chroot) {
        struct stat st;
        if(stat(params.pid_dir_pre_chroot, &st)) {
            if(mkdir(params.pid_dir_pre_chroot, 0755))
                dmn_log_fatal("pidfile directory %s does not exist and mkdir() failed with: %s", params.pid_dir_pre_chroot, dmn_strerror(errno));
            if(stat(params.pid_dir_pre_chroot, &st)) // reload st for privdrop below
                dmn_log_fatal("stat() of pidfile directory %s failed (post-mkdir): %s", params.pid_dir_pre_chroot, dmn_strerror(errno));
        }
        else if(!S_ISDIR(st.st_mode)) {
            dmn_log_fatal("pidfile directory %s is not a directory!", params.pid_dir_pre_chroot);
        }
        else if((st.st_mode & PERMS_MASK) != PERMS755) {
            if(chmod(params.pid_dir_pre_chroot, PERMS755))
                dmn_log_fatal("chmod('%s',%.4o) failed: %s", params.pid_dir_pre_chroot, PERMS755, dmn_strerror(errno));
        }

        // directory chown only applies in privdrop case
        if(params.will_privdrop) {
            if(st.st_uid != params.uid || st.st_gid != params.gid)
                if(chown(params.pid_dir_pre_chroot, params.uid, params.gid))
                    dmn_log_fatal("chown('%s',%u,%u) failed: %s", params.pid_dir_pre_chroot, params.uid, params.gid, dmn_strerror(errno));
        }

        if(!lstat(params.pid_file_pre_chroot, &st)) {
            if(!S_ISREG(st.st_mode))
                dmn_log_fatal("pidfile %s exists and is not a regular file!", params.pid_file_pre_chroot);
            if((st.st_mode & PERMS_MASK) != PERMS644)
                if(chmod(params.pid_file_pre_chroot, PERMS644))
                    dmn_log_fatal("chmod('%s',%.4o) failed: %s", params.pid_file_pre_chroot, PERMS644, dmn_strerror(errno));
            // file chown only if privdrop
            if(params.will_privdrop) {
                if(st.st_uid != params.uid || st.st_gid != params.gid)
                    if(chown(params.pid_file_pre_chroot, params.uid, params.gid))
                        dmn_log_fatal("chown('%s',%u,%u) failed: %s", params.pid_file_pre_chroot, params.uid, params.gid, dmn_strerror(errno));
            }
        }
    }

    state.phase = PHASE3_INIT3;
}

unsigned dmn_add_pcall(dmn_func_vv_t func) {
    phase_check(0, PHASE4_FORKED, 0);
    if(!func)
        dmn_log_fatal("BUG: set_pcall requires a funcptr argument!");
    const unsigned idx = num_pcalls;
    if(idx >= 64)
        dmn_log_fatal("Too many pcalls registered (64+)!");
    pcalls = realloc(pcalls, sizeof(dmn_func_vv_t) * (++num_pcalls));
    pcalls[idx] = func;
    return idx;
}

void dmn_fork(void) {
    phase_check(PHASE3_INIT3, PHASE5_SECURED, 1);

    // whether this invocation needs a forked helper process.
    // In background cases, we always need this to hold the
    //   terminal/parent open until final exit status is ready,
    //   and the "helper" is actually the original process instance
    //   from before any daemonization forks.
    // In foreground cases, we fork off a separate helper iff
    //   we plan to privdrop *and* pcalls have been registered, so
    //   that we have a root-owned process to execute the pcalls with.
    params.need_helper = true;

    // if foreground and not doing privdrop+pcalls, this
    //  whole phase basically does nothing
    if(params.foreground && (!params.will_privdrop || !num_pcalls)) {
        params.need_helper = false;
        state.phase = PHASE4_FORKED;
        return;
    }

    // These pipes are used to communicate with the "helper" process,
    //   which is the original parent when daemonizing properly, or
    //   a special forked helper when necessary in the foreground.
    int to_helper[2];
    int from_helper[2];
    if(pipe(to_helper))
        dmn_log_fatal("pipe() failed: %s", dmn_strerror(errno));
    if(pipe(from_helper))
        dmn_log_fatal("pipe() failed: %s", dmn_strerror(errno));
    state.fd_to_helper = to_helper[1];
    state.fd_from_helper = from_helper[0];

    // Fork for the first time...
    const pid_t first_fork_pid = fork();
    if(first_fork_pid == -1)
        dmn_log_fatal("fork() failed: %s", dmn_strerror(errno));

    if(params.foreground) {
        // In the special case of foreground+privdrop+pcalls, the parent/child relationship
        //  is reversed from normal flow below, and we do not do the rest
        //  of the daemonization steps.
        // send the child off to wait for messages in helper_proc()
        if(!first_fork_pid) { // helper child proc
            if(close(to_helper[1])) // close write-side
                dmn_log_fatal("close() of to_helper pipe write-side failed in foreground helper: %s", dmn_strerror(errno));
            if(close(from_helper[0])) // close read-side
                dmn_log_fatal("close() of from_helper pipe read-side failed in foreground helper: %s", dmn_strerror(errno));
            helper_proc(to_helper[0], from_helper[1]);
            dmn_assert(0); // above never returns control
        }
        if(close(to_helper[0])) // close read-side
            dmn_log_fatal("close() of to_helper pipe write-side failed in foreground daemon: %s", dmn_strerror(errno));
        if(close(from_helper[1])) // close read-side
            dmn_log_fatal("close() of from_helper pipe read-side failed in foreground daemon: %s", dmn_strerror(errno));

        state.phase = PHASE4_FORKED;
        return;
    }

    if(first_fork_pid) { // original parent proc
        if(close(to_helper[1])) // close write-side
            dmn_log_fatal("close() of to_helper pipe write-side failed in foreground helper: %s", dmn_strerror(errno));
        if(close(from_helper[0])) // close read-side
            dmn_log_fatal("close() of from_helper pipe read-side failed in foreground helper: %s", dmn_strerror(errno));
        helper_proc(to_helper[0], from_helper[1]);
        dmn_assert(0); // above never returns control
    }

    if(close(to_helper[0])) // close read-side
        dmn_log_fatal("close() of to_helper pipe write-side failed in first child: %s", dmn_strerror(errno));
    if(close(from_helper[1])) // close read-side
        dmn_log_fatal("close() of from_helper pipe read-side failed in first child: %s", dmn_strerror(errno));

    // setsid() and ignore HUP/PIPE before the second fork
    if(setsid() == -1) dmn_log_fatal("setsid() failed: %s", dmn_strerror(errno));
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;

    if(sigaction(SIGHUP, &sa, NULL))
        dmn_log_fatal("sigaction to ignore SIGHUP failed: %s", dmn_strerror(errno));

    if(sigaction(SIGPIPE, &sa, NULL))
        dmn_log_fatal("sigaction to ignore SIGPIPE failed: %s", dmn_strerror(errno));

    // Fork again.  This time the intermediate parent exits immediately.
    const pid_t second_fork_pid = fork();
    if(second_fork_pid == -1)
        dmn_log_fatal("fork() failed: %s", dmn_strerror(errno));
    if(second_fork_pid) // intermediate parent proc
        _exit(0);

    // we're now in the final child daemon
    umask(022);

    if(!freopen("/dev/null", "r", stdin))
        dmn_log_fatal("Cannot open /dev/null: %s", dmn_strerror(errno));
    if(!freopen("/dev/null", "w", stdout))
        dmn_log_fatal("Cannot open /dev/null: %s", dmn_strerror(errno));
    if(!freopen("/dev/null", "r+", stderr))
        dmn_log_fatal("Cannot open /dev/null: %s", dmn_strerror(errno));
    dmn_log_info("Daemonized, final pid is %li", (long)getpid());

    state.phase = PHASE4_FORKED;
}

void dmn_secure(void) {
    phase_check(PHASE4_FORKED, PHASE6_PIDLOCKED, 1);

    if(params.will_chroot) {
        dmn_assert(params.invoked_as_root);
        dmn_assert(params.will_privdrop);
        dmn_assert(params.chroot);
        dmn_assert(params.chroot[0] == '/');

        // On most systems, this seems to get the timezone cached for vsyslog() to use inside chroot()
        tzset();
        // lock self into the chroot directory
        if(chroot(params.chroot))
            dmn_log_fatal("chroot(%s) failed: %s", params.chroot, dmn_strerror(errno));
        if(chdir("/"))
            dmn_log_fatal("chdir(/) inside chroot(%s) failed: %s", params.chroot, dmn_strerror(errno));
    }

    if(params.will_privdrop) {
        dmn_assert(params.invoked_as_root);
        dmn_assert(params.uid);
        dmn_assert(params.gid);

        // drop privs
        if(setgid(params.gid))
            dmn_log_fatal("setgid(%u) failed: %s", params.gid, dmn_strerror(errno));
        if(setuid(params.uid))
            dmn_log_fatal("setuid(%u) failed: %s", params.uid, dmn_strerror(errno));

        // verify that regaining root privs fails, and [e][ug]id values are as expected
        if(    !setegid(0)
            || !seteuid(0)
            || geteuid() != params.uid
            || getuid() != params.uid
            || getegid() != params.gid
            || getgid() != params.gid
        )
            dmn_log_fatal("Platform-specific BUG: setgid() and/or setuid() do not permanently drop privs as expected!");
    }

    state.phase = PHASE5_SECURED;
}

void dmn_acquire_pidfile(void) {
    phase_check(PHASE5_SECURED, PHASE7_FINISHED, 1);

    if(!params.pid_file_post_chroot) {
        state.phase = PHASE6_PIDLOCKED;
        return;
    }

    pid_t pid = getpid();

    // string copy of pid for writing to pidfile
    char pidbuf[PIDBUF_LEN];
    const ssize_t pidlen = snprintf(pidbuf, PIDBUF_LEN, "%li\n", (long)pid);
    if(pidlen < 2)
        dmn_log_fatal("snprintf() for pid number failed");

    // flock structure for acquiring pidfile lock
    struct flock pidlock_set;
    memset(&pidlock_set, 0, sizeof(struct flock));
    pidlock_set.l_type = F_WRLCK;
    pidlock_set.l_whence = SEEK_SET;

    // get an open write-handle on the pidfile for lock+update
    int pidfd = open(params.pid_file_post_chroot, O_WRONLY | O_CREAT, 0644);
    if(pidfd == -1)
        dmn_log_fatal("open(%s, O_WRONLY|O_CREAT) failed: %s", params.pid_file_post_chroot, dmn_strerror(errno));
    if(fcntl(pidfd, F_SETFD, FD_CLOEXEC))
        dmn_log_fatal("fcntl(%s, F_SETFD, FD_CLOEXEC) failed: %s", params.pid_file_post_chroot, dmn_strerror(errno));

    // if restarting, TERM the old daemon and wait for it to exit for a bit...
    if(params.restart) {
        const pid_t old_pid = dmn_status();
        if(old_pid) {
            dmn_log_info("restart: Stopping previous daemon instance at pid %li...", (long)old_pid);
            terminate_pid_and_wait(old_pid);
        }
        else {
            dmn_log_info("restart: No previous daemon instance to stop...");
        }
    }

    // Attempt lock
    if(fcntl(pidfd, F_SETLK, &pidlock_set)) {
        // Various failure modes
        if(errno != EAGAIN && errno != EACCES)
            dmn_log_fatal("bug? fcntl(pidfile, F_SETLK) failed: %s", dmn_strerror(errno));
        if(params.restart)
            dmn_log_fatal("restart: failed, cannot shut down previous instance and/or acquire pidfile lock (pidfile: %s, pid: %li)", params.pid_file_post_chroot, (long)dmn_status());
        else
            dmn_log_fatal("start: failed, another instance of this daemon is already running (pidfile: %s, pid: %li)", params.pid_file_post_chroot, (long)dmn_status());
    }

    // Success - assuming writing to our locked pidfile doesn't fail!
    if(ftruncate(pidfd, 0))
        dmn_log_fatal("truncating pidfile failed: %s", dmn_strerror(errno));
    if(write(pidfd, pidbuf, (size_t) pidlen) != pidlen)
        dmn_log_fatal("writing to pidfile failed: %s", dmn_strerror(errno));

    // leak of pidfd here is intentional, it stays open/locked for the duration
    //   of the daemon's execution.  Daemon death by any means unlocks-on-close,
    //   signalling to other code that this instance is no longer running...
    state.phase = PHASE6_PIDLOCKED;
}

void dmn_pcall(unsigned id) {
    phase_check(PHASE4_FORKED, PHASE7_FINISHED, 0);

    if(id >= num_pcalls)
        dmn_log_fatal("BUG: dmn_daemon_pcall() on non-existent index %u", id);

    // if !will_privdrop, we can execute locally since privileges never changed
    if(!params.will_privdrop)
        return pcalls[id]();

    dmn_assert(state.fd_to_helper >= 0);
    dmn_assert(state.fd_from_helper >= 0);

    uint8_t msg = id + 64U;
    if(1 != write(state.fd_to_helper, (char*)&msg, 1))
        dmn_log_fatal("Bug? failed to write pcall request for %u to helper! Errno was %s", id, dmn_strerror(errno));
    if(1 != read(state.fd_from_helper, (char*)&msg, 1))
        dmn_log_fatal("Bug? failed to read pcall return for %u from helper! Errno was %s", id, dmn_strerror(errno));
    if(msg != ((id + 64U) | 128U))
        dmn_log_fatal("Bug? invalid pcall return of '%hhu' for %u from helper!", msg, id);
}

void dmn_finish(void) {
    phase_check(PHASE6_PIDLOCKED, 0, 1);

    // Again, in this case there's no other process to talk to
    if(!params.need_helper) {
        dmn_assert(state.fd_to_helper == -1);
        dmn_assert(state.fd_from_helper == -1);
        state.phase = PHASE7_FINISHED;
        return;
    }

    dmn_assert(state.fd_to_helper >= 0);
    dmn_assert(state.fd_from_helper >= 0);

    // inform the helper of our success, but if for some reason
    //   it died before we could do so, carry on anyways...
    errno = 0;
    uint8_t msg = 0;
    if(1 != write(state.fd_to_helper, &msg, 1))
        dmn_log_fatal("Bug? failed to notify helper of daemon success! Errno was %s", dmn_strerror(errno));
    if(1 != read(state.fd_from_helper, &msg, 1))
        dmn_log_fatal("Bug? failed to read helper final status! Errno was %s", dmn_strerror(errno));
    if(msg != 128U)
        dmn_log_fatal("Bug? final message from helper was '%hhu'", msg);
    close(state.fd_to_helper);
    close(state.fd_from_helper);
    if(!params.foreground)
        dmn_log_close_stderr_out();

    state.phase = PHASE7_FINISHED;
}
