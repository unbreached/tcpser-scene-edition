/*
 * Secure Script Execution for TCPSER
 *
 * Provides controlled script execution with security constraints:
 * - Scripts must be in predefined directory
 * - Script names validated (no path traversal)
 * - Scripts must have proper permissions
 * - Direct execution (no shell interpretation)
 * - Execution timeout protection
 */

/* Needed for fexecve() on glibc/musl (and keeps usleep visible). */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>

#include "secure_script.h"
#include "debug.h"

extern char **environ;

/**
 * Validate script name - only allow safe characters
 * Prevents path traversal and shell injection
 */
int validate_script_name(const char *script_name) {
    if (!script_name || strlen(script_name) == 0) {
        LOG(LOG_ERROR, "Script name is empty");
        return 0;
    }

    if (strlen(script_name) >= MAX_SCRIPT_NAME) {
        LOG(LOG_ERROR, "Script name too long: %zu chars (max %d)",
            strlen(script_name), MAX_SCRIPT_NAME);
        return 0;
    }

    /* Only allow alphanumeric, dash, underscore, dot */
    for (const char *p = script_name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
            LOG(LOG_ERROR, "Invalid character in script name: '%c'", *p);
            return 0;
        }
    }

    /* Prevent hidden files and parent directory references */
    if (script_name[0] == '.' || strstr(script_name, "..")) {
        LOG(LOG_ERROR, "Script name contains forbidden patterns: %s", script_name);
        return 0;
    }

    return 1;
}

/**
 * Validate script file - check permissions and ownership
 */
int validate_script_file(const char *script_path) {
    struct stat st;

    /* lstat (not stat) so a symlinked script is detected rather than followed. */
    if (lstat(script_path, &st) != 0) {
        LOG(LOG_DEBUG, "Script not found: %s", script_path);
        return 0;
    }

    /* Reject symlinks outright — they are a path-substitution (TOCTOU) vector. */
    if (S_ISLNK(st.st_mode)) {
        LOG(LOG_ERROR, "Script is a symlink (refused): %s", script_path);
        return 0;
    }

    /* Must be a regular file */
    if (!S_ISREG(st.st_mode)) {
        LOG(LOG_ERROR, "Script is not a regular file: %s", script_path);
        return 0;
    }

    /* Must be executable */
    if (!(st.st_mode & S_IXUSR)) {
        LOG(LOG_ERROR, "Script is not executable: %s", script_path);
        return 0;
    }

    /* Security check: not world-writable */
    if (st.st_mode & S_IWOTH) {
        LOG(LOG_ERROR, "Script is world-writable (insecure): %s", script_path);
        return 0;
    }

    /* Security check: reasonable size (prevent huge scripts) */
    if (st.st_size > 1024 * 1024) {  /* 1MB limit */
        LOG(LOG_ERROR, "Script too large: %lld bytes", (long long)st.st_size);
        return 0;
    }

    return 1;
}

/* ---- fork-side helpers: async-signal-safe only (no stdio, no malloc, no LOG) ---- */

/* Write "tcpser[hook]: <name>: <msg> (<n>)" to the log descriptor from a forked
 * child; n < 0 omits the parenthesised number. */
static void child_note(int logfd, const char *name, const char *msg, long n) {
    char buf[320];
    char num[24];
    size_t i = 0;
    const char *parts[7];
    int np = 0;

    num[0] = 0;
    if (n >= 0) {   /* minimal ltoa */
        char tmp[24]; int t = 0; unsigned long u = (unsigned long)n;
        do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u && t < 23);
        int k = 0;
        while (t > 0) num[k++] = tmp[--t];
        num[k] = 0;
    }
    parts[np++] = "tcpser[hook]: "; parts[np++] = name; parts[np++] = ": "; parts[np++] = msg;
    if (n >= 0) { parts[np++] = " ("; parts[np++] = num; parts[np++] = ")"; }
    for (int pi = 0; pi < np; pi++)
        for (const char *q = parts[pi]; q && *q && i < sizeof(buf) - 2; q++) buf[i++] = *q;
    buf[i++] = '\n';
    (void)!write(logfd, buf, i);
}

/* Build the environment for the hook BEFORE fork(): copy the parent's
 * environment minus dangerous/overridden names, then add ours. */
static char **build_hook_env(const char *peer_ip) {
    static const char *drop[] = { "IFS=", "PATH=", "LD_PRELOAD=", "LD_LIBRARY_PATH=",
                                  "DYLD_INSERT_LIBRARIES=", "DYLD_LIBRARY_PATH=",
                                  "TCPSER_PEER_IP=", "TCPSER_SCRIPT_DIR=", NULL };
    int n = 0;
    for (char **e = environ; e && *e; e++) n++;
    char **env = calloc((size_t)n + 4, sizeof(char *));
    if (!env) return NULL;
    int k = 0;
    for (char **e = environ; e && *e; e++) {
        int skip = 0;
        for (int d = 0; drop[d]; d++)
            if (strncmp(*e, drop[d], strlen(drop[d])) == 0) { skip = 1; break; }
        if (!skip) env[k++] = *e;   /* parent's strings stay valid until exec */
    }
    static char path_var[] = "PATH=/usr/local/bin:/usr/bin:/bin";
    static char dir_var[] = "TCPSER_SCRIPT_DIR=" TCPSER_SCRIPT_DIR;
    static char ip_var[64];
    env[k++] = path_var;
    env[k++] = dir_var;
    if (peer_ip) {
        snprintf(ip_var, sizeof(ip_var), "TCPSER_PEER_IP=%s", peer_ip);
        env[k++] = ip_var;
    }
    env[k] = NULL;
    return env;
}

