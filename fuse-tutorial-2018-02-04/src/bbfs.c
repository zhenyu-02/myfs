/*
  Big Brother File System
  Copyright (C) 2012 Joseph J. Pfeiffer, Jr., Ph.D. <pfeiffer@cs.nmsu.edu>

  This program can be distributed under the terms of the GNU GPLv3.
  See the file COPYING.

  This code is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.
  A copy of that code is included in the file fuse.h
  
  The point of this FUSE filesystem is to provide an introduction to
  FUSE.  It was my first FUSE filesystem as I got to know the
  software; hopefully, the comments in this code will help people who
  follow later to get a gentler introduction.

  This might be called a no-op filesystem:  it doesn't impose
  filesystem semantics on top of any other existing structure.  It
  simply reports the requests that come in, and passes them to an
  underlying filesystem.  The information is saved in a logfile named
  bbfs.log, in the directory from which you run bbfs.
*/
#include "config.h"
#include "params.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"
#include "protocol.h"

//  All the paths I see are relative to the root of the mounted
//  filesystem.  In order to get to the underlying filesystem, I need to
//  have the mountpoint.  I'll save it away early on in main(), and then
//  whenever I need a path for something I'll call this to construct
//  it.
static void bb_fullpath(char fpath[PATH_MAX], const char *path)
{
    strcpy(fpath, BB_DATA->rootdir);
    strncat(fpath, path, PATH_MAX); // ridiculously long paths will
				    // break here

    log_msg("    bb_fullpath:  rootdir = \"%s\", path = \"%s\", fpath = \"%s\"\n",
	    BB_DATA->rootdir, path, fpath);
}

///////////////////////////////////////////////////////////
// MYFS Helper Functions
///////////////////////////////////////////////////////////

// Helper function to send all data (handles partial sends)
static ssize_t send_all(int sockfd, const void* buf, size_t len) {
    size_t total_sent = 0;
    const char* ptr = (const char*)buf;
    
    while (total_sent < len) {
        ssize_t sent = send(sockfd, ptr + total_sent, len - total_sent, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;  // Interrupted, retry
            return -1;  // Error
        }
        if (sent == 0) {
            return -1;  // Connection closed
        }
        total_sent += sent;
    }
    return total_sent;
}

