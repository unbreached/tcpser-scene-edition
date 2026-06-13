/*
 * Secure HTTP Request Handling for TCPSER Web Dashboard
 *
 * Provides safe HTTP request parsing to prevent common web vulnerabilities:
 * - Request smuggling
 * - Header injection
 * - Path traversal
 * - Buffer overflows
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "secure_http.h"
#include "debug.h"

/**
 * Validate HTTP method
 */
int validate_http_method(const char *method) {
    if (!method) return 0;

    /* Only allow safe HTTP methods */
    const char *allowed_methods[] = {
        "GET", "POST", "HEAD", "OPTIONS", NULL
    };

    for (int i = 0; allowed_methods[i]; i++) {
        if (strcmp(method, allowed_methods[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/**
 * Validate HTTP path for security
 */
int validate_http_path(const char *path) {
    if (!path || strlen(path) == 0) return 0;

    /* Path must start with / */
    if (path[0] != '/') return 0;

    /* Check for directory traversal */
    if (strstr(path, "..") ||
        strstr(path, "//") ||
        strstr(path, "\\") ||
        strstr(path, "%2e") ||
        strstr(path, "%2f") ||
        strstr(path, "%5c")) {
        return 0;
    }

    /* Check path length */
    if (strlen(path) > 255) return 0;

    /* Check for valid characters only */
    for (const char *p = path; *p; p++) {
        if (!isprint((unsigned char)*p) || *p == '\r' || *p == '\n') {
            return 0;
        }
    }

    return 1;
}

/**
 * Parse HTTP request safely
 */
int parse_http_request_safe(const char *buffer, size_t buffer_len, http_request_t *request) {
    if (!buffer || !request || buffer_len == 0) {
        return -1;
    }

    /* Initialize request structure */
    memset(request, 0, sizeof(http_request_t));
    request->content_length = -1;

    /* Find end of first line */
    const char *line_end = memchr(buffer, '\n', buffer_len);
    if (!line_end || line_end == buffer) {
        LOG(LOG_ERROR, "Invalid HTTP request: no newline found");
        return -1;
    }

    /* Extract first line */
    size_t first_line_len = line_end - buffer;
    if (first_line_len >= 512) {
        LOG(LOG_ERROR, "HTTP request line too long: %zu bytes", first_line_len);
        return -1;
    }

    char first_line[513];
    memcpy(first_line, buffer, first_line_len);
    first_line[first_line_len] = '\0';

    /* Remove trailing \r if present */
    if (first_line_len > 0 && first_line[first_line_len - 1] == '\r') {
        first_line[first_line_len - 1] = '\0';
        first_line_len--;
    }

    /* Parse method, path, version */
    char *saveptr;
    char *token = strtok_r(first_line, " ", &saveptr);
    if (!token) {
        LOG(LOG_ERROR, "Invalid HTTP request: no method");
        return -1;
    }

    /* Copy method with bounds checking */
    if (strlen(token) >= sizeof(request->method)) {
        LOG(LOG_ERROR, "HTTP method too long: %s", token);
        return -1;
    }
    strcpy(request->method, token);

    /* Validate method */
    if (!validate_http_method(request->method)) {
        LOG(LOG_ERROR, "Invalid HTTP method: %s", request->method);
        return -1;
    }

    /* Parse path */
    token = strtok_r(NULL, " ", &saveptr);
    if (!token) {
        LOG(LOG_ERROR, "Invalid HTTP request: no path");
        return -1;
    }

    /* Copy path with bounds checking */
    if (strlen(token) >= sizeof(request->path)) {
        LOG(LOG_ERROR, "HTTP path too long: %s", token);
        return -1;
    }
    strcpy(request->path, token);

    /* Validate path */
    if (!validate_http_path(request->path)) {
        LOG(LOG_ERROR, "Invalid HTTP path: %s", request->path);
        return -1;
    }

    /* Parse version */
    token = strtok_r(NULL, " \t", &saveptr);
    if (!token) {
        /* Default to HTTP/1.0 if no version specified */
        strcpy(request->version, "HTTP/1.0");
    } else {
        if (strlen(token) >= sizeof(request->version)) {
            LOG(LOG_ERROR, "HTTP version too long: %s", token);
            return -1;
        }
        strcpy(request->version, token);

        /* Validate version */
        if (strcmp(request->version, "HTTP/1.0") != 0 &&
            strcmp(request->version, "HTTP/1.1") != 0) {
            LOG(LOG_ERROR, "Unsupported HTTP version: %s", request->version);
            return -1;
        }
    }

    /* Parse headers (simplified for dashboard use) */
    const char *headers_start = line_end + 1;
    const char *headers_end = strstr(headers_start, "\r\n\r\n");
    if (!headers_end) {
        headers_end = strstr(headers_start, "\n\n");
        if (!headers_end) {
            /* Headers not complete */
            request->headers_complete = 0;
            return 0;
        }
    }

    request->headers_complete = 1;

    /* Look for Content-Length header */
    const char *content_length_header = strstr(headers_start, "Content-Length:");
    if (content_length_header && content_length_header < headers_end) {
        content_length_header += 15; /* Skip "Content-Length:" */
        while (*content_length_header == ' ' || *content_length_header == '\t') {
            content_length_header++;
        }

        char *endptr;
        long content_length = strtol(content_length_header, &endptr, 10);
        if (content_length >= 0 && content_length <= 1048576) { /* Max 1MB */
            request->content_length = (int)content_length;
        } else {
            LOG(LOG_ERROR, "Invalid Content-Length: %ld", content_length);
            return -1;
        }
    }

    LOG(LOG_DEBUG, "HTTP request parsed: %s %s %s (Content-Length: %d)",
        request->method, request->path, request->version, request->content_length);

    return 0;
}

/**
 * Send HTTP response with security headers
 */
void send_http_response_safe(int fd, int status_code, const char *content_type,
                            const char *body, size_t body_len) {
    char status_text[64];
    char header[1024];

    /* Map status code to text */
    switch (status_code) {
        case 200: strcpy(status_text, "OK"); break;
        case 400: strcpy(status_text, "Bad Request"); break;
        case 401: strcpy(status_text, "Unauthorized"); break;
        case 404: strcpy(status_text, "Not Found"); break;
        case 405: strcpy(status_text, "Method Not Allowed"); break;
        case 413: strcpy(status_text, "Request Entity Too Large"); break;
        case 500: strcpy(status_text, "Internal Server Error"); break;
        case 503: strcpy(status_text, "Service Unavailable"); break;
        default: strcpy(status_text, "Unknown"); break;
    }

    /* Build headers with security features */
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-XSS-Protection: 1; mode=block\r\n"
        "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status_code, status_text, content_type ? content_type : "text/plain", body_len
    );

    /* Send header */
    ssize_t written = write(fd, header, strlen(header));
    if (written < 0) {
        LOG(LOG_ERROR, "Failed to write HTTP response header");
        return;
    }

    /* Send body if present */
    if (body && body_len > 0) {
        written = write(fd, body, body_len);
        if (written < 0) {
            LOG(LOG_ERROR, "Failed to write HTTP response body");
        }
    }
}