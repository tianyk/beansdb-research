/* Kqueue(2)-based ae.c module
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
 * Released under the BSD license. See the COPYING file for more info. */

// 对kqueue的封装

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

typedef struct aeApiState {
    int kqfd;
    struct kevent events[AE_SETSIZE];
} aeApiState;

static int aeApiCreate(EventLoop *eventLoop) {
    aeApiState *state = malloc(sizeof(aeApiState));

    if (!state) return -1;
    state->kqfd = kqueue(); // 初始化一个kqueue
    if (state->kqfd == -1) return -1;
    eventLoop->apidata = state;

    return 0;
}

static void aeApiFree(EventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    free(state);
}

static int aeApiAddEvent(EventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

static int aeApiUpdateEvent(EventLoop *eventLoop, int fd, int mask) {
    return aeApiAddEvent(eventLoop, fd, mask);
}

static int aeApiDelEvent(EventLoop *eventLoop, int fd) {
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

    EV_SET(&ke, fd, EVFILT_READ | EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    return 0;
}

static int aeApiPoll(EventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    if (tvp != NULL) {
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        retval = kevent(state->kqfd, NULL, 0, state->events, AE_SETSIZE, &timeout);
    } else {
        retval = kevent(state->kqfd, NULL, 0, state->events, AE_SETSIZE, NULL);
    }

    if (retval > 0) {
        int j;

        numevents = retval;
        for(j = 0; j < numevents; j++) {
            int mask = 0;
            struct kevent *e = state->events+j;

            if (e->filter == EVFILT_READ) mask |= AE_READABLE;
            if (e->filter == EVFILT_WRITE) mask |= AE_WRITABLE;
            eventLoop->fired[j] = e->ident;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}