// Connect to a storage node
static int connect_to_node(const char* host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Try to convert host as IP address first
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        // If not an IP, try to resolve as hostname
        struct hostent* he = gethostbyname(host);
        if (he == NULL) {
            fprintf(stderr, "Failed to resolve host: %s\n", host);
            close(sock);
            return -1;
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    return sock;
}

// Reconnect to a specific node
static int reconnect_to_node(int node_id) {
    struct bb_state* state = BB_DATA;
    
    if (node_id < 0 || node_id >= state->num_nodes) {
        return -1;
    }
    
    fprintf(stderr, "[MYFS] Attempting to reconnect to node %d (%s:%d)...\n",
            node_id, state->nodes[node_id].host, state->nodes[node_id].port);
    log_msg("[MYFS] Reconnecting to node %d\n", node_id);
    
    // Close old socket if still open
    if (state->nodes[node_id].socket_fd >= 0) {
        close(state->nodes[node_id].socket_fd);
        state->nodes[node_id].socket_fd = -1;
    }
    
    // Try to reconnect
    state->nodes[node_id].socket_fd = connect_to_node(state->nodes[node_id].host, 
                                                       state->nodes[node_id].port);
    if (state->nodes[node_id].socket_fd < 0) {
        fprintf(stderr, "[MYFS] ✗ Reconnection to node %d failed\n", node_id);
        log_msg("[MYFS] Reconnection to node %d failed\n", node_id);
        return -1;
    }
    
    fprintf(stderr, "[MYFS] ✓ Reconnected to node %d, new socket fd=%d\n", 
            node_id, state->nodes[node_id].socket_fd);
    log_msg("[MYFS] Reconnected to node %d, socket fd=%d\n", 
            node_id, state->nodes[node_id].socket_fd);
    return 0;
}

// Initialize connections to all nodes
static int init_node_connections() {
    struct bb_state* state = BB_DATA;
    
    fprintf(stderr, "[MYFS] Initializing connections to %d storage nodes...\n", state->num_nodes);
    log_msg("[MYFS] Initializing connections to %d storage nodes...\n", state->num_nodes);
    
    for (int i = 0; i < state->num_nodes; i++) {
        fprintf(stderr, "[MYFS] Connecting to node %d: %s:%d\n", i, state->nodes[i].host, state->nodes[i].port);
        log_msg("[MYFS] Connecting to node %d: %s:%d\n", i, state->nodes[i].host, state->nodes[i].port);
        
        state->nodes[i].socket_fd = connect_to_node(state->nodes[i].host, state->nodes[i].port);
        if (state->nodes[i].socket_fd < 0) {
            fprintf(stderr, "[MYFS ERROR] Failed to connect to node %d (%s:%d)\n", 
                    i, state->nodes[i].host, state->nodes[i].port);
            log_msg("[MYFS ERROR] Failed to connect to node %d (%s:%d)\n", 
                    i, state->nodes[i].host, state->nodes[i].port);
            return -1;
        }
        
        fprintf(stderr, "[MYFS] ✓ Connected to node %d, socket fd=%d\n", i, state->nodes[i].socket_fd);
        log_msg("[MYFS] Connected to node %d, socket fd=%d\n", i, state->nodes[i].socket_fd);
    }
    
    fprintf(stderr, "[MYFS] ✓ All nodes connected successfully!\n");
    log_msg("[MYFS] All nodes connected successfully!\n");
    return 0;
}

// XOR data buffers for parity calculation
static void xor_buffers(char* dest, const char* src, size_t size) {
    for (size_t i = 0; i < size; i++) {
        dest[i] ^= src[i];
    }
}

// Buffer for accumulating writes before sending to storage nodes
typedef struct {
    char* buffer;
    size_t size;
    size_t capacity;
    off_t max_offset;
} write_buffer_t;

static write_buffer_t* get_write_buffer(const char* path, int create) {
    // For simplicity, use a static buffer for now
    // In production, you'd want a hash table of buffers per file
    static write_buffer_t buffer = {NULL, 0, 0, 0};
    static char last_path[PATH_MAX] = "";
    
    // If different file, flush old buffer
    if (buffer.buffer && strcmp(last_path, path) != 0) {
        free(buffer.buffer);
        buffer.buffer = NULL;
        buffer.size = 0;
        buffer.capacity = 0;
        buffer.max_offset = 0;
    }
    
    strncpy(last_path, path, PATH_MAX - 1);
    
    if (create && !buffer.buffer) {
        buffer.capacity = 4 * 1024 * 1024;  // 4MB max
        buffer.buffer = (char*)malloc(buffer.capacity);
        buffer.size = 0;
        buffer.max_offset = 0;
    }
    
    return &buffer;
}

// Distributed write function
static int myfs_write(const char* path, const char* buf, size_t size, off_t offset) {
    struct bb_state* state = BB_DATA;
    int num_nodes = state->num_nodes;
    int num_data_fragments = num_nodes - 1;  // n-1 data fragments
    
    fprintf(stderr, "[MYFS WRITE] path=%s, size=%zu, offset=%ld\n", 
            path, size, offset);
    log_msg("\n[MYFS WRITE] path=%s, size=%zu, offset=%ld, num_nodes=%d\n", 
            path, size, offset, num_nodes);
    
    // Get or create write buffer for this file
    write_buffer_t* wb = get_write_buffer(path, 1);
    if (!wb || !wb->buffer) {
        fprintf(stderr, "[MYFS WRITE ERROR] Failed to allocate write buffer\n");
        return -ENOMEM;
    }
    
    // Ensure buffer can hold data at this offset
    size_t required_size = offset + size;
    if (required_size > wb->capacity) {
        fprintf(stderr, "[MYFS WRITE ERROR] Write exceeds buffer capacity (%zu > %zu)\n",
                required_size, wb->capacity);
        return -EFBIG;
    }
    
    // Copy data to buffer
    memcpy(wb->buffer + offset, buf, size);
    if (offset + size > wb->size) {
        wb->size = offset + size;
    }
    if (offset + size > wb->max_offset) {
        wb->max_offset = offset + size;
    }
    
    fprintf(stderr, "[MYFS WRITE] Buffered %zu bytes at offset %ld (total buffered: %zu)\n",
            size, offset, wb->size);
    
    // Return success - actual distribution happens on flush/close
    return size;
}

// Function to actually send buffered data to storage nodes
static int myfs_flush_write_buffer(const char* path) {
    struct bb_state* state = BB_DATA;
    int num_nodes = state->num_nodes;
    int num_data_fragments = num_nodes - 1;
    
    write_buffer_t* wb = get_write_buffer(path, 0);
    if (!wb || !wb->buffer || wb->size == 0) {
        return 0;  // Nothing to flush
    }
    
    fprintf(stderr, "[MYFS FLUSH] ========== DISTRIBUTING %zu BYTES ==========\n", wb->size);
    log_msg("[MYFS FLUSH] Distributing %zu bytes to %d nodes\n", wb->size, num_nodes);
    
    // Calculate fragment size
    size_t fragment_size = (wb->size + num_data_fragments - 1) / num_data_fragments;
    
    fprintf(stderr, "[MYFS FLUSH] Fragment size: %zu bytes (total: %zu)\n", 
            fragment_size, wb->size);
    
    // Allocate buffers for fragments
    char** fragments = (char**)malloc(num_nodes * sizeof(char*));
    if (!fragments) {
        fprintf(stderr, "[MYFS WRITE ERROR] Failed to allocate fragment pointer array\n");
        log_msg("[MYFS WRITE ERROR] Failed to allocate fragment pointer array\n");
        return -ENOMEM;
    }
    
    // Initialize all pointers to NULL for safe cleanup
    for (int i = 0; i < num_nodes; i++) {
        fragments[i] = NULL;
    }
    
    // Allocate each fragment buffer
    for (int i = 0; i < num_nodes; i++) {
        fragments[i] = (char*)calloc(fragment_size, 1);
        if (!fragments[i]) {
            fprintf(stderr, "[MYFS WRITE ERROR] Failed to allocate fragment %d buffer (%zu bytes)\n", 
                    i, fragment_size);
            log_msg("[MYFS WRITE ERROR] Failed to allocate fragment %d buffer\n", i);
            // Clean up already allocated buffers
            for (int j = 0; j < i; j++) {
                free(fragments[j]);
            }
            free(fragments);
            return -ENOMEM;
        }
    }
    
    // Distribute buffered data across fragments
    fprintf(stderr, "[MYFS FLUSH] Distributing data across %d data fragments...\n", num_data_fragments);
    for (size_t i = 0; i < wb->size; i++) {
        int frag_idx = (i / fragment_size) % num_data_fragments;  // Which fragment
        size_t frag_pos = i % fragment_size;  // Position within fragment
        
        if (frag_idx < num_data_fragments && frag_pos < fragment_size) {
            fragments[frag_idx][frag_pos] = wb->buffer[i];
        }
    }
    
    // Calculate parity fragment (XOR of all data fragments)
    fprintf(stderr, "[MYFS FLUSH] Calculating parity (XOR) for fragment %d...\n", num_nodes - 1);
    memset(fragments[num_nodes - 1], 0, fragment_size);
    for (int i = 0; i < num_data_fragments; i++) {
        xor_buffers(fragments[num_nodes - 1], fragments[i], fragment_size);
    }
    
    // Send fragments to nodes
    fprintf(stderr, "[MYFS FLUSH] Sending fragments to %d nodes...\n", num_nodes);
    int retstat = 0;
    for (int i = 0; i < num_nodes; i++) {
        request_header_t req;
        response_header_t resp;
        
        req.type = REQ_WRITE;
        strncpy(req.filename, path + 1, sizeof(req.filename) - 1);  // Skip leading '/'
        req.size = fragment_size;
        req.offset = 0;  // Write entire fragment from beginning
        req.fragment_id = i;
        
        fprintf(stderr, "[MYFS FLUSH] Node %d: Sending header (file=%s, frag=%u, size=%zu, offset=%ld)...\n",
                i, req.filename, req.fragment_id, req.size, req.offset);
        
        // Send request header (with retry on connection failure)
        int send_success = 0;
        for (int retry = 0; retry < 2; retry++) {
            if (send_all(state->nodes[i].socket_fd, &req, sizeof(req)) == sizeof(req)) {
                send_success = 1;
                break;
            }
            
            // Send failed, try to reconnect
            if (retry == 0) {
                fprintf(stderr, "[MYFS FLUSH] ⚠ Node %d: Send header failed, attempting reconnect...\n", i);
                if (reconnect_to_node(i) < 0) {
                    fprintf(stderr, "[MYFS FLUSH ERROR] Node %d: Reconnect failed\n", i);
                    break;
                }
                // Retry with new connection
            }
        }
        
        if (!send_success) {
            fprintf(stderr, "[MYFS FLUSH ERROR] Failed to send request header to node %d after retry\n", i);
            log_msg("Failed to send request to node %d\n", i);
            retstat = -EIO;
            goto cleanup;
        }
        
        fprintf(stderr, "[MYFS FLUSH] Node %d: Sending data (%zu bytes)...\n", i, fragment_size);
        
        // Send data
        if (send_all(state->nodes[i].socket_fd, fragments[i], fragment_size) != (ssize_t)fragment_size) {
            fprintf(stderr, "[MYFS FLUSH ERROR] Failed to send data to node %d\n", i);
            log_msg("Failed to send data to node %d\n", i);
            retstat = -EIO;
            goto cleanup;
        }
        
        fprintf(stderr, "[MYFS FLUSH] Node %d: Waiting for response...\n", i);
        
        // Receive response
        if (recv(state->nodes[i].socket_fd, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) {
            fprintf(stderr, "[MYFS FLUSH ERROR] Failed to receive response from node %d\n", i);
            log_msg("Failed to receive response from node %d\n", i);
            retstat = -EIO;
            goto cleanup;
        }
        
        if (resp.status != 0) {
            fprintf(stderr, "[MYFS FLUSH ERROR] Node %d returned error: status=%d, errno=%d\n", 
                    i, resp.status, resp.error_code);
            log_msg("[MYFS FLUSH ERROR] Node %d returned error: %d\n", i, resp.error_code);
            retstat = -resp.error_code;
            goto cleanup;
        }
        
        fprintf(stderr, "[MYFS FLUSH] ✓ Node %d: Fragment %d written successfully (%zu bytes)\n", 
                i, i, fragment_size);
        log_msg("[MYFS FLUSH] Successfully wrote fragment %d to node %d\n", i, i);
    }
    
    fprintf(stderr, "[MYFS FLUSH] ========== COMPLETE: %zu bytes written ==========\n", wb->size);
    retstat = wb->size;  // Return number of bytes written
    
cleanup:
    fprintf(stderr, "[MYFS FLUSH] Cleanup: Freeing %d fragment buffers\n", num_nodes);
    for (int i = 0; i < num_nodes; i++) {
        if (fragments[i]) {
            free(fragments[i]);
        }
    }
    free(fragments);
    
    if (retstat < 0) {
        fprintf(stderr, "[MYFS FLUSH] ========== FAILED: error=%d ==========\n", retstat);
    } else {
        // Clear buffer after successful flush
        wb->size = 0;
        wb->max_offset = 0;
    }
    
    return retstat;
}

// Distributed read function with fault tolerance
static int myfs_read(const char* path, char* buf, size_t size, off_t offset) {
    struct bb_state* state = BB_DATA;
    int num_nodes = state->num_nodes;
    int num_data_fragments = num_nodes - 1;
    
    fprintf(stderr, "[MYFS READ] ========== START ==========\n");
    fprintf(stderr, "[MYFS READ] path=%s, size=%zu, offset=%ld, nodes=%d\n", 
            path, size, offset, num_nodes);
    log_msg("\n[MYFS READ] path=%s, size=%zu, offset=%ld, num_nodes=%d\n", 
            path, size, offset, num_nodes);
    
    // CRITICAL: Get actual file size from metadata file
    // The 'size' parameter from FUSE may be larger (e.g., 4096), but we need
    // to use the actual file size to calculate the correct fragment_size
    char fpath[PATH_MAX];
    snprintf(fpath, PATH_MAX, "%s%s", state->rootdir, path);
    
    struct stat st;
    if (stat(fpath, &st) < 0) {
        fprintf(stderr, "[MYFS READ ERROR] Cannot stat file: %s\n", strerror(errno));
        log_msg("[MYFS READ ERROR] Cannot stat file %s: %s\n", fpath, strerror(errno));
        return -errno;
    }
    
    size_t file_size = st.st_size;
    fprintf(stderr, "[MYFS READ] File actual size: %zu bytes (requested: %zu bytes)\n", 
            file_size, size);
    log_msg("[MYFS READ] File actual size: %zu bytes (requested: %zu bytes)\n", 
            file_size, size);
    
    // Limit read size to actual file size
    if (offset >= (off_t)file_size) {
        fprintf(stderr, "[MYFS READ] Offset %ld >= file size %zu, returning 0\n", offset, file_size);
        return 0;  // EOF
    }
    
    size_t bytes_to_read = size;
    if (offset + size > file_size) {
        bytes_to_read = file_size - offset;
        fprintf(stderr, "[MYFS READ] Adjusted read size to %zu bytes (EOF)\n", bytes_to_read);
    }
    
    // Calculate fragment size based on ACTUAL FILE SIZE, not requested size
    size_t fragment_size = (file_size + num_data_fragments - 1) / num_data_fragments;
    fprintf(stderr, "[MYFS READ] Fragment size: %zu bytes (file_size=%zu, fragments=%d)\n", 
            fragment_size, file_size, num_data_fragments);
    
    // Allocate buffers for fragments
    fprintf(stderr, "[MYFS READ] Allocating memory: %d fragments × %zu bytes = %zu bytes total\n",
            num_nodes, fragment_size, num_nodes * fragment_size);
    
    char** fragments = (char**)malloc(num_nodes * sizeof(char*));
    if (!fragments) {
        fprintf(stderr, "[MYFS READ ERROR] Failed to allocate fragment pointer array\n");
        return -ENOMEM;
    }
    
    int* node_status = (int*)calloc(num_nodes, sizeof(int));  // 0=failed, 1=success
    if (!node_status) {
        fprintf(stderr, "[MYFS READ ERROR] Failed to allocate node_status array\n");
        free(fragments);
        return -ENOMEM;
    }
    
    // Initialize all pointers to NULL
    for (int i = 0; i < num_nodes; i++) {
        fragments[i] = NULL;
    }
    
    // Allocate each fragment buffer
    for (int i = 0; i < num_nodes; i++) {
        fragments[i] = (char*)calloc(fragment_size, 1);
        if (!fragments[i]) {
            fprintf(stderr, "[MYFS READ ERROR] Failed to allocate fragment %d buffer (%zu bytes)\n", 
                    i, fragment_size);
            // Clean up
            for (int j = 0; j < i; j++) {
                free(fragments[j]);
            }
            free(fragments);
            free(node_status);
            return -ENOMEM;
        }
    }
    fprintf(stderr, "[MYFS READ] ✓ Memory allocated successfully\n");
    
    // Try to read from all nodes
    fprintf(stderr, "[MYFS READ] Reading fragments from %d nodes...\n", num_nodes);
    for (int i = 0; i < num_nodes; i++) {
        request_header_t req;
        response_header_t resp;
        
        req.type = REQ_READ;
        strncpy(req.filename, path + 1, sizeof(req.filename) - 1);  // Skip leading '/'
        req.size = fragment_size;
        req.offset = 0;  // Always read from start of fragment file
        req.fragment_id = i;
        
        fprintf(stderr, "[MYFS READ] Node %d: Sending read request (file=%s, frag=%u, size=%zu, offset=%ld)...\n",
                i, req.filename, req.fragment_id, req.size, req.offset);
        
        // Send request (with retry on connection failure)
        int send_success = 0;
        for (int retry = 0; retry < 2; retry++) {
            if (send_all(state->nodes[i].socket_fd, &req, sizeof(req)) == sizeof(req)) {
                send_success = 1;
                break;
            }
            
            // Send failed, try to reconnect
            if (retry == 0) {
                fprintf(stderr, "[MYFS READ] ⚠ Node %d: Send request failed, attempting reconnect...\n", i);
                if (reconnect_to_node(i) < 0) {
                    fprintf(stderr, "[MYFS READ] ✗ Node %d: Reconnect failed\n", i);
                    break;
                }
                // Retry with new connection
            }
        }
        
        if (!send_success) {
            fprintf(stderr, "[MYFS READ] ✗ Node %d: Failed to send request after retry\n", i);
            log_msg("Failed to send read request to node %d\n", i);
            node_status[i] = 0;
            continue;
        }
        
        // Receive response
        if (recv(state->nodes[i].socket_fd, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) {
            fprintf(stderr, "[MYFS READ] ✗ Node %d: Failed to receive response\n", i);
            log_msg("Failed to receive response from node %d\n", i);
            node_status[i] = 0;
            continue;
        }
        
        if (resp.status != 0) {
            fprintf(stderr, "[MYFS READ] ✗ Node %d: Server returned error: status=%d, errno=%d\n", 
                    i, resp.status, resp.error_code);
            log_msg("Node %d returned error: %d\n", i, resp.error_code);
            node_status[i] = 0;
            continue;
        }
        
        // Receive data
        fprintf(stderr, "[MYFS READ] Node %d: Receiving data (%zu bytes)...\n", i, resp.size);
        if (resp.size > 0) {
            ssize_t received = recv(state->nodes[i].socket_fd, fragments[i], resp.size, MSG_WAITALL);
            if (received != (ssize_t)resp.size) {
                fprintf(stderr, "[MYFS READ] ✗ Node %d: Failed to receive data (expected %zu, got %zd)\n", 
                        i, resp.size, received);
                log_msg("Failed to receive data from node %d\n", i);
                node_status[i] = 0;
                continue;
            }
        }
        
        node_status[i] = 1;
        fprintf(stderr, "[MYFS READ] ✓ Node %d: Fragment read successfully (%zu bytes)\n", i, resp.size);
        log_msg("Successfully read fragment %d from node %d\n", i, i);
    }
    
    // Count successful reads
    int success_count = 0;
    int failed_node = -1;
    for (int i = 0; i < num_nodes; i++) {
        if (node_status[i]) {
            success_count++;
        } else {
            failed_node = i;
        }
    }
    
    fprintf(stderr, "[MYFS READ] Successfully read from %d/%d nodes\n", success_count, num_nodes);
    log_msg("[MYFS READ] Successfully read from %d/%d nodes\n", success_count, num_nodes);
    
    // Need at least n-1 fragments to reconstruct data
    if (success_count < num_data_fragments) {
        fprintf(stderr, "[MYFS READ ERROR] Not enough fragments to reconstruct data\n");
        log_msg("[MYFS READ ERROR] Not enough fragments to reconstruct data\n");
        free(node_status);
        for (int i = 0; i < num_nodes; i++) {
            free(fragments[i]);
        }
        free(fragments);
        return -EIO;
    }
    
    // If one node failed, reconstruct its fragment using XOR
    if (success_count == num_data_fragments && failed_node >= 0) {
        fprintf(stderr, "[MYFS READ] ⚠ Node %d failed, reconstructing using XOR...\n", failed_node);
        log_msg("[MYFS READ] Reconstructing fragment %d using XOR\n", failed_node);
        
        // Start with all zeros
        memset(fragments[failed_node], 0, fragment_size);
        
        // XOR all other fragments
        for (int i = 0; i < num_nodes; i++) {
            if (i != failed_node) {
                xor_buffers(fragments[failed_node], fragments[i], fragment_size);
            }
        }
        
        fprintf(stderr, "[MYFS READ] ✓ Fragment %d reconstructed successfully\n", failed_node);
        log_msg("[MYFS READ] Successfully reconstructed fragment %d\n", failed_node);
    }
    
    // Reconstruct original data from data fragments
    // Only reconstruct the portion we actually need to read
    memset(buf, 0, bytes_to_read);
    
    // Handle offset: we need to skip the first 'offset' bytes
    for (size_t i = 0; i < bytes_to_read; i++) {
        size_t file_pos = offset + i;  // Position in the original file
        int frag_idx = (file_pos / fragment_size) % num_data_fragments;
        size_t pos = file_pos % fragment_size;
        buf[i] = fragments[frag_idx][pos];
    }
    
    fprintf(stderr, "[MYFS READ] ✓ Read complete: %zu bytes (requested %zu, file_size %zu)\n", 
            bytes_to_read, size, file_size);
    log_msg("[MYFS READ] Reconstructed %zu bytes\n", bytes_to_read);
    
    // Cleanup
    free(node_status);
    for (int i = 0; i < num_nodes; i++) {
        free(fragments[i]);
    }
    free(fragments);
    
    return bytes_to_read;  // Return actual bytes read, not requested size
}

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come from /usr/include/fuse.h
//
/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int bb_getattr(const char *path, struct stat *statbuf)
{
    int retstat;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_getattr(path=\"%s\", statbuf=0x%08x)\n",
	  path, statbuf);
    bb_fullpath(fpath, path);

    retstat = log_syscall("lstat", lstat(fpath, statbuf), 0);
    
    log_stat(statbuf);
    
    return retstat;
}

/** Read the target of a symbolic link
 *
 * The buffer should be filled with a null terminated string.  The
 * buffer size argument includes the space for the terminating
 * null character.  If the linkname is too long to fit in the
 * buffer, it should be truncated.  The return value should be 0
 * for success.
 */
// Note the system readlink() will truncate and lose the terminating
// null.  So, the size passed to to the system readlink() must be one
// less than the size passed to bb_readlink()
// bb_readlink() code by Bernardo F Costa (thanks!)
int bb_readlink(const char *path, char *link, size_t size)
{
    int retstat;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_readlink(path=\"%s\", link=\"%s\", size=%d)\n",
	  path, link, size);
    bb_fullpath(fpath, path);

    retstat = log_syscall("readlink", readlink(fpath, link, size - 1), 0);
    if (retstat >= 0) {
	link[retstat] = '\0';
	retstat = 0;
	log_msg("    link=\"%s\"\n", link);
    }
    
    return retstat;
}

/** Create a file node
 *
 * There is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
// shouldn't that comment be "if" there is no.... ?
int bb_mknod(const char *path, mode_t mode, dev_t dev)
{
    int retstat;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_mknod(path=\"%s\", mode=0%3o, dev=%lld)\n",
	  path, mode, dev);
    bb_fullpath(fpath, path);
    
    // On Linux this could just be 'mknod(path, mode, dev)' but this
    // tries to be be more portable by honoring the quote in the Linux
    // mknod man page stating the only portable use of mknod() is to
    // make a fifo, but saying it should never actually be used for
    // that.
    if (S_ISREG(mode)) {
	retstat = log_syscall("open", open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode), 0);
	if (retstat >= 0)
	    retstat = log_syscall("close", close(retstat), 0);
    } else
	if (S_ISFIFO(mode))
	    retstat = log_syscall("mkfifo", mkfifo(fpath, mode), 0);
	else
	    retstat = log_syscall("mknod", mknod(fpath, mode, dev), 0);
    
    return retstat;
}

/** Create a directory */
int bb_mkdir(const char *path, mode_t mode)
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
    bb_fullpath(fpath, path);

    return log_syscall("mkdir", mkdir(fpath, mode), 0);
}

/** Remove a file */
int bb_unlink(const char *path)
{
    char fpath[PATH_MAX];
    
    log_msg("bb_unlink(path=\"%s\")\n",
	    path);
    bb_fullpath(fpath, path);

    return log_syscall("unlink", unlink(fpath), 0);
}

/** Remove a directory */
int bb_rmdir(const char *path)
{
    char fpath[PATH_MAX];
    
    log_msg("bb_rmdir(path=\"%s\")\n",
	    path);
    bb_fullpath(fpath, path);

    return log_syscall("rmdir", rmdir(fpath), 0);
}

/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
int bb_symlink(const char *path, const char *link)
{
    char flink[PATH_MAX];
    
    log_msg("\nbb_symlink(path=\"%s\", link=\"%s\")\n",
	    path, link);
    bb_fullpath(flink, link);

    return log_syscall("symlink", symlink(path, flink), 0);
}

/** Rename a file */
// both path and newpath are fs-relative
int bb_rename(const char *path, const char *newpath)
{
    char fpath[PATH_MAX];
    char fnewpath[PATH_MAX];
    
    log_msg("\nbb_rename(fpath=\"%s\", newpath=\"%s\")\n",
	    path, newpath);
    bb_fullpath(fpath, path);
    bb_fullpath(fnewpath, newpath);

    return log_syscall("rename", rename(fpath, fnewpath), 0);
}

/** Create a hard link to a file */
int bb_link(const char *path, const char *newpath)
{
    char fpath[PATH_MAX], fnewpath[PATH_MAX];
    
    log_msg("\nbb_link(path=\"%s\", newpath=\"%s\")\n",
	    path, newpath);
    bb_fullpath(fpath, path);
    bb_fullpath(fnewpath, newpath);

    return log_syscall("link", link(fpath, fnewpath), 0);
}

/** Change the permission bits of a file */
int bb_chmod(const char *path, mode_t mode)
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_chmod(fpath=\"%s\", mode=0%03o)\n",
	    path, mode);
    bb_fullpath(fpath, path);

    return log_syscall("chmod", chmod(fpath, mode), 0);
}

/** Change the owner and group of a file */
int bb_chown(const char *path, uid_t uid, gid_t gid)
  
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_chown(path=\"%s\", uid=%d, gid=%d)\n",
	    path, uid, gid);
    bb_fullpath(fpath, path);

    return log_syscall("chown", chown(fpath, uid, gid), 0);
}

