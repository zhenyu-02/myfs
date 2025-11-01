/*
  Copyright (C) 2012 Joseph J. Pfeiffer, Jr., Ph.D. <pfeiffer@cs.nmsu.edu>

  This program can be distributed under the terms of the GNU GPLv3.
  See the file COPYING.

  There are a couple of symbols that need to be #defined before
  #including all the headers.
*/

#ifndef _PARAMS_H_
#define _PARAMS_H_

// The FUSE API has been changed a number of times.  So, our code
// needs to define the version of the API that we assume.  As of this
// writing, the most current API version is 26
#define FUSE_USE_VERSION 26

// need this to get pwrite().  I have to use setvbuf() instead of
// setlinebuf() later in consequence.
#define _XOPEN_SOURCE 500

// maintain bbfs state in here
#include <limits.h>
#include <stdio.h>
#include <pthread.h>

#define MAX_NODES 10

// Node information
typedef struct {
    char host[256];
    int port;
    int socket_fd;
    pthread_mutex_t socket_mutex;  // Mutex for thread-safe socket access
} node_info_t;

struct bb_state {
    FILE *logfile;
    char *rootdir;
    int num_nodes;              // Number of storage nodes
    node_info_t nodes[MAX_NODES]; // Node information array
    pthread_mutex_t nodes_mutex;    // Global mutex for nodes operations
};
#define BB_DATA ((struct bb_state *) fuse_get_context()->private_data)

#endif
