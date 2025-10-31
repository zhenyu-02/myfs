bin_PROGRAMS = bbfs server
bbfs_SOURCES = bbfs.c log.c log.h params.h protocol.h
server_SOURCES = server.c protocol.h
AM_CFLAGS = @FUSE_CFLAGS@
bbfs_LDADD = @FUSE_LIBS@
server_LDADD = -lpthread