/** Change the size of a file */
int bb_truncate(const char *path, off_t newsize)
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_truncate(path=\"%s\", newsize=%lld)\n",
	    path, newsize);
    bb_fullpath(fpath, path);

    return log_syscall("truncate", truncate(fpath, newsize), 0);
}

/** Change the access and/or modification times of a file */
/* note -- I'll want to change this as soon as 2.6 is in debian testing */
int bb_utime(const char *path, struct utimbuf *ubuf)
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_utime(path=\"%s\", ubuf=0x%08x)\n",
	    path, ubuf);
    bb_fullpath(fpath, path);

    return log_syscall("utime", utime(fpath, ubuf), 0);
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int bb_open(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    int fd;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_open(path\"%s\", fi=0x%08x)\n",
	    path, fi);
    bb_fullpath(fpath, path);
    
    // if the open call succeeds, my retstat is the file descriptor,
    // else it's -errno.  I'm making sure that in that case the saved
    // file descriptor is exactly -1.
    fd = log_syscall("open", open(fpath, fi->flags), 0);
    if (fd < 0)
	retstat = log_error("open");
	
    fi->fh = fd;

    log_fi(fi);
    
    return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
// I don't fully understand the documentation above -- it doesn't
// match the documentation for the read() system call which says it
// can return with anything up to the amount of data requested. nor
// with the fusexmp code which returns the amount of data also
// returned by read.
int bb_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg("\nbb_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);
    // no need to get fpath on this one, since I work from fi->fh not the path
    log_fi(fi);

    // Use distributed read if nodes are configured
    if (BB_DATA->num_nodes > 0) {
        return myfs_read(path, buf, size, offset);
    }
    
    // Fallback to local read
    return log_syscall("pread", pread(fi->fh, buf, size, offset), 0);
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
// As  with read(), the documentation above is inconsistent with the
// documentation for the write() system call.
int bb_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg("\nbb_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi
	    );
    // no need to get fpath on this one, since I work from fi->fh not the path
    log_fi(fi);

    // Use distributed write if nodes are configured
    if (BB_DATA->num_nodes > 0) {
        retstat = myfs_write(path, buf, size, offset);
        
        // CRITICAL: Update local metadata file size after successful write
        // This ensures getattr() returns the correct file size for subsequent reads
        if (retstat > 0 && fi->fh > 0) {
            off_t new_size = offset + retstat;
            struct stat st;
            
            // Get current file size
            if (fstat(fi->fh, &st) == 0) {
                // Only extend file if new write goes beyond current size
                if (st.st_size < new_size) {
                    if (ftruncate(fi->fh, new_size) == 0) {
                        log_msg("[MYFS] Updated local metadata file size: %lld -> %lld bytes\n", 
                                st.st_size, new_size);
                        fprintf(stderr, "[MYFS] ✓ Updated metadata file size to %lld bytes\n", new_size);
                    } else {
                        log_msg("[MYFS ERROR] Failed to update file size: %s\n", strerror(errno));
                        fprintf(stderr, "[MYFS ERROR] Failed to update metadata file size\n");
                    }
                }
            }
        }
        
        return retstat;
    }
    
    // Fallback to local write
    return log_syscall("pwrite", pwrite(fi->fh, buf, size, offset), 0);
}

/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 * Replaced 'struct statfs' parameter with 'struct statvfs' in
 * version 2.5
 */
int bb_statfs(const char *path, struct statvfs *statv)
{
    int retstat = 0;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_statfs(path=\"%s\", statv=0x%08x)\n",
	    path, statv);
    bb_fullpath(fpath, path);
    
    // get stats for underlying filesystem
    retstat = log_syscall("statvfs", statvfs(fpath, statv), 0);
    
    log_statvfs(statv);
    
    return retstat;
}

/** Possibly flush cached data
 *
 * BIG NOTE: This is not equivalent to fsync().  It's not a
 * request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor.  So if a
 * filesystem wants to return write errors in close() and the file
 * has cached dirty data, this is a good place to write back data
 * and return any errors.  Since many applications ignore close()
 * errors this is not always useful.
 *
 * NOTE: The flush() method may be called more than once for each
 * open().  This happens if more than one file descriptor refers
 * to an opened file due to dup(), dup2() or fork() calls.  It is
 * not possible to determine if a flush is final, so each flush
 * should be treated equally.  Multiple write-flush sequences are
 * relatively rare, so this shouldn't be a problem.
 *
 * Filesystems shouldn't assume that flush will always be called
 * after some writes, or that if will be called at all.
 *
 * Changed in version 2.2
 */
// this is a no-op in BBFS.  It just logs the call and returns success
int bb_flush(const char *path, struct fuse_file_info *fi)
{
    log_msg("\nbb_flush(path=\"%s\", fi=0x%08x)\n", path, fi);
    // no need to get fpath on this one, since I work from fi->fh not the path
    log_fi(fi);
    
    // Flush any buffered writes to storage nodes
    if (BB_DATA->num_nodes > 0) {
        int ret = myfs_flush_write_buffer(path);
        if (ret < 0) {
            log_msg("[MYFS] Flush failed: %d\n", ret);
            return ret;
        }
    }
	
    return 0;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int bb_release(const char *path, struct fuse_file_info *fi)
{
    log_msg("\nbb_release(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    log_fi(fi);
    
    // Flush any remaining buffered writes before closing
    if (BB_DATA->num_nodes > 0) {
        int ret = myfs_flush_write_buffer(path);
        if (ret < 0) {
            log_msg("[MYFS] Final flush on release failed: %d\n", ret);
            // Continue with close even if flush fails
        }
    }

    // We need to close the file.  Had we allocated any resources
    // (buffers etc) we'd need to free them here as well.
    return log_syscall("close", close(fi->fh), 0);
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 *
 * Changed in version 2.2
 */
int bb_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    log_msg("\nbb_fsync(path=\"%s\", datasync=%d, fi=0x%08x)\n",
	    path, datasync, fi);
    log_fi(fi);
    
    // some unix-like systems (notably freebsd) don't have a datasync call
#ifdef HAVE_FDATASYNC
    if (datasync)
	return log_syscall("fdatasync", fdatasync(fi->fh), 0);
    else
#endif	
	return log_syscall("fsync", fsync(fi->fh), 0);
}

#ifdef HAVE_SYS_XATTR_H
/** Note that my implementations of the various xattr functions use
    the 'l-' versions of the functions (eg bb_setxattr() calls
    lsetxattr() not setxattr(), etc).  This is because it appears any
    symbolic links are resolved before the actual call takes place, so
    I only need to use the system-provided calls that don't follow
    them */

/** Set extended attributes */
int bb_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_setxattr(path=\"%s\", name=\"%s\", value=\"%s\", size=%d, flags=0x%08x)\n",
	    path, name, value, size, flags);
    bb_fullpath(fpath, path);

    return log_syscall("lsetxattr", lsetxattr(fpath, name, value, size, flags), 0);
}

/** Get extended attributes */
int bb_getxattr(const char *path, const char *name, char *value, size_t size)
{
    int retstat = 0;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_getxattr(path = \"%s\", name = \"%s\", value = 0x%08x, size = %d)\n",
	    path, name, value, size);
    bb_fullpath(fpath, path);

    retstat = log_syscall("lgetxattr", lgetxattr(fpath, name, value, size), 0);
    if (retstat >= 0)
	log_msg("    value = \"%s\"\n", value);
    
    return retstat;
}

/** List extended attributes */
int bb_listxattr(const char *path, char *list, size_t size)
{
    int retstat = 0;
    char fpath[PATH_MAX];
    char *ptr;
    
    log_msg("\nbb_listxattr(path=\"%s\", list=0x%08x, size=%d)\n",
	    path, list, size
	    );
    bb_fullpath(fpath, path);

    retstat = log_syscall("llistxattr", llistxattr(fpath, list, size), 0);
    if (retstat >= 0) {
	log_msg("    returned attributes (length %d):\n", retstat);
	if (list != NULL)
	    for (ptr = list; ptr < list + retstat; ptr += strlen(ptr)+1)
		log_msg("    \"%s\"\n", ptr);
	else
	    log_msg("    (null)\n");
    }
    
    return retstat;
}

/** Remove extended attributes */
int bb_removexattr(const char *path, const char *name)
{
    char fpath[PATH_MAX];
    
    log_msg("\nbb_removexattr(path=\"%s\", name=\"%s\")\n",
	    path, name);
    bb_fullpath(fpath, path);

    return log_syscall("lremovexattr", lremovexattr(fpath, name), 0);
}
#endif

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int bb_opendir(const char *path, struct fuse_file_info *fi)
{
    DIR *dp;
    int retstat = 0;
    char fpath[PATH_MAX];
    
    log_msg("\nbb_opendir(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    bb_fullpath(fpath, path);

    // since opendir returns a pointer, takes some custom handling of
    // return status.
    dp = opendir(fpath);
    log_msg("    opendir returned 0x%p\n", dp);
    if (dp == NULL)
	retstat = log_error("bb_opendir opendir");
    
    fi->fh = (intptr_t) dp;
    
    log_fi(fi);
    
    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */

int bb_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
    int retstat = 0;
    DIR *dp;
    struct dirent *de;
    
    log_msg("\nbb_readdir(path=\"%s\", buf=0x%08x, filler=0x%08x, offset=%lld, fi=0x%08x)\n",
	    path, buf, filler, offset, fi);
    // once again, no need for fullpath -- but note that I need to cast fi->fh
    dp = (DIR *) (uintptr_t) fi->fh;

    // Every directory contains at least two entries: . and ..  If my
    // first call to the system readdir() returns NULL I've got an
    // error; near as I can tell, that's the only condition under
    // which I can get an error from readdir()
    de = readdir(dp);
    log_msg("    readdir returned 0x%p\n", de);
    if (de == 0) {
	retstat = log_error("bb_readdir readdir");
	return retstat;
    }

    // This will copy the entire directory into the buffer.  The loop exits
    // when either the system readdir() returns NULL, or filler()
    // returns something non-zero.  The first case just means I've
    // read the whole directory; the second means the buffer is full.
    do {
	log_msg("calling filler with name %s\n", de->d_name);
	if (filler(buf, de->d_name, NULL, 0) != 0) {
	    log_msg("    ERROR bb_readdir filler:  buffer full");
	    return -ENOMEM;
	}
    } while ((de = readdir(dp)) != NULL);
    
    log_fi(fi);
    
    return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int bb_releasedir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg("\nbb_releasedir(path=\"%s\", fi=0x%08x)\n",
	    path, fi);
    log_fi(fi);
    
    closedir((DIR *) (uintptr_t) fi->fh);
    
    return retstat;
}

/** Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data
 *
 * Introduced in version 2.3
 */
// when exactly is this called?  when a user calls fsync and it
// happens to be a directory? ??? >>> I need to implement this...
int bb_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg("\nbb_fsyncdir(path=\"%s\", datasync=%d, fi=0x%08x)\n",
	    path, datasync, fi);
    log_fi(fi);
    
    return retstat;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *bb_init(struct fuse_conn_info *conn)
{
    log_msg("\nbb_init()\n");
    
    log_conn(conn);
    log_fuse_context(fuse_get_context());
    
    // Initialize connections to storage nodes
    if (BB_DATA->num_nodes > 0) {
        log_msg("Initializing connections to %d nodes\n", BB_DATA->num_nodes);
        if (init_node_connections() < 0) {
            fprintf(stderr, "Failed to initialize node connections\n");
        } else {
            log_msg("Successfully connected to all nodes\n");
        }
    }
    
    return BB_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void bb_destroy(void *userdata)
{
    log_msg("\nbb_destroy(userdata=0x%08x)\n", userdata);
}

/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 *
 * Introduced in version 2.5
 */
int bb_access(const char *path, int mask)
{
    int retstat = 0;
    char fpath[PATH_MAX];
   
    log_msg("\nbb_access(path=\"%s\", mask=0%o)\n",
	    path, mask);
    bb_fullpath(fpath, path);
    
    retstat = access(fpath, mask);
    
    if (retstat < 0)
	retstat = log_error("bb_access access");
    
    return retstat;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
// Not implemented.  I had a version that used creat() to create and
// open the file, which it turned out opened the file write-only.

/**
 * Change the size of an open file
 *
 * This method is called instead of the truncate() method if the
 * truncation was invoked from an ftruncate() system call.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the truncate() method will be
 * called instead.
 *
 * Introduced in version 2.5
 */
int bb_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg("\nbb_ftruncate(path=\"%s\", offset=%lld, fi=0x%08x)\n",
	    path, offset, fi);
    log_fi(fi);
    
    retstat = ftruncate(fi->fh, offset);
    if (retstat < 0)
	retstat = log_error("bb_ftruncate ftruncate");
    
    return retstat;
}

/**
 * Get attributes from an open file
 *
 * This method is called instead of the getattr() method if the
 * file information is available.
 *
 * Currently this is only called after the create() method if that
 * is implemented (see above).  Later it may be called for
 * invocations of fstat() too.
 *
 * Introduced in version 2.5
 */
int bb_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg("\nbb_fgetattr(path=\"%s\", statbuf=0x%08x, fi=0x%08x)\n",
	    path, statbuf, fi);
    log_fi(fi);

    // On FreeBSD, trying to do anything with the mountpoint ends up
    // opening it, and then using the FD for an fgetattr.  So in the
    // special case of a path of "/", I need to do a getattr on the
    // underlying root directory instead of doing the fgetattr().
    if (!strcmp(path, "/"))
	return bb_getattr(path, statbuf);
    
    retstat = fstat(fi->fh, statbuf);
    if (retstat < 0)
	retstat = log_error("bb_fgetattr fstat");
    
    log_stat(statbuf);
    
    return retstat;
}

struct fuse_operations bb_oper = {
  .getattr = bb_getattr,
  .readlink = bb_readlink,
  // no .getdir -- that's deprecated
  .getdir = NULL,
  .mknod = bb_mknod,
  .mkdir = bb_mkdir,
  .unlink = bb_unlink,
  .rmdir = bb_rmdir,
  .symlink = bb_symlink,
  .rename = bb_rename,
  .link = bb_link,
  .chmod = bb_chmod,
  .chown = bb_chown,
  .truncate = bb_truncate,
  .utime = bb_utime,
  .open = bb_open,
  .read = bb_read,
  .write = bb_write,
  /** Just a placeholder, don't set */ // huh???
  .statfs = bb_statfs,
  .flush = bb_flush,
  .release = bb_release,
  .fsync = bb_fsync,
  
#ifdef HAVE_SYS_XATTR_H
  .setxattr = bb_setxattr,
  .getxattr = bb_getxattr,
  .listxattr = bb_listxattr,
  .removexattr = bb_removexattr,
#endif
  
  .opendir = bb_opendir,
  .readdir = bb_readdir,
  .releasedir = bb_releasedir,
  .fsyncdir = bb_fsyncdir,
  .init = bb_init,
  .destroy = bb_destroy,
  .access = bb_access,
  .ftruncate = bb_ftruncate,
  .fgetattr = bb_fgetattr
};

void bb_usage()
{
    fprintf(stderr, "usage:  bbfs [FUSE and mount options] rootDir mountPoint [host1:port1 host2:port2 ...]\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  bbfs rootdir mountdir\n");
    fprintf(stderr, "  bbfs rootdir mountdir 10.0.1.5:8001 10.0.1.6:8002 10.0.1.7:8003\n");
    abort();
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct bb_state *bb_data;

    // bbfs doesn't do any access checking on its own (the comment
    // blocks in fuse.h mention some of the functions that need
    // accesses checked -- but note there are other functions, like
    // chown(), that also need checking!).  Since running bbfs as root
    // will therefore open Metrodome-sized holes in the system
    // security, we'll check if root is trying to mount the filesystem
    // and refuse if it is.  The somewhat smaller hole of an ordinary
    // user doing it with the allow_other flag is still there because
    // I don't want to parse the options string.
    if ((getuid() == 0) || (geteuid() == 0)) {
    	fprintf(stderr, "Running BBFS as root opens unnacceptable security holes\n");
    	return 1;
    }

    // See which version of fuse we're running
    fprintf(stderr, "Fuse library version %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
    
    // Perform some sanity checking on the command line:  make sure
    // there are enough arguments, and that neither of the last two
    // start with a hyphen (this will break if you actually have a
    // rootpoint or mountpoint whose name starts with a hyphen, but so
    // will a zillion other programs)
    if (argc < 3)
	bb_usage();

    bb_data = malloc(sizeof(struct bb_state));
    if (bb_data == NULL) {
	perror("main calloc");
	abort();
    }
    
    // Initialize node count to 0
    bb_data->num_nodes = 0;

    // Find where node specifications start (after rootdir and mountpoint)
    // We expect: bbfs [options] rootdir mountpoint node1 node2 ...
    int node_start_idx = -1;
    int non_option_count = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && strchr(argv[i], ':') == NULL) {
            // This is rootdir or mountpoint (no colon means not a node spec)
            non_option_count++;
            if (non_option_count == 2) {
                // After rootdir and mountpoint, nodes start
                node_start_idx = i + 1;
                break;
            }
        }
    }
    
    fprintf(stderr, "[MYFS] Parsed arguments: node_start_idx=%d\n", node_start_idx);
    
    // Parse node specifications (host:port format)
    if (node_start_idx > 0 && node_start_idx < argc) {
        for (int i = node_start_idx; i < argc && bb_data->num_nodes < MAX_NODES; i++) {
            if (argv[i][0] == '-') continue;  // Skip options
            
            char* colon = strchr(argv[i], ':');
            if (colon != NULL) {
                // Parse host:port
                size_t host_len = colon - argv[i];
                if (host_len < sizeof(bb_data->nodes[0].host)) {
                    strncpy(bb_data->nodes[bb_data->num_nodes].host, argv[i], host_len);
                    bb_data->nodes[bb_data->num_nodes].host[host_len] = '\0';
                    bb_data->nodes[bb_data->num_nodes].port = atoi(colon + 1);
                    bb_data->nodes[bb_data->num_nodes].socket_fd = -1;
                    
                    fprintf(stderr, "Node %d: %s:%d\n", bb_data->num_nodes,
                            bb_data->nodes[bb_data->num_nodes].host,
                            bb_data->nodes[bb_data->num_nodes].port);
                    
                    bb_data->num_nodes++;
                }
            }
        }
    }
    
    fprintf(stderr, "Configured %d storage nodes\n", bb_data->num_nodes);
    
    // Find rootdir and mountpoint
    int rootdir_idx = -1, mountpoint_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && strchr(argv[i], ':') == NULL) {
            if (rootdir_idx == -1) {
                rootdir_idx = i;
            } else if (mountpoint_idx == -1) {
                mountpoint_idx = i;
                break;
            }
        }
    }
    
    if (rootdir_idx == -1 || mountpoint_idx == -1) {
        bb_usage();
    }

    // Pull the rootdir out of the argument list and save it in my internal data
    bb_data->rootdir = realpath(argv[rootdir_idx], NULL);
    
    // Build new argv without node specifications
    char** new_argv = malloc(argc * sizeof(char*));
    int new_argc = 0;
    new_argv[new_argc++] = argv[0];  // Program name
    
    // Copy FUSE options and mountpoint (but skip rootdir and node specs)
    for (int i = 1; i < argc; i++) {
        if (i == rootdir_idx) continue;  // Skip rootdir (we'll handle it separately)
        if (strchr(argv[i], ':') != NULL) continue;  // Skip node specs
        new_argv[new_argc++] = argv[i];
    }
    
    bb_data->logfile = log_open();
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, rootdir=%s\n", bb_data->rootdir);
    fuse_stat = fuse_main(new_argc, new_argv, &bb_oper, bb_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    
    free(new_argv);
    return fuse_stat;
}
