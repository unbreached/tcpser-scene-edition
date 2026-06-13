#ifndef SECURE_SCRIPT_H
#define SECURE_SCRIPT_H

/* Secure script execution for tcpser */

/* Default script directory - only scripts from here can be executed */
#ifndef TCPSER_SCRIPT_DIR
#define TCPSER_SCRIPT_DIR "/usr/local/tcpser/scripts"
#endif

/* Maximum script name length */
#define MAX_SCRIPT_NAME 64

/* Script execution result codes */
#define SCRIPT_OK 0
#define SCRIPT_NOT_FOUND 1
#define SCRIPT_INVALID_NAME 2
#define SCRIPT_NOT_EXECUTABLE 3
#define SCRIPT_PERMISSION_DENIED 4
#define SCRIPT_EXEC_FAILED 5

/**
 * Validate and execute a script securely
 * @param script_name - Script name (basename only, no paths)
 * @param peer_ip - Client IP address (passed as environment variable)
 * @param timeout_seconds - Script execution timeout (0 = no timeout)
 * @return SCRIPT_OK on success, error code on failure
 */
int execute_script_secure(const char *script_name, const char *peer_ip, int timeout_seconds);

/**
 * Validate script name for security
 * @param script_name - Script name to validate
 * @return 1 if valid, 0 if invalid
 */
int validate_script_name(const char *script_name);

/**
 * Check if script exists and is executable
 * @param script_path - Full path to script
 * @return 1 if safe to execute, 0 otherwise
 */
int validate_script_file(const char *script_path);

#endif