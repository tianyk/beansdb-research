/* Linux epoll(2) based ae.c module
 * Copyright (C) 2009-2010 Salvatore Sanfilippo - antirez@gmail.com
 * Released under the BSD license. See the COPYING file for more info. */

// 对epoll的封装
// http://www.cnblogs.com/Bozh/archive/2012/04/23/2466951.html

#include <sys/epoll.h>
#include <errno.h>

typedef struct aeApiState {
    int epfd; // epoll fd
    struct epoll_event events[AE_SETSIZE];
} aeApiState;

static int aeApiCreate(EventLoop *eventLoop) {
    // 初始化state
    aeApiState *state = malloc(sizeof(aeApiState));
    if (!state) return -1;
    state->epfd = epoll_create(1024); /* 1024 is just an hint for the kernel */
    if (state->epfd == -1) return -1;

    eventLoop->apidata = state;
    return 0;
}

static void aeApiFree(EventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    free(state);
}

static int aeApiAddEvent(EventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    ee.events = EPOLLONESHOT;
    if (mask & AE_READABLE) ee.events |= EPOLLIN; // EPOLLIN = 1
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT; // EPOLLOUT = 4
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    // 第一个参数是由 epoll_create 返回的描述符
    // 第二个参数是由宏定义的几个值
    // EPOLL_CTL_ADD：类似于 select 的 FD_SET() ，将一个描述符加入到epoll 监听队列中
    // EPOLL_CTL_MOD：修改已经注册的fd的事件类型
    // EPOLL_CTL_DEL：将一个描述符从epoll 监听队列中删除
    // 第三个参数是需要加入的描述符
    // epoll_event 结构体里面的events 表示的是返回的事件类型或者是加入时候的事件类型。也有可能是带外数据或者错误等，它由几个宏定义：
    // EPOLLIN ：文件描述符上的读事件
    // EPOLLOUT：文件描述符上的写事件
    // EPOLLPRI：描述符有紧急的数据可读（这里应该表示有带外数据到来）；
    // EPOLLERR：描述符发生错误；
    // EPOLLHUP：描述符被挂断；
    // EPOLLET： 边缘触发(Edge Triggered)模式
    // EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里

    if (epoll_ctl(state->epfd, EPOLL_CTL_ADD,fd,&ee) == -1 && errno != EEXIST) {
        fprintf(stderr, "epoll_ctl(%d,%d) failed: %d\n", EPOLL_CTL_ADD,fd,errno);
        return -1;
    }
    return 0;
}

static int aeApiUpdateEvent(EventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    ee.events = EPOLLONESHOT;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    if (epoll_ctl(state->epfd, EPOLL_CTL_MOD,fd,&ee) == -1) {
        fprintf(stderr, "epoll_ctl(%d,%d) failed: %d\n", EPOLL_CTL_ADD,fd,errno);
        return -1;
    }
    return 0;
}

static int aeApiDelEvent(EventLoop *eventLoop, int fd) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;

    ee.events = 0;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    /* Note, Kernel < 2.6.9 requires a non null event pointer even for
     * EPOLL_CTL_DEL. */
    if ( epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee) == -1
            && errno != ENOENT && errno != EBADF) {
        fprintf(stderr, "epoll_ctl(%d,%d) failed: %d\n", EPOLL_CTL_DEL,fd,errno);
        return -1;
    }
    return 0;
}

// struct timeval {
//     time_t       tv_sec;     /* seconds */
//     suseconds_t   tv_usec; /* microseconds */
// };
// tv_usec 的说明为时间的毫秒部分
int aeApiPoll(EventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    // http://www.cnblogs.com/Bozh/archive/2012/04/23/2466951.html
    // int epoll_wait(int epfd, struct epoll_event * events, int maxevents, int timeout);
    // 第一个参数是epoll的描述符，
    // 第二个参数是一个指向 struct epoll_event  的指针，这里需要传入的是一个数组，epoll_event 类型.
    // 第三个，最大的监听事件数组值。
    // 第四个是超时时间，对于Nginx或者很多如libevent 的超时时间管理是利用红黑树和最小堆来管理的，
    // 很巧妙的方式，以后写一篇博文介绍，这里只需要知道timeout 是 epoll_wait 的阻塞的最大值，如果超过这个值不管是否有事件都返回，
    // 0表示立即返回，即有无事件都返回，-1 是永久阻塞。
    retval = epoll_wait(state->epfd,state->events,AE_SETSIZE,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            // EPOLLIN | EPOLLOUT 读写
            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            eventLoop->fired[j] = e->data.fd;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "epoll";
}