/**
 * Execute script with security constraints — ASYNCHRONOUSLY.
 *
 * Process model (why two forks): tcpser ignores SIGCHLD so it never has to
 * reap. We fork a small supervisor that (1) forks the real script in its own
 * session, (2) waits for it with a polling waitpid(WNOHANG) up to
 * timeout_seconds, (3) kills the script's whole process group on timeout, and
 * (4) reports the outcome to the log descriptor with write(2). The supervisor
 * itself is auto-reaped by the parent's SIG_IGN disposition. The connection
 * thread returns immediately, so the caller gets RING/CONNECT on time.
 *
 * The previous implementation blocked the connection thread in waitpid() and
 * armed alarm() with SIGALRM at SIG_DFL — whose default action TERMINATES THE
 * PROCESS — so any hook that outlived its timeout killed tcpser (confirmed:
 * exit status 142). It also called setenv()/LOG() between fork and exec in a
 * multithreaded process, which can deadlock the child on a mutex held by
 * another thread at fork time. Nothing between fork and exec here touches
 * stdio, malloc or locks.
 */
int execute_script_secure(const char *script_name, const char *peer_ip, int timeout_seconds) {
    char script_path[512];
    pid_t pid;

    /* Validate script name */
    if (!validate_script_name(script_name)) {
        return SCRIPT_INVALID_NAME;
    }

    /* Build full script path */
    snprintf(script_path, sizeof(script_path), "%s/%s", TCPSER_SCRIPT_DIR, script_name);

    /* Validate script file */
    if (!validate_script_file(script_path)) {
        return SCRIPT_NOT_EXECUTABLE;
    }

    LOG(LOG_INFO, "Launching hook script %s (peer: %s, timeout %ds, async)",
        script_path, peer_ip ? peer_ip : "unknown", timeout_seconds);

    char **env = build_hook_env(peer_ip);
    if (!env) {
        LOG(LOG_ERROR, "Out of memory building hook environment");
        return SCRIPT_EXEC_FAILED;
    }
    int logfd = log_get_fd();
    char *const script_argv[] = { (char *)script_name, (char *)NULL };

    pid = fork();
    if (pid < 0) {
        LOG(LOG_ERROR, "Failed to fork for script execution: %s", strerror(errno));
        free(env);
        return SCRIPT_EXEC_FAILED;
    }
    if (pid > 0) {
        /* Parent: supervisor is running detached; it is auto-reaped (SIGCHLD ignored). */
        free(env);
        return SCRIPT_OK;
    }

    /* ---- supervisor (child #1): async-signal-safe calls only from here on ---- */
    signal(SIGCHLD, SIG_DFL);   /* we DO want to wait for our child */
    signal(SIGALRM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    pid_t script_pid = fork();
    if (script_pid < 0) {
        child_note(logfd, script_name, "could not fork script, errno", (long)errno);
        _exit(1);
    }
    if (script_pid == 0) {
        /* ---- script process (child #2) ---- */
        if (setsid() == -1) { /* own session+group so the supervisor can kill the whole tree */ }

        /* Re-open the script with O_NOFOLLOW (reject a symlink swapped in after
         * validation) and re-verify the descriptor, closing the TOCTOU window
         * between validate_script_file() and exec. */
        int sfd = open(script_path, O_RDONLY | O_NOFOLLOW);
        if (sfd < 0) {
            child_note(logfd, script_name, "cannot open script, errno", (long)errno);
            _exit(127);
        }
        struct stat fst;
        if (fstat(sfd, &fst) != 0 || !S_ISREG(fst.st_mode) ||
            !(fst.st_mode & S_IXUSR) || (fst.st_mode & S_IWOTH)) {
            child_note(logfd, script_name, "failed descriptor re-validation", -1);
            _exit(127);
        }
#if defined(__linux__)
        /* Execute the exact descriptor we validated — symlink/TOCTOU-proof. */
        extern int fexecve(int, char *const[], char *const[]);
        fexecve(sfd, script_argv, env);
#endif
        /* Portable fallback: the O_NOFOLLOW open + fstat above already rejected
         * a symlinked target. */
        execve(script_path, script_argv, env);
        child_note(logfd, script_name, "exec failed, errno", (long)errno);
        _exit(127);
    }

    /* ---- supervisor: wait with a bounded, signal-free timeout ---- */
    {
        int status = 0;
        long waited_ms = 0;
        long limit_ms = timeout_seconds > 0 ? (long)timeout_seconds * 1000L : -1;
        struct timespec tick = { 0, 100L * 1000L * 1000L };   /* 100 ms */
        for (;;) {
            pid_t w = waitpid(script_pid, &status, WNOHANG);
            if (w == script_pid) break;
            if (w < 0 && errno != EINTR) {
                child_note(logfd, script_name, "waitpid failed, errno", (long)errno);
                _exit(1);
            }
            if (limit_ms >= 0 && waited_ms >= limit_ms) {
                child_note(logfd, script_name, "timed out, killing it; timeout seconds", (long)timeout_seconds);
                kill(-script_pid, SIGTERM);
                nanosleep(&tick, NULL); nanosleep(&tick, NULL);
                kill(-script_pid, SIGKILL);
                waitpid(script_pid, &status, 0);
                _exit(2);
            }
            nanosleep(&tick, NULL);
            waited_ms += 100;
        }
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0)        child_note(logfd, script_name, "completed successfully", -1);
            else if (code == 127) child_note(logfd, script_name, "failed to execute (exit 127)", -1);
            else                  child_note(logfd, script_name, "exited with non-zero code", (long)code);
        } else if (WIFSIGNALED(status)) {
            child_note(logfd, script_name, "terminated by signal", (long)WTERMSIG(status));
        }
        _exit(0);
    }
}