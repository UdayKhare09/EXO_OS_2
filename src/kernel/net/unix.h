/* net/unix.h — AF_UNIX (Unix Domain) socket implementation */
#pragma once
#include "net/socket.h"
typedef struct task task_t;

/* Initialise AF_UNIX subsystem (call once at boot) */
void unix_socket_init(void);

/* Create and attach AF_UNIX proto_ops to an existing socket_t.
 * Called from socket_create() when domain == AF_UNIX.
 * Returns 0 on success, -errno on failure. */
int unix_socket_create(socket_t *sk, int type);

/* Create a connected AF_UNIX socket pair (socketpair(2)).
 * sv[0] and sv[1] are filled with the two new fds.
 * Returns 0 on success, -errno on failure. */
int unix_socketpair(int type, int sv[2]);

/* SCM_RIGHTS helpers: push/pop file_t references through a unix socket's queue.
 * unix_push_files: enqueue up to 'count' file_t* refs into dest (src task holds fds).
 * unix_pop_files:  dequeue pending file_t* refs, install into receiver's fd table,
 *                  store new fd numbers in out_fds[0..n-1], return n. */
typedef struct unix_sock unix_sock_t_pub; /* opaque forward ref */
void unix_push_files(void *dest_unix_sock, file_t **files, int count);
int  unix_pop_files(void *src_unix_sock, task_t *receiver, int *out_fds, int maxcount);

/* Get the opaque unix_sock handle from a socket_t (for SCM ancdata access) */
void *unix_sock_from_socket(socket_t *sk);
/* Get the peer's unix_sock handle (NULL if not connected) */
void *unix_sock_get_peer(void *us);
