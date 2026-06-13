#ifndef SECURE_PATHS_H
#define SECURE_PATHS_H

#include <stddef.h>

/* Maximum safe path length */
#define MAX_SAFE_PATH 512

/**
 * Validate and sanitize a file path for security
 * @param path - Input path to validate
 * @param sanitized - Output buffer for sanitized path
 * @param sanitized_size - Size of output buffer
 * @return 0 on success, -1 on validation failure
 */
int validate_file_path(const char *path, char *sanitized, size_t sanitized_size);

/**
 * Check if a path contains directory traversal sequences
 * @param path - Path to check
 * @return 1 if path contains traversal, 0 if safe
 */
int contains_path_traversal(const char *path);

#endif