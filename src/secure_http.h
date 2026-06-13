#ifndef SECURE_HTTP_H
#define SECURE_HTTP_H

#include <stddef.h>

/* HTTP request structure */
typedef struct {
    char method[16];
    char path[256];
    char version[16];
    int content_length;
    int headers_complete;
    char *body;
    size_t body_size;
} http_request_t;

/* HTTP response codes */
#define HTTP_OK 200
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_METHOD_NOT_ALLOWED 405
#define HTTP_REQUEST_TOO_LARGE 413
#define HTTP_INTERNAL_ERROR 500

/**
 * Parse HTTP request safely
 * @param buffer - Raw HTTP request data
 * @param buffer_len - Length of buffer
 * @param request - Output structure
 * @return 0 on success, negative on error
 */
int parse_http_request_safe(const char *buffer, size_t buffer_len, http_request_t *request);

/**
 * Send HTTP response with security headers
 * @param fd - Socket file descriptor
 * @param status_code - HTTP status code
 * @param content_type - Content type header
 * @param body - Response body
 * @param body_len - Length of body
 */
void send_http_response_safe(int fd, int status_code, const char *content_type,
                            const char *body, size_t body_len);

/**
 * Validate HTTP method
 * @param method - HTTP method string
 * @return 1 if valid, 0 if invalid
 */
int validate_http_method(const char *method);

/**
 * Validate HTTP path for security
 * @param path - HTTP path string
 * @return 1 if valid, 0 if invalid
 */
int validate_http_path(const char *path);

#endif