//
// Created by victor on 5/7/26.
//
#ifndef OFFS_HTTP_STATUS_H
#define OFFS_HTTP_STATUS_H

// Use http-parser's status codes and status string function.
// http-parser defines enum http_status and http_status_str().
// Include <http_parser.h> to access HTTP_STATUS_OK, HTTP_STATUS_NOT_FOUND, etc.
// Use http_status_str((enum http_status)status) to get the reason phrase.

#include <http_parser.h>

#endif // OFFS_HTTP_STATUS_H