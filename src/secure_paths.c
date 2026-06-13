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

    /* Check for obvious traversal patterns */
    if (strstr(path, "..") ||       /* Parent directory */
        strstr(path, "//") ||       /* Double slash */
        strstr(path, "./") ||       /* Current directory prefix */
        strstr(path, "/.") ||       /* Current/parent directory */
        strstr(path, "~")) {        /* Home directory expansion */
        return 1;
    }

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

/**
 * Get a safe configuration file path from environment or default
 */
const char *get_safe_config_path(const char *env_var, const char *default_path) {
    static char safe_path[MAX_SAFE_PATH];
    const char *env_path;

    if (!env_var || !default_path) {
        LOG(LOG_ERROR, "Invalid parameters for config path");
        return default_path;
    }

    env_path = getenv(env_var);

    /* Use default if environment variable not set */
    if (!env_path || strlen(env_path) == 0) {
        LOG(LOG_DEBUG, "Using default path for %s: %s", env_var, default_path);
        return default_path;
    }

    /* Validate the environment path */
    if (validate_file_path(env_path, safe_path, sizeof(safe_path)) != 0) {
        LOG(LOG_WARN, "Environment path %s='%s' failed validation, using default: %s",
            env_var, env_path, default_path);
        return default_path;
    }

    /* Additional security check: ensure path is in allowed directory */
    const char *allowed_prefixes[] = {
        "/etc/tcpser/",
        "/usr/local/etc/tcpser/",
        "/var/lib/tcpser/",
        "/home/tcpser/",
        "/opt/tcpser/",
        NULL
    };

    int path_allowed = 0;
    for (int i = 0; allowed_prefixes[i]; i++) {
        if (strncmp(safe_path, allowed_prefixes[i], strlen(allowed_prefixes[i])) == 0) {
            path_allowed = 1;
            break;
        }
    }

    if (!path_allowed) {
        LOG(LOG_WARN, "Environment path %s='%s' not in allowed directory, using default: %s",
            env_var, safe_path, default_path);
        return default_path;
    }

    LOG(LOG_INFO, "Using environment path for %s: %s", env_var, safe_path);
    return safe_path;
}