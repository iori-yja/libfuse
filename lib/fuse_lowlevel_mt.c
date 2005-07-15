/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2005  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU LGPL.
    See the file COPYING.LIB.
*/

#include "fuse_lowlevel_i.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#define FUSE_MAX_WORKERS 10

struct fuse_worker {
    struct fuse_ll *f;
    pthread_t threads[FUSE_MAX_WORKERS];
    void *data;
    fuse_ll_processor_t proc;
};

static int start_thread(struct fuse_worker *w, pthread_t *thread_id);

static void *do_work(void *data)
{
    struct fuse_worker *w = (struct fuse_worker *) data;
    struct fuse_ll *f = w->f;
    int is_mainthread = (f->numworker == 1);

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (1) {
        struct fuse_cmd *cmd;

        if (fuse_ll_exited(f))
            break;

        cmd = fuse_ll_read_cmd(w->f);
        if (cmd == NULL)
            continue;

        if (f->numavail == 0 && f->numworker < FUSE_MAX_WORKERS) {
            pthread_mutex_lock(&f->worker_lock);
            if (f->numworker < FUSE_MAX_WORKERS) {
                /* FIXME: threads should be stored in a list instead
                   of an array */
                int res;
                pthread_t *thread_id = &w->threads[f->numworker];
                f->numavail ++;
                f->numworker ++;
                pthread_mutex_unlock(&f->worker_lock);
                res = start_thread(w, thread_id);
                if (res == -1) {
                    pthread_mutex_lock(&f->worker_lock);
                    f->numavail --;
                    pthread_mutex_unlock(&f->worker_lock);
                }
            } else
                pthread_mutex_unlock(&f->worker_lock);
        }

        w->proc(w->f, cmd, w->data);
    }

    /* Wait for cancellation */
    if (!is_mainthread)
        pause();

    return NULL;
}

static int start_thread(struct fuse_worker *w, pthread_t *thread_id)
{
    int res = pthread_create(thread_id, NULL, do_work, w);
    if (res != 0) {
        fprintf(stderr, "fuse: error creating thread: %s\n", strerror(res));
        return -1;
    }

    pthread_detach(*thread_id);
    return 0;
}

int fuse_ll_loop_mt_proc(struct fuse_ll *f, fuse_ll_processor_t proc, void *data)
{
    struct fuse_worker *w;
    int i;

    w = malloc(sizeof(struct fuse_worker));
    if (w == NULL) {
        fprintf(stderr, "fuse: failed to allocate worker structure\n");
        return -1;
    }
    memset(w, 0, sizeof(struct fuse_worker));
    w->f = f;
    w->data = data;
    w->proc = proc;

    f->numworker = 1;
    do_work(w);

    pthread_mutex_lock(&f->worker_lock);
    for (i = 1; i < f->numworker; i++)
        pthread_cancel(w->threads[i]);
    pthread_mutex_unlock(&f->worker_lock);
    free(w);
    f->exited = 0;
    return 0;
}

int fuse_ll_loop_mt(struct fuse_ll *f)
{
    if (f)
        return fuse_ll_loop_mt_proc(f, (fuse_ll_processor_t) fuse_ll_process_cmd, NULL);
    else
        return -1;
}

