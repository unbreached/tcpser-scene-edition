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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

#include "secure_script.h"
#include "debug.h"

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

    if (stat(script_path, &st) != 0) {
        LOG(LOG_DEBUG, "Script not found: %s", script_path);
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

/**
 * Execute script with security constraints
 */
int execute_script_secure(const char *script_name, const char *peer_ip, int timeout_seconds) {
    char script_path[512];
    pid_t pid;
    int status;
    int result = SCRIPT_OK;

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

    LOG(LOG_INFO, "Executing script: %s (peer: %s)", script_path, peer_ip ? peer_ip : "unknown");

    /* Fork child process */
    pid = fork();
    if (pid == 0) {
        /* Child process */

        /* Set environment variables for script */
        if (peer_ip) {
            setenv("TCPSER_PEER_IP", peer_ip, 1);
        }
        setenv("TCPSER_SCRIPT_DIR", TCPSER_SCRIPT_DIR, 1);

        /* Clear potentially dangerous environment variables */
        unsetenv("IFS");
        unsetenv("PATH");
        unsetenv("LD_PRELOAD");
        unsetenv("LD_LIBRARY_PATH");

        /* Set safe PATH */
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);

        /* Create new process group for easier cleanup */
        if (setsid() == -1) {
            LOG(LOG_ERROR, "setsid failed: %s", strerror(errno));
        }

        /* Execute script directly (no shell) */
        execl(script_path, script_name, (char *)NULL);

        /* If we get here, exec failed */
        LOG(LOG_ERROR, "Failed to execute script %s: %s", script_path, strerror(errno));
        _exit(127);  /* Exit with distinctive code */
    }
    else if (pid > 0) {
        /* Parent process - wait for child */

        if (timeout_seconds > 0) {
            /* Set up timeout */
            struct sigaction old_action;
            struct sigaction action;
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            action.sa_flags = 0;
            sigaction(SIGALRM, &action, &old_action);

            alarm(timeout_seconds);

            /* Wait for child with timeout */
            pid_t wait_result = waitpid(pid, &status, 0);

            alarm(0);  /* Cancel alarm */
            sigaction(SIGALRM, &old_action, NULL);  /* Restore handler */

            if (wait_result == -1) {
                if (errno == EINTR) {
                    /* Timeout occurred */
                    LOG(LOG_WARN, "Script %s timed out after %d seconds, killing",
                        script_name, timeout_seconds);
                    kill(-pid, SIGTERM);  /* Kill process group */
                    usleep(100000);  /* Give it 100ms to exit gracefully */
                    kill(-pid, SIGKILL);  /* Force kill if needed */
                    waitpid(pid, &status, 0);  /* Reap zombie */
                    result = SCRIPT_EXEC_FAILED;
                } else {
                    LOG(LOG_ERROR, "waitpid failed: %s", strerror(errno));
                    result = SCRIPT_EXEC_FAILED;
                }
            }
        } else {
            /* No timeout, simple wait */
            if (waitpid(pid, &status, 0) == -1) {
                LOG(LOG_ERROR, "waitpid failed: %s", strerror(errno));
                result = SCRIPT_EXEC_FAILED;
            }
        }

        /* Check exit status */
        if (result == SCRIPT_OK) {
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    LOG(LOG_INFO, "Script %s completed successfully", script_name);
                } else if (exit_code == 127) {
                    LOG(LOG_ERROR, "Script %s failed to execute", script_name);
                    result = SCRIPT_EXEC_FAILED;
                } else {
                    LOG(LOG_WARN, "Script %s exited with code %d", script_name, exit_code);
                    /* Still consider this success - script ran but returned non-zero */
                }
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                LOG(LOG_WARN, "Script %s terminated by signal %d", script_name, sig);
                result = SCRIPT_EXEC_FAILED;
            }
        }
    }
    else {
        /* Fork failed */
        LOG(LOG_ERROR, "Failed to fork for script execution: %s", strerror(errno));
        result = SCRIPT_EXEC_FAILED;
    }

    return result;
}