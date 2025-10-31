/*
  MYFS Protocol Header
  Defines communication protocol between client and storage nodes
*/

#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>

// Maximum data size per request (1 MiB)
#define MAX_CHUNK_SIZE (1024 * 1024)
#define MAX_FRAGMENT_SIZE (MAX_CHUNK_SIZE + 1024)  // Extra space for metadata

// Request types
typedef enum {
    REQ_WRITE = 1,
    REQ_READ = 2,
    REQ_DELETE = 3
} request_type_t;

// Request header structure
typedef struct {
    request_type_t type;      // Request type
    char filename[256];       // File name
    size_t size;              // Data size
    off_t offset;             // File offset
    uint32_t fragment_id;     // Fragment ID (0 to n-1)
} request_header_t;

// Response header structure
typedef struct {
    int status;               // 0 = success, -1 = error
    size_t size;              // Size of data returned (for READ)
    int error_code;           // errno if error occurred
} response_header_t;

#endif

