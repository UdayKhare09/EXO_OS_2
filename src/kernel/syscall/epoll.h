/* syscall/epoll.h — epoll syscall declarations */
#pragma once
#include <stdint.h>
#include "syscall.h"

int64_t sys_epoll_create1(int flags);
int64_t sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *event);
int64_t sys_epoll_wait(int epfd, epoll_event_t *events, int maxevents, int timeout);
int64_t sys_epoll_pwait(int epfd, epoll_event_t *events, int maxevents, int timeout,
                        const uint64_t *sigmask, uint64_t sigsetsize);
