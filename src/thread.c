/*
 *  Beansdb - A high available distributed key-value storage system:
 *
 *      http://beansdb.googlecode.com
 *
 *  Copyright 2010 Douban Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Davies Liu <davies.liu@gmail.com>
 *
 */

#include "beansdb.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <pthread.h>

// 事件循环
typedef struct EventLoop {
    conn* conns[AE_SETSIZE]; // AE_SETSIZE (1024*60)
    int   fired[AE_SETSIZE];
    int   nready;
    void *apidata;
} EventLoop;

/* Lock for connection freelist */
static pthread_mutex_t conn_lock;

/* Lock for item buffer freelist */
static pthread_mutex_t ibuffer_lock;

static EventLoop loop;
static pthread_mutex_t leader;

/*
 * Pulls a conn structure from the freelist, if one is available.
 * 线程安全的获取一个连接
 */
conn *mt_conn_from_freelist() {
    conn *c;
    pthread_mutex_lock(&conn_lock); // 加锁
    c = do_conn_from_freelist();
    pthread_mutex_unlock(&conn_lock); // 释放锁
    return c;
}

/*
 * Adds a conn structure to the freelist.
 *
 * Returns 0 on success, 1 if the structure couldn't be added.
 * 线程安全的归还一个连接
 */
bool mt_conn_add_to_freelist(conn *c) {
    bool result;

    pthread_mutex_lock(&conn_lock);
    result = do_conn_add_to_freelist(c);
    pthread_mutex_unlock(&conn_lock);

    return result;
}

/*
 * Pulls a item buffer from the freelist, if one is available.
 */

item *mt_item_from_freelist(void) {
    item *it;
    pthread_mutex_lock(&ibuffer_lock);
    it = do_item_from_freelist();
    pthread_mutex_unlock(&ibuffer_lock);
    return it;
}

/*
 * Adds a item buffer to the freelist.
 *
 * Returns 0 on success, 1 if the buffer couldn't be added.
 */
int mt_item_add_to_freelist(item *it){
    int result;

    pthread_mutex_lock(&ibuffer_lock);
    result = do_item_add_to_freelist(it);
    pthread_mutex_unlock(&ibuffer_lock);

    return result;
}

/******************************* GLOBAL STATS ******************************/

void mt_stats_lock() {
}

void mt_stats_unlock() {
}

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
//  不同的异步IO
#ifdef HAVE_EPOLL
#include "ae_epoll.c"
#else
    #ifdef HAVE_KQUEUE
    #include "ae_kqueue.c"
    #else
    #include "ae_select.c"
    #endif
#endif

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of event handler threads to spawn
 */
// 首先初始化事件模型
void thread_init(int nthreads) {
    int         i;
    pthread_mutex_init(&ibuffer_lock, NULL);
    pthread_mutex_init(&conn_lock, NULL);
    pthread_mutex_init(&leader, NULL);

    memset(&loop, 0, sizeof(loop));
    if (aeApiCreate(&loop) == -1) {
        exit(1);
    }
}

int add_event(int fd, int mask, conn *c)
{
    if (fd >= AE_SETSIZE) {
        fprintf(stderr, "fd is too large: %d\n", fd);
        return AE_ERR;
    }
    assert(loop.conns[fd] == NULL);
    loop.conns[fd] = c;
    // 添加事件
    if (aeApiAddEvent(&loop, fd, mask) == -1){ // I/O 多路复用，比如 epoll，select，kqueue
        loop.conns[fd] = NULL;
        return AE_ERR;
    }
    return AE_OK;
}

int update_event(int fd, int mask, conn *c)
{
    loop.conns[fd] = c;
    if (aeApiUpdateEvent(&loop, fd, mask) == -1){
        loop.conns[fd] = NULL;
        return AE_ERR;
    }
    return AE_OK;
}

int delete_event(int fd)
{
    if (fd >= AE_SETSIZE) return -1;
    loop.conns[fd] = NULL;
    if (aeApiDelEvent(&loop, fd) == -1)
        return -1;
    return 0;
}

// leader/flower模式
static void *worker_main(void *arg) {
    pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, 0);

    // struct timeval {
    //     time_t       tv_sec;     /* seconds */
    //     suseconds_t   tv_usec; /* microseconds */
    // };
    // tv_usec 的说明为时间的毫秒部分
    struct timeval tv = {1, 0};
    // daemon_quit 退出标识。 0 没有退出、1退出
    while (!daemon_quit) {
        pthread_mutex_lock(&leader);

AGAIN:
        while(loop.nready == 0 && daemon_quit == 0)
            // 取出事件
            loop.nready = aeApiPoll(&loop, &tv);
        if (daemon_quit) {
            pthread_mutex_unlock(&leader);
            break;
        }

        // 遍历事件
        loop.nready --;
        int fd = loop.fired[loop.nready];
        conn *c = loop.conns[fd];
        if (c == NULL){
            fprintf(stderr, "Bug: conn %d should not be NULL\n", fd);
            delete_event(fd);
            close(fd);
            goto AGAIN;
        }
        //loop.conns[fd] = NULL;
        pthread_mutex_unlock(&leader);

        if (drive_machine(c)) {
            if (update_event(fd, c->ev_flags, c)) conn_close(c);
        }
    }
    return NULL;
}

void loop_run(int nthread)
{
    int i, ret;
    pthread_attr_t  attr;
    pthread_attr_init(&attr);
    // pthread_t pthread_t用于声明线程ID。 typedef unsigned long int pthread_t;
    pthread_t* tids = malloc(sizeof(pthread_t) * nthread);

    for (i=0; i<nthread - 1; i++) {
        // pthread_create是类Unix操作系统（Unix、Linux、Mac OS X等）的创建线程的函数。
        // 第一个参数为指向线程标识符的指针。
        // 第二个参数用来设置线程属性。
        // 第三个参数是线程运行函数的起始地址。
        if ((ret = pthread_create(tids + i, &attr, worker_main, NULL)) != 0) {
            fprintf(stderr, "Can't create thread: %s\n",
                    strerror(ret));
            exit(1);
        }
    }

    worker_main(NULL);

    // wait workers to stop
    for (i=0; i<nthread - 1; i++) {
        // http://flyingv.iteye.com/blog/776476
        (void) pthread_join(tids[i], NULL);
        pthread_detach(tids[i]);
    }
    free(tids);

    aeApiFree(&loop);
}
