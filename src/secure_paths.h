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
 * Get a safe configuration file path from environment or default
 * @param env_var - Environment variable name
 * @param default_path - Default path if env var not set or invalid
 * @return Safe path (statically allocated)
 */
const char *get_safe_config_path(const char *env_var, const char *default_path);

/**
 * Check if a path contains directory traversal sequences
 * @param path - Path to check
 * @return 1 if path contains traversal, 0 if safe
 */
int contains_path_traversal(const char *path);

#endif