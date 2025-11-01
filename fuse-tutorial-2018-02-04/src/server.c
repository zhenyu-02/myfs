/*
  MYFS Storage Server
  Each storage node runs this server to handle read/write requests
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <limits.h>

#include "protocol.h"

// Global storage directory
char storage_dir[PATH_MAX];

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

// Function to handle client request
void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);
    
    request_header_t req;
    response_header_t resp;
    // Initialize response to avoid sending garbage
    memset(&resp, 0, sizeof(resp));
    char filepath[PATH_MAX];
    char* data_buffer = NULL;
    
    while (1) {
        // Read request header
        ssize_t n = recv(client_sock, &req, sizeof(req), MSG_WAITALL);
        if (n <= 0) {
            if (n < 0) perror("recv request header");
            break;
        }
        
        // Build file path
        snprintf(filepath, PATH_MAX, "%s/%s.frag%u", storage_dir, req.filename, req.fragment_id);
        
        printf("[Server] Request type=%d, file=%s, size=%zu, offset=%ld\n", 
               req.type, filepath, req.size, req.offset);
        
        // Always reset response for each request
        memset(&resp, 0, sizeof(resp));
        
        if (req.type == REQ_WRITE) {
            // Allocate buffer for data
            data_buffer = (char*)malloc(req.size);
            if (!data_buffer) {
                resp.status = -1;
                resp.error_code = ENOMEM;
                resp.size = 0;
                send_all(client_sock, &resp, sizeof(resp));
                continue;
            }
            
            // Receive data
            n = recv(client_sock, data_buffer, req.size, MSG_WAITALL);
            if (n != (ssize_t)req.size) {
                perror("recv data");
                resp.status = -1;
                resp.error_code = errno;
                resp.size = 0;
                send_all(client_sock, &resp, sizeof(resp));
                free(data_buffer);
                continue;
            }
            
            // Open/create file
            // Use O_TRUNC when offset is 0 to ensure we start fresh
            int flags = O_WRONLY | O_CREAT;
            if (req.offset == 0) {
                flags |= O_TRUNC;  // Clear file when writing from beginning
            }
            int fd = open(filepath, flags, 0644);
            if (fd < 0) {
                perror("open file for write");
                resp.status = -1;
                resp.error_code = errno;
                resp.size = 0;
                send_all(client_sock, &resp, sizeof(resp));
                free(data_buffer);
                continue;
            }
            
            // Write data at offset
            ssize_t written = pwrite(fd, data_buffer, req.size, req.offset);
            close(fd);
            free(data_buffer);
            
            if (written != (ssize_t)req.size) {
                perror("pwrite");
                resp.status = -1;
                resp.error_code = errno;
                resp.size = 0;
            } else {
                resp.status = 0;
                resp.error_code = 0;
                resp.size = written;
            }
            
            send_all(client_sock, &resp, sizeof(resp));
            
        } else if (req.type == REQ_READ) {
            // Open file
            int fd = open(filepath, O_RDONLY);
            if (fd < 0) {
                perror("open file for read");
                resp.status = -1;
                resp.error_code = errno;
                resp.size = 0;
                send_all(client_sock, &resp, sizeof(resp));
                continue;
            }
            
            // Allocate buffer
            data_buffer = (char*)malloc(req.size);
            if (!data_buffer) {
                resp.status = -1;
                resp.error_code = ENOMEM;
                resp.size = 0;
                send_all(client_sock, &resp, sizeof(resp));
                close(fd);
                continue;
            }
            
            // Read data at offset
            ssize_t nread = pread(fd, data_buffer, req.size, req.offset);
            close(fd);
            
            if (nread < 0) {
                perror("pread");
                resp.status = -1;
                resp.error_code = errno;
                resp.size = 0;
                send_all(client_sock, &resp, sizeof(resp));
                free(data_buffer);
                continue;
            }
            
            // Send response
            resp.status = 0;
            resp.error_code = 0;
            resp.size = nread;
            send_all(client_sock, &resp, sizeof(resp));
            
            // Send data
            if (nread > 0) {
                send_all(client_sock, data_buffer, nread);
            }
            
            free(data_buffer);
            
        } else if (req.type == REQ_DELETE) {
            // Delete file
            if (unlink(filepath) < 0) {
                perror("unlink");
                resp.status = -1;
                resp.error_code = errno;
            } else {
                resp.status = 0;
                resp.error_code = 0;
            }
            resp.size = 0;
            send_all(client_sock, &resp, sizeof(resp));
        }
    }
    
    close(client_sock);
    printf("[Server] Client disconnected\n");
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <storage_dir>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    strncpy(storage_dir, argv[2], PATH_MAX - 1);
    
    // Create storage directory if not exists
    mkdir(storage_dir, 0755);
    
    printf("[Server] Starting on port %d, storage dir: %s\n", port, storage_dir);
    
    // Create socket
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }
    
    // Set socket option to reuse address
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    
    // Listen
    if (listen(server_sock, 10) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }
    
    printf("[Server] Listening on port %d...\n", port);
    
    // Accept connections
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int* client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (*client_sock < 0) {
            perror("accept");
            free(client_sock);
            continue;
        }
        
        printf("[Server] Client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // Create thread to handle client
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, client_sock) != 0) {
            perror("pthread_create");
            close(*client_sock);
            free(client_sock);
            continue;
        }
        
        pthread_detach(thread);
    }
    
    close(server_sock);
    return 0;
}

