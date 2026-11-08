/*
 * Secure Path Validation for TCPSER
 *
 * Provides safe file path handling to prevent directory traversal attacks
 * and other path-based security vulnerabilities.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>

#include "secure_paths.h"
#include "debug.h"

/**
 * Check if a path contains directory traversal sequences
 */
int contains_path_traversal(const char *path) {
    if (!path) return 1;

    /* Traversal patterns. Only whole "." / ".." segments are rejected: the
     * old "/." test refused every dotfile directory, so
     * TCPSER_BLOCKLIST=/home/pi/.tcpser/blocklist was silently ignored. */
    if (strstr(path, "..") ||       /* parent directory, anywhere */
        strstr(path, "//") ||       /* double slash */
        strstr(path, "/./") ||      /* "." segment */
        strncmp(path, "./", 2) == 0 ||
        strstr(path, "~")) {        /* home directory expansion */
        return 1;
    }
    { size_t n = strlen(path); if (n >= 2 && strcmp(path + n - 2, "/.") == 0) return 1; }

    /* Check for encoded traversal attempts */
    if (strstr(path, "%2e") ||      /* URL encoded dots */
        strstr(path, "%2f") ||      /* URL encoded slash */
        strstr(path, "%5c")) {      /* URL encoded backslash */
        return 1;
    }

    /* Check for null bytes (path truncation) */
    for (const char *p = path; *p; p++) {
        if (*p == '\0') return 1;
    }

    return 0;
}

/**
 * Validate and sanitize a file path for security
 */
int validate_file_path(const char *path, char *sanitized, size_t sanitized_size) {
    if (!path || !sanitized || sanitized_size == 0) {
        LOG(LOG_ERROR, "Invalid parameters for path validation");
        return -1;
    }

    /* Check for null or empty path */
    if (strlen(path) == 0) {
        LOG(LOG_ERROR, "Empty path not allowed");
        return -1;
    }

    /* Check path length */
    if (strlen(path) >= MAX_SAFE_PATH) {
        LOG(LOG_ERROR, "Path too long: %zu chars (max %d)", strlen(path), MAX_SAFE_PATH);
        return -1;
    }

    /* Check for directory traversal */
    if (contains_path_traversal(path)) {
        LOG(LOG_ERROR, "Path contains traversal sequences: %s", path);
        return -1;
    }

    /* Path must be absolute for security */
    if (path[0] != '/') {
        LOG(LOG_ERROR, "Relative paths not allowed: %s", path);
        return -1;
    }

    /* Check for suspicious characters */
    for (const char *p = path; *p; p++) {
        if (!isprint((unsigned char)*p) || *p == '\n' || *p == '\r') {
            LOG(LOG_ERROR, "Path contains invalid character: 0x%02x", (unsigned char)*p);
            return -1;
        }
    }

    /* Resolve to canonical path */
    char *canonical = realpath(path, NULL);
    if (canonical) {
        /* Ensure canonical path is still absolute and doesn't escape */
        if (canonical[0] != '/' || contains_path_traversal(canonical)) {
            LOG(LOG_ERROR, "Canonical path validation failed: %s -> %s", path, canonical);
            free(canonical);
            return -1;
        }

        /* Copy to output buffer */
        size_t canonical_len = strlen(canonical);
        if (canonical_len >= sanitized_size) {
            LOG(LOG_ERROR, "Canonical path too long for buffer");
            free(canonical);
            return -1;
        }

        strcpy(sanitized, canonical);
        free(canonical);
    } else {
        /* File doesn't exist - validate the path syntactically */
        if (strlen(path) >= sanitized_size) {
            LOG(LOG_ERROR, "Path too long for buffer");
            return -1;
        }
        strcpy(sanitized, path);
    }

    LOG(LOG_DEBUG, "Path validation successful: %s", sanitized);
    return 0;
}