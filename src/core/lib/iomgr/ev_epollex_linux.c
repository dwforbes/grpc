/*
 *
 * Copyright 2017, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/lib/iomgr/port.h"

/* This polling engine is only relevant on linux kernels supporting epoll() */
#ifdef GRPC_LINUX_EPOLL

#include "src/core/lib/iomgr/ev_epollex_linux.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/tls.h>
#include <grpc/support/useful.h>

#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/is_epollexclusive_available.h"
#include "src/core/lib/iomgr/lockfree_event.h"
#include "src/core/lib/iomgr/sys_epoll_wrapper.h"
#include "src/core/lib/iomgr/timer.h"
#include "src/core/lib/iomgr/wakeup_fd_posix.h"
#include "src/core/lib/iomgr/workqueue.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/support/block_annotate.h"
#include "src/core/lib/support/spinlock.h"

/*******************************************************************************
 * Pollset-set sibling link
 */

typedef enum {
  PO_POLLING_GROUP,
  PO_POLLSET_SET,
  PO_POLLSET,
  PO_FD, /* ordering is important: we always want to lock pollsets before fds:
            this guarantees that using an fd as a pollable is safe */
  PO_EMPTY_POLLABLE,
  PO_COUNT
} polling_obj_type;

typedef struct polling_obj polling_obj;
typedef struct polling_group polling_group;

struct polling_obj {
  gpr_mu mu;
  polling_obj_type type;
  polling_group *group;
  struct polling_obj *next;
  struct polling_obj *prev;
};

struct polling_group {
  polling_obj po;
  gpr_refcount refs;
};

static void po_init(polling_obj *po, polling_obj_type type);
static void po_destroy(polling_obj *po);
static void po_join(grpc_exec_ctx *exec_ctx, polling_obj *a, polling_obj *b);
static int po_cmp(polling_obj *a, polling_obj *b);

static void pg_create(grpc_exec_ctx *exec_ctx, polling_obj **initial_po,
                      size_t initial_po_count);
static polling_group *pg_ref(polling_group *pg);
static void pg_unref(polling_group *pg);
static void pg_merge(grpc_exec_ctx *exec_ctx, polling_group *a,
                     polling_group *b);
static void pg_join(grpc_exec_ctx *exec_ctx, polling_group *pg,
                    polling_obj *po);

/*******************************************************************************
 * pollable Declarations
 */

typedef struct pollable {
  polling_obj po;
  int epfd;
  grpc_wakeup_fd wakeup;
  grpc_pollset_worker *root_worker;
} pollable;

static pollable g_empty_pollable;

static void pollable_init(pollable *p, polling_obj_type type);
static void pollable_destroy(pollable *p);
/* ensure that p->epfd, p->wakeup are initialized; p->po.mu must be held */
static grpc_error *pollable_materialize(pollable *p);

/*******************************************************************************
 * Fd Declarations
 */

struct grpc_fd {
  pollable pollable;
  int fd;
  /* refst format:
       bit 0    : 1=Active / 0=Orphaned
       bits 1-n : refcount
     Ref/Unref by two to avoid altering the orphaned bit */
  gpr_atm refst;

  /* Wakeup fd used to wake pollers to check the contents of workqueue_items */
  grpc_wakeup_fd workqueue_wakeup_fd;
  grpc_closure_scheduler workqueue_scheduler;
  /* Spinlock guarding the read end of the workqueue (must be held to pop from
   * workqueue_items) */
  gpr_spinlock workqueue_read_mu;
  /* Queue of closures to be executed */
  gpr_mpscq workqueue_items;
  /* Count of items in workqueue_items */
  gpr_atm workqueue_item_count;

  /* The fd is either closed or we relinquished control of it. In either
     cases, this indicates that the 'fd' on this structure is no longer
     valid */
  gpr_mu orphaned_mu;
  bool orphaned;

  gpr_atm read_closure;
  gpr_atm write_closure;

  struct grpc_fd *freelist_next;
  grpc_closure *on_done_closure;

  /* The pollset that last noticed that the fd is readable. The actual type
   * stored in this is (grpc_pollset *) */
  gpr_atm read_notifier_pollset;

  grpc_iomgr_object iomgr_object;
};

static void fd_global_init(void);
static void fd_global_shutdown(void);

static void workqueue_enqueue(grpc_exec_ctx *exec_ctx, grpc_closure *closure,
                              grpc_error *error);

static const grpc_closure_scheduler_vtable workqueue_scheduler_vtable = {
    workqueue_enqueue, workqueue_enqueue, "workqueue"};

/*******************************************************************************
 * Pollset Declarations
 */

typedef struct pollset_worker_link {
  grpc_pollset_worker *next;
  grpc_pollset_worker *prev;
} pollset_worker_link;

typedef enum {
  PWL_POLLSET,
  PWL_POLLABLE,
  POLLSET_WORKER_LINK_COUNT
} pollset_worker_links;

struct grpc_pollset_worker {
  bool kicked;
  bool initialized_cv;
  pollset_worker_link links[POLLSET_WORKER_LINK_COUNT];
  gpr_cv cv;
  grpc_pollset *pollset;
  pollable *pollable;
};

struct grpc_pollset {
  pollable pollable;
  pollable *current_pollable;
  bool kicked_without_poller;
  grpc_closure *shutdown_closure;
  grpc_pollset_worker *root_worker;
};

/*******************************************************************************
 * Pollset-set Declarations
 */
struct grpc_pollset_set {
  polling_obj po;
};

/*******************************************************************************
 * Common helpers
 */

static bool append_error(grpc_error **composite, grpc_error *error,
                         const char *desc) {
  if (error == GRPC_ERROR_NONE) return true;
  if (*composite == GRPC_ERROR_NONE) {
    *composite = GRPC_ERROR_CREATE_FROM_COPIED_STRING(desc);
  }
  *composite = grpc_error_add_child(*composite, error);
  return false;
}

/*******************************************************************************
 * Fd Definitions
 */

/* We need to keep a freelist not because of any concerns of malloc performance
 * but instead so that implementations with multiple threads in (for example)
 * epoll_wait deal with the race between pollset removal and incoming poll
 * notifications.
 *
 * The problem is that the poller ultimately holds a reference to this
 * object, so it is very difficult to know when is safe to free it, at least
 * without some expensive synchronization.
 *
 * If we keep the object freelisted, in the worst case losing this race just
 * becomes a spurious read notification on a reused fd.
 */

/* The alarm system needs to be able to wakeup 'some poller' sometimes
 * (specifically when a new alarm needs to be triggered earlier than the next
 * alarm 'epoch'). This wakeup_fd gives us something to alert on when such a
 * case occurs. */

static grpc_fd *fd_freelist = NULL;
static gpr_mu fd_freelist_mu;

#ifdef GRPC_FD_REF_COUNT_DEBUG
#define REF_BY(fd, n, reason) ref_by(fd, n, reason, __FILE__, __LINE__)
#define UNREF_BY(ec, fd, n, reason) \
  unref_by(ec, fd, n, reason, __FILE__, __LINE__)
static void ref_by(grpc_fd *fd, int n, const char *reason, const char *file,
                   int line) {
  gpr_log(GPR_DEBUG, "FD %d %p   ref %d %ld -> %ld [%s; %s:%d]", fd->fd,
          (void *)fd, n, gpr_atm_no_barrier_load(&fd->refst),
          gpr_atm_no_barrier_load(&fd->refst) + n, reason, file, line);
#else
#define REF_BY(fd, n, reason) ref_by(fd, n)
#define UNREF_BY(ec, fd, n, reason) unref_by(ec, fd, n)
static void ref_by(grpc_fd *fd, int n) {
#endif
  GPR_ASSERT(gpr_atm_no_barrier_fetch_add(&fd->refst, n) > 0);
}

static void fd_destroy(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {
  grpc_fd *fd = arg;
  /* Add the fd to the freelist */
  grpc_iomgr_unregister_object(&fd->iomgr_object);
  pollable_destroy(&fd->pollable);
  gpr_mu_destroy(&fd->orphaned_mu);
  gpr_mu_lock(&fd_freelist_mu);
  fd->freelist_next = fd_freelist;
  fd_freelist = fd;

  grpc_lfev_destroy(&fd->read_closure);
  grpc_lfev_destroy(&fd->write_closure);

  gpr_mu_unlock(&fd_freelist_mu);
}

#ifdef GRPC_FD_REF_COUNT_DEBUG
static void unref_by(grpc_exec_ctx *exec_ctx, grpc_fd *fd, int n,
                     const char *reason, const char *file, int line) {
  gpr_atm old;
  gpr_log(GPR_DEBUG, "FD %d %p unref %d %ld -> %ld [%s; %s:%d]", fd->fd,
          (void *)fd, n, gpr_atm_no_barrier_load(&fd->refst),
          gpr_atm_no_barrier_load(&fd->refst) - n, reason, file, line);
#else
static void unref_by(grpc_exec_ctx *exec_ctx, grpc_fd *fd, int n) {
  gpr_atm old;
#endif
  old = gpr_atm_full_fetch_add(&fd->refst, -n);
  if (old == n) {
    grpc_closure_sched(exec_ctx, grpc_closure_create(fd_destroy, fd,
                                                     grpc_schedule_on_exec_ctx),
                       GRPC_ERROR_NONE);
  } else {
    GPR_ASSERT(old > n);
  }
}

static void fd_global_init(void) { gpr_mu_init(&fd_freelist_mu); }

static void fd_global_shutdown(void) {
  gpr_mu_lock(&fd_freelist_mu);
  gpr_mu_unlock(&fd_freelist_mu);
  while (fd_freelist != NULL) {
    grpc_fd *fd = fd_freelist;
    fd_freelist = fd_freelist->freelist_next;
    gpr_free(fd);
  }
  gpr_mu_destroy(&fd_freelist_mu);
}

static grpc_fd *fd_create(int fd, const char *name) {
  grpc_fd *new_fd = NULL;

  gpr_mu_lock(&fd_freelist_mu);
  if (fd_freelist != NULL) {
    new_fd = fd_freelist;
    fd_freelist = fd_freelist->freelist_next;
  }
  gpr_mu_unlock(&fd_freelist_mu);

  if (new_fd == NULL) {
    new_fd = gpr_malloc(sizeof(grpc_fd));
  }

  pollable_init(&new_fd->pollable, PO_FD);

  gpr_atm_rel_store(&new_fd->refst, (gpr_atm)1);
  new_fd->fd = fd;
  gpr_mu_init(&new_fd->orphaned_mu);
  new_fd->orphaned = false;
  grpc_lfev_init(&new_fd->read_closure);
  grpc_lfev_init(&new_fd->write_closure);
  gpr_atm_no_barrier_store(&new_fd->read_notifier_pollset, (gpr_atm)NULL);

  GRPC_LOG_IF_ERROR("fd_create",
                    grpc_wakeup_fd_init(&new_fd->workqueue_wakeup_fd));
  new_fd->workqueue_scheduler.vtable = &workqueue_scheduler_vtable;
  new_fd->workqueue_read_mu = GPR_SPINLOCK_INITIALIZER;
  gpr_mpscq_init(&new_fd->workqueue_items);
  gpr_atm_no_barrier_store(&new_fd->workqueue_item_count, 0);

  new_fd->freelist_next = NULL;
  new_fd->on_done_closure = NULL;

  char *fd_name;
  gpr_asprintf(&fd_name, "%s fd=%d", name, fd);
  grpc_iomgr_register_object(&new_fd->iomgr_object, fd_name);
#ifdef GRPC_FD_REF_COUNT_DEBUG
  gpr_log(GPR_DEBUG, "FD %d %p create %s", fd, (void *)new_fd, fd_name);
#endif
  gpr_free(fd_name);
  return new_fd;
}

static int fd_wrapped_fd(grpc_fd *fd) {
  int ret_fd = -1;
  gpr_mu_lock(&fd->orphaned_mu);
  if (!fd->orphaned) {
    ret_fd = fd->fd;
  }
  gpr_mu_unlock(&fd->orphaned_mu);

  return ret_fd;
}

static void fd_orphan(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                      grpc_closure *on_done, int *release_fd,
                      const char *reason) {
  bool is_fd_closed = false;
  grpc_error *error = GRPC_ERROR_NONE;

  gpr_mu_lock(&fd->pollable.po.mu);
  gpr_mu_lock(&fd->orphaned_mu);
  fd->on_done_closure = on_done;

  /* If release_fd is not NULL, we should be relinquishing control of the file
     descriptor fd->fd (but we still own the grpc_fd structure). */
  if (release_fd != NULL) {
    *release_fd = fd->fd;
  } else {
    close(fd->fd);
    is_fd_closed = true;
  }

  fd->orphaned = true;

  if (!is_fd_closed) {
    gpr_log(GPR_DEBUG, "TODO: handle fd removal?");
  }

  /* Remove the active status but keep referenced. We want this grpc_fd struct
     to be alive (and not added to freelist) until the end of this function */
  REF_BY(fd, 1, reason);

  grpc_closure_sched(exec_ctx, fd->on_done_closure, GRPC_ERROR_REF(error));

  gpr_mu_unlock(&fd->orphaned_mu);
  gpr_mu_unlock(&fd->pollable.po.mu);
  UNREF_BY(exec_ctx, fd, 2, reason); /* Drop the reference */
  GRPC_LOG_IF_ERROR("fd_orphan", GRPC_ERROR_REF(error));
  GRPC_ERROR_UNREF(error);
}

static grpc_pollset *fd_get_read_notifier_pollset(grpc_exec_ctx *exec_ctx,
                                                  grpc_fd *fd) {
  gpr_atm notifier = gpr_atm_acq_load(&fd->read_notifier_pollset);
  return (grpc_pollset *)notifier;
}

static bool fd_is_shutdown(grpc_fd *fd) {
  return grpc_lfev_is_shutdown(&fd->read_closure);
}

/* Might be called multiple times */
static void fd_shutdown(grpc_exec_ctx *exec_ctx, grpc_fd *fd, grpc_error *why) {
  if (grpc_lfev_set_shutdown(exec_ctx, &fd->read_closure,
                             GRPC_ERROR_REF(why))) {
    shutdown(fd->fd, SHUT_RDWR);
    grpc_lfev_set_shutdown(exec_ctx, &fd->write_closure, GRPC_ERROR_REF(why));
  }
  GRPC_ERROR_UNREF(why);
}

static void fd_notify_on_read(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                              grpc_closure *closure) {
  grpc_lfev_notify_on(exec_ctx, &fd->read_closure, closure);
}

static void fd_notify_on_write(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                               grpc_closure *closure) {
  grpc_lfev_notify_on(exec_ctx, &fd->write_closure, closure);
}

static grpc_workqueue *fd_get_workqueue(grpc_fd *fd) {
  REF_BY(fd, 2, "return_workqueue");
  return (grpc_workqueue *)fd;
}

#ifdef GRPC_WORKQUEUE_REFCOUNT_DEBUG
static grpc_workqueue *workqueue_ref(grpc_workqueue *workqueue,
                                     const char *file, int line,
                                     const char *reason) {
  if (workqueue != NULL) {
    ref_by((grpc_fd *)workqueue, 2, file, line, reason);
  }
  return workqueue;
}

static void workqueue_unref(grpc_exec_ctx *exec_ctx, grpc_workqueue *workqueue,
                            const char *file, int line, const char *reason) {
  if (workqueue != NULL) {
    unref_by(exec_ctx, (grpc_fd *)workqueue, 2, file, line, reason);
  }
}
#else
static grpc_workqueue *workqueue_ref(grpc_workqueue *workqueue) {
  if (workqueue != NULL) {
    ref_by((grpc_fd *)workqueue, 2);
  }
  return workqueue;
}

static void workqueue_unref(grpc_exec_ctx *exec_ctx,
                            grpc_workqueue *workqueue) {
  if (workqueue != NULL) {
    unref_by(exec_ctx, (grpc_fd *)workqueue, 2);
  }
}
#endif

static void workqueue_wakeup(grpc_fd *fd) {
  GRPC_LOG_IF_ERROR("workqueue_enqueue",
                    grpc_wakeup_fd_wakeup(&fd->workqueue_wakeup_fd));
}

static void workqueue_enqueue(grpc_exec_ctx *exec_ctx, grpc_closure *closure,
                              grpc_error *error) {
  GPR_TIMER_BEGIN("workqueue.enqueue", 0);
  grpc_fd *fd = (grpc_fd *)(((char *)closure->scheduler) -
                            offsetof(grpc_fd, workqueue_scheduler));
  REF_BY(fd, 2, "workqueue_enqueue");
  gpr_atm last = gpr_atm_no_barrier_fetch_add(&fd->workqueue_item_count, 1);
  closure->error_data.error = error;
  gpr_mpscq_push(&fd->workqueue_items, &closure->next_data.atm_next);
  if (last == 0) {
    workqueue_wakeup(fd);
  }
  UNREF_BY(exec_ctx, fd, 2, "workqueue_enqueue");
}

static void fd_invoke_workqueue(grpc_exec_ctx *exec_ctx, grpc_fd *fd) {
  /* handle spurious wakeups */
  if (!gpr_spinlock_trylock(&fd->workqueue_read_mu)) return;
  gpr_mpscq_node *n = gpr_mpscq_pop(&fd->workqueue_items);
  gpr_spinlock_unlock(&fd->workqueue_read_mu);
  if (n != NULL) {
    if (gpr_atm_full_fetch_add(&fd->workqueue_item_count, -1) > 1) {
      workqueue_wakeup(fd);
    }
    grpc_closure *c = (grpc_closure *)n;
    grpc_error *error = c->error_data.error;
#ifndef NDEBUG
    c->scheduled = false;
#endif
    c->cb(exec_ctx, c->cb_arg, error);
    GRPC_ERROR_UNREF(error);
  } else if (gpr_atm_no_barrier_load(&fd->workqueue_item_count) > 0) {
    /* n == NULL might mean there's work but it's not available to be popped
     * yet - try to ensure another workqueue wakes up to check shortly if so
     */
    workqueue_wakeup(fd);
  }
}

static grpc_closure_scheduler *workqueue_scheduler(grpc_workqueue *workqueue) {
  return &((grpc_fd *)workqueue)->workqueue_scheduler;
}

/*******************************************************************************
 * Pollable Definitions
 */

static void pollable_init(pollable *p, polling_obj_type type) {
  po_init(&p->po, type);
  p->root_worker = NULL;
  p->epfd = -1;
}

static void pollable_destroy(pollable *p) {
  po_destroy(&p->po);
  if (p->epfd != -1) {
    close(p->epfd);
    grpc_wakeup_fd_destroy(&p->wakeup);
  }
}

/* ensure that p->epfd, p->wakeup are initialized; p->po.mu must be held */
static grpc_error *pollable_materialize(pollable *p) {
  if (p->epfd == -1) {
    int new_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (new_epfd < 0) {
      return GRPC_OS_ERROR(errno, "epoll_create1");
    }
    grpc_error *err = grpc_wakeup_fd_init(&p->wakeup);
    if (err != GRPC_ERROR_NONE) {
      close(new_epfd);
      return err;
    }
    struct epoll_event ev = {.events = (uint32_t)(EPOLLIN | EPOLLET),
                             .data.ptr = &p->wakeup};
    if (epoll_ctl(new_epfd, EPOLL_CTL_ADD, p->wakeup.read_fd, &ev) != 0) {
      err = GRPC_OS_ERROR(errno, "epoll_ctl");
      close(new_epfd);
      grpc_wakeup_fd_destroy(&p->wakeup);
      return err;
    }

    p->epfd = new_epfd;
  }
  return GRPC_ERROR_NONE;
}

/* pollable must be materialized */
static grpc_error *pollable_add_fd(pollable *p, grpc_fd *fd) {
  grpc_error *error = GRPC_ERROR_NONE;
  static const char *err_desc = "pollable_add_fd";
  const int epfd = p->epfd;
  GPR_ASSERT(epfd != -1);

  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, "add fd %p to pollable %p", fd, p);
  }

  gpr_mu_lock(&fd->orphaned_mu);
  if (fd->orphaned) {
    gpr_mu_unlock(&fd->orphaned_mu);
    return GRPC_ERROR_NONE;
  }
  struct epoll_event ev_fd = {
      .events = (uint32_t)(EPOLLET | EPOLLIN | EPOLLOUT | EPOLLEXCLUSIVE),
      .data.ptr = fd};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd->fd, &ev_fd) != 0) {
    switch (errno) {
      case EEXIST: /* if this fd is already in the epoll set, the workqueue fd
                      must also be - just return */
        gpr_mu_unlock(&fd->orphaned_mu);
        return GRPC_ERROR_NONE;
      default:
        append_error(&error, GRPC_OS_ERROR(errno, "epoll_ctl"), err_desc);
    }
  }
  struct epoll_event ev_wq = {
      .events = (uint32_t)(EPOLLET | EPOLLIN | EPOLLEXCLUSIVE),
      .data.ptr = (void *)(1 + (intptr_t)fd)};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd->workqueue_wakeup_fd.read_fd, &ev_wq) !=
      0) {
    switch (errno) {
      case EEXIST: /* if the workqueue fd is already in the epoll set we're ok
                      - no need to do anything special */
        break;
      default:
        append_error(&error, GRPC_OS_ERROR(errno, "epoll_ctl"), err_desc);
    }
  }
  gpr_mu_unlock(&fd->orphaned_mu);

  return error;
}

/*******************************************************************************
 * Pollset Definitions
 */

GPR_TLS_DECL(g_current_thread_pollset);
GPR_TLS_DECL(g_current_thread_worker);

/* Global state management */
static grpc_error *pollset_global_init(void) {
  gpr_tls_init(&g_current_thread_pollset);
  gpr_tls_init(&g_current_thread_worker);
  pollable_init(&g_empty_pollable, PO_EMPTY_POLLABLE);
  return GRPC_ERROR_NONE;
}

static void pollset_global_shutdown(void) {
  pollable_destroy(&g_empty_pollable);
  gpr_tls_destroy(&g_current_thread_pollset);
  gpr_tls_destroy(&g_current_thread_worker);
}

static grpc_error *pollset_kick_all(grpc_pollset *pollset) {
  grpc_error *error = GRPC_ERROR_NONE;
  if (pollset->root_worker != NULL) {
    grpc_pollset_worker *worker = pollset->root_worker;
    do {
      if (worker->pollable != &pollset->pollable) {
        gpr_mu_lock(&worker->pollable->po.mu);
      }
      if (worker->initialized_cv) {
        worker->kicked = true;
        gpr_cv_signal(&worker->cv);
      } else {
        append_error(&error, grpc_wakeup_fd_wakeup(&worker->pollable->wakeup),
                     "pollset_shutdown");
      }
      if (worker->pollable != &pollset->pollable) {
        gpr_mu_unlock(&worker->pollable->po.mu);
      }

      worker = worker->links[PWL_POLLSET].next;
    } while (worker != pollset->root_worker);
  }
  return error;
}

static grpc_error *pollset_kick_inner(grpc_pollset *pollset, pollable *p,
                                      grpc_pollset_worker *specific_worker) {
  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG,
            "PS:%p kick %p tls_pollset=%p tls_worker=%p "
            "root_worker=(pollset:%p pollable:%p)",
            p, specific_worker, (void *)gpr_tls_get(&g_current_thread_pollset),
            (void *)gpr_tls_get(&g_current_thread_worker), pollset->root_worker,
            p->root_worker);
  }
  if (specific_worker == NULL) {
    if (gpr_tls_get(&g_current_thread_pollset) != (intptr_t)pollset) {
      if (pollset->root_worker == NULL) {
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_DEBUG, "PS:%p kicked_any_without_poller", p);
        }
        pollset->kicked_without_poller = true;
        return GRPC_ERROR_NONE;
      } else {
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_DEBUG, "PS:%p kicked_any_via_wakeup_fd", p);
        }
        grpc_error *err = pollable_materialize(p);
        if (err != GRPC_ERROR_NONE) return err;
        return grpc_wakeup_fd_wakeup(&p->wakeup);
      }
    } else {
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_DEBUG, "PS:%p kicked_any_but_awake", p);
      }
      return GRPC_ERROR_NONE;
    }
  } else if (specific_worker->kicked) {
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_DEBUG, "PS:%p kicked_specific_but_already_kicked", p);
    }
    return GRPC_ERROR_NONE;
  } else if (gpr_tls_get(&g_current_thread_worker) ==
             (intptr_t)specific_worker) {
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_DEBUG, "PS:%p kicked_specific_but_awake", p);
    }
    specific_worker->kicked = true;
    return GRPC_ERROR_NONE;
  } else if (specific_worker == p->root_worker) {
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_DEBUG, "PS:%p kicked_specific_via_wakeup_fd", p);
    }
    grpc_error *err = pollable_materialize(p);
    if (err != GRPC_ERROR_NONE) return err;
    specific_worker->kicked = true;
    return grpc_wakeup_fd_wakeup(&p->wakeup);
  } else {
    if (GRPC_TRACER_ON(grpc_polling_trace)) {
      gpr_log(GPR_DEBUG, "PS:%p kicked_specific_via_cv", p);
    }
    specific_worker->kicked = true;
    gpr_cv_signal(&specific_worker->cv);
    return GRPC_ERROR_NONE;
  }
}

/* p->po.mu must be held before calling this function */
static grpc_error *pollset_kick(grpc_pollset *pollset,
                                grpc_pollset_worker *specific_worker) {
  pollable *p = pollset->current_pollable;
  if (p != &pollset->pollable) {
    gpr_mu_lock(&p->po.mu);
  }
  grpc_error *error = pollset_kick_inner(pollset, p, specific_worker);
  if (p != &pollset->pollable) {
    gpr_mu_unlock(&p->po.mu);
  }
  return error;
}

static void pollset_init(grpc_pollset *pollset, gpr_mu **mu) {
  pollable_init(&pollset->pollable, PO_POLLSET);
  pollset->current_pollable = &g_empty_pollable;
  pollset->kicked_without_poller = false;
  pollset->shutdown_closure = NULL;
  pollset->root_worker = NULL;
  *mu = &pollset->pollable.po.mu;
}

/* Convert a timespec to milliseconds:
   - Very small or negative poll times are clamped to zero to do a non-blocking
     poll (which becomes spin polling)
   - Other small values are rounded up to one millisecond
   - Longer than a millisecond polls are rounded up to the next nearest
     millisecond to avoid spinning
   - Infinite timeouts are converted to -1 */
static int poll_deadline_to_millis_timeout(gpr_timespec deadline,
                                           gpr_timespec now) {
  gpr_timespec timeout;
  if (gpr_time_cmp(deadline, gpr_inf_future(deadline.clock_type)) == 0) {
    return -1;
  }

  if (gpr_time_cmp(deadline, now) <= 0) {
    return 0;
  }

  static const gpr_timespec round_up = {
      .clock_type = GPR_TIMESPAN, .tv_sec = 0, .tv_nsec = GPR_NS_PER_MS - 1};
  timeout = gpr_time_sub(deadline, now);
  int millis = gpr_time_to_millis(gpr_time_add(timeout, round_up));
  return millis >= 1 ? millis : 1;
}

static void fd_become_readable(grpc_exec_ctx *exec_ctx, grpc_fd *fd,
                               grpc_pollset *notifier) {
  grpc_lfev_set_ready(exec_ctx, &fd->read_closure);

  /* Note, it is possible that fd_become_readable might be called twice with
     different 'notifier's when an fd becomes readable and it is in two epoll
     sets (This can happen briefly during polling island merges). In such cases
     it does not really matter which notifer is set as the read_notifier_pollset
     (They would both point to the same polling island anyway) */
  /* Use release store to match with acquire load in fd_get_read_notifier */
  gpr_atm_rel_store(&fd->read_notifier_pollset, (gpr_atm)notifier);
}

static void fd_become_writable(grpc_exec_ctx *exec_ctx, grpc_fd *fd) {
  grpc_lfev_set_ready(exec_ctx, &fd->write_closure);
}

static grpc_error *fd_become_pollable_locked(grpc_fd *fd) {
  grpc_error *error = GRPC_ERROR_NONE;
  static const char *err_desc = "fd_become_pollable";
  if (append_error(&error, pollable_materialize(&fd->pollable), err_desc)) {
    append_error(&error, pollable_add_fd(&fd->pollable, fd), err_desc);
  }
  return error;
}

static void pollset_maybe_finish_shutdown(grpc_exec_ctx *exec_ctx,
                                          grpc_pollset *pollset) {
  if (pollset->shutdown_closure != NULL && pollset->root_worker == NULL) {
    grpc_closure_sched(exec_ctx, pollset->shutdown_closure, GRPC_ERROR_NONE);
    pollset->shutdown_closure = NULL;
  }
}

/* pollset->po.mu lock must be held by the caller before calling this */
static void pollset_shutdown(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                             grpc_closure *closure) {
  GPR_ASSERT(pollset->shutdown_closure == NULL);
  pollset->shutdown_closure = closure;
  GRPC_LOG_IF_ERROR("pollset_shutdown", pollset_kick_all(pollset));
  pollset_maybe_finish_shutdown(exec_ctx, pollset);
}

static bool pollset_is_pollable_fd(grpc_pollset *pollset, pollable *p) {
  return p != &g_empty_pollable && p != &pollset->pollable;
}

/* pollset_shutdown is guaranteed to be called before pollset_destroy. */
static void pollset_destroy(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset) {
  pollable_destroy(&pollset->pollable);
  if (pollset_is_pollable_fd(pollset, pollset->current_pollable)) {
    UNREF_BY(exec_ctx, (grpc_fd *)pollset->current_pollable, 2,
             "pollset_pollable");
  }
}

#define MAX_EPOLL_EVENTS 100

static grpc_error *pollset_epoll(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                                 pollable *p, gpr_timespec now,
                                 gpr_timespec deadline) {
  struct epoll_event events[MAX_EPOLL_EVENTS];
  static const char *err_desc = "pollset_poll";

  int timeout = poll_deadline_to_millis_timeout(deadline, now);

  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, "PS:%p poll %p for %dms", pollset, p, timeout);
  }

  if (timeout != 0) {
    GRPC_SCHEDULING_START_BLOCKING_REGION;
  }
  int r;
  do {
    r = epoll_wait(p->epfd, events, MAX_EPOLL_EVENTS, timeout);
  } while (r < 0 && errno == EINTR);
  if (timeout != 0) {
    GRPC_SCHEDULING_END_BLOCKING_REGION;
  }

  if (r < 0) return GRPC_OS_ERROR(errno, "epoll_wait");

  if (GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, "PS:%p poll %p got %d events", pollset, p, r);
  }

  grpc_error *error = GRPC_ERROR_NONE;
  for (int i = 0; i < r; i++) {
    void *data_ptr = events[i].data.ptr;
    if (data_ptr == &p->wakeup) {
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_DEBUG, "PS:%p poll %p got pollset_wakeup", pollset, p);
      }
      append_error(&error, grpc_wakeup_fd_consume_wakeup(&p->wakeup), err_desc);
    } else {
      grpc_fd *fd = (grpc_fd *)(((intptr_t)data_ptr) & ~(intptr_t)1);
      bool is_workqueue = (((intptr_t)data_ptr) & 1) != 0;
      bool cancel = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
      bool read_ev = (events[i].events & (EPOLLIN | EPOLLPRI)) != 0;
      bool write_ev = (events[i].events & EPOLLOUT) != 0;
      if (GRPC_TRACER_ON(grpc_polling_trace)) {
        gpr_log(GPR_DEBUG,
                "PS:%p poll %p got fd %p: is_wq=%d cancel=%d read=%d "
                "write=%d",
                pollset, p, fd, is_workqueue, cancel, read_ev, write_ev);
      }
      if (is_workqueue) {
        append_error(&error,
                     grpc_wakeup_fd_consume_wakeup(&fd->workqueue_wakeup_fd),
                     err_desc);
        fd_invoke_workqueue(exec_ctx, fd);
      } else {
        if (read_ev || cancel) {
          fd_become_readable(exec_ctx, fd, pollset);
        }
        if (write_ev || cancel) {
          fd_become_writable(exec_ctx, fd);
        }
      }
    }
  }

  return error;
}

/* Return true if first in list */
static bool worker_insert(grpc_pollset_worker **root, pollset_worker_links link,
                          grpc_pollset_worker *worker) {
  if (*root == NULL) {
    *root = worker;
    worker->links[link].next = worker->links[link].prev = worker;
    return true;
  } else {
    worker->links[link].next = *root;
    worker->links[link].prev = worker->links[link].next->links[link].prev;
    worker->links[link].next->links[link].prev = worker;
    worker->links[link].prev->links[link].next = worker;
    return false;
  }
}

/* Return true if last in list */
typedef enum { EMPTIED, NEW_ROOT, REMOVED } worker_remove_result;

static worker_remove_result worker_remove(grpc_pollset_worker **root,
                                          pollset_worker_links link,
                                          grpc_pollset_worker *worker) {
  if (worker == *root) {
    if (worker == worker->links[link].next) {
      *root = NULL;
      return EMPTIED;
    } else {
      *root = worker->links[link].next;
      worker->links[link].prev->links[link].next = worker->links[link].next;
      worker->links[link].next->links[link].prev = worker->links[link].prev;
      return NEW_ROOT;
    }
  } else {
    worker->links[link].prev->links[link].next = worker->links[link].next;
    worker->links[link].next->links[link].prev = worker->links[link].prev;
    return REMOVED;
  }
}

/* Return true if this thread should poll */
static bool begin_worker(grpc_pollset *pollset, grpc_pollset_worker *worker,
                         grpc_pollset_worker **worker_hdl, gpr_timespec *now,
                         gpr_timespec deadline) {
  bool do_poll = true;
  if (worker_hdl != NULL) *worker_hdl = worker;
  worker->initialized_cv = false;
  worker->kicked = false;
  worker->pollset = pollset;
  worker->pollable = pollset->current_pollable;

  if (pollset_is_pollable_fd(pollset, worker->pollable)) {
    REF_BY((grpc_fd *)worker->pollable, 2, "one_poll");
  }

  worker_insert(&pollset->root_worker, PWL_POLLSET, worker);
  if (!worker_insert(&worker->pollable->root_worker, PWL_POLLABLE, worker)) {
    worker->initialized_cv = true;
    gpr_cv_init(&worker->cv);
    if (worker->pollable != &pollset->pollable) {
      gpr_mu_unlock(&pollset->pollable.po.mu);
    }
    if (GRPC_TRACER_ON(grpc_polling_trace) &&
        worker->pollable->root_worker != worker) {
      gpr_log(GPR_DEBUG, "PS:%p wait %p w=%p for %dms", pollset,
              worker->pollable, worker,
              poll_deadline_to_millis_timeout(deadline, *now));
    }
    while (do_poll && worker->pollable->root_worker != worker) {
      if (gpr_cv_wait(&worker->cv, &worker->pollable->po.mu, deadline)) {
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_DEBUG, "PS:%p timeout_wait %p w=%p", pollset,
                  worker->pollable, worker);
        }
        do_poll = false;
      } else if (worker->kicked) {
        if (GRPC_TRACER_ON(grpc_polling_trace)) {
          gpr_log(GPR_DEBUG, "PS:%p wakeup %p w=%p", pollset, worker->pollable,
                  worker);
        }
        do_poll = false;
      } else if (GRPC_TRACER_ON(grpc_polling_trace) &&
                 worker->pollable->root_worker != worker) {
        gpr_log(GPR_DEBUG, "PS:%p spurious_wakeup %p w=%p", pollset,
                worker->pollable, worker);
      }
    }
    if (worker->pollable != &pollset->pollable) {
      gpr_mu_unlock(&worker->pollable->po.mu);
      gpr_mu_lock(&pollset->pollable.po.mu);
      gpr_mu_lock(&worker->pollable->po.mu);
    }
    *now = gpr_now(now->clock_type);
  }

  return do_poll && pollset->shutdown_closure == NULL &&
         pollset->current_pollable == worker->pollable;
}

static void end_worker(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                       grpc_pollset_worker *worker,
                       grpc_pollset_worker **worker_hdl) {
  if (NEW_ROOT ==
      worker_remove(&worker->pollable->root_worker, PWL_POLLABLE, worker)) {
    gpr_cv_signal(&worker->pollable->root_worker->cv);
  }
  if (worker->initialized_cv) {
    gpr_cv_destroy(&worker->cv);
  }
  if (pollset_is_pollable_fd(pollset, worker->pollable)) {
    UNREF_BY(exec_ctx, (grpc_fd *)worker->pollable, 2, "one_poll");
  }
  if (EMPTIED == worker_remove(&pollset->root_worker, PWL_POLLSET, worker)) {
    pollset_maybe_finish_shutdown(exec_ctx, pollset);
  }
}

/* pollset->po.mu lock must be held by the caller before calling this.
   The function pollset_work() may temporarily release the lock (pollset->po.mu)
   during the course of its execution but it will always re-acquire the lock and
   ensure that it is held by the time the function returns */
static grpc_error *pollset_work(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                                grpc_pollset_worker **worker_hdl,
                                gpr_timespec now, gpr_timespec deadline) {
  grpc_pollset_worker worker;
  if (0 && GRPC_TRACER_ON(grpc_polling_trace)) {
    gpr_log(GPR_DEBUG, "PS:%p work hdl=%p worker=%p now=%" PRId64
                       ".%09d deadline=%" PRId64 ".%09d kwp=%d root_worker=%p",
            pollset, worker_hdl, &worker, now.tv_sec, now.tv_nsec,
            deadline.tv_sec, deadline.tv_nsec, pollset->kicked_without_poller,
            pollset->root_worker);
  }
  grpc_error *error = GRPC_ERROR_NONE;
  static const char *err_desc = "pollset_work";
  if (pollset->kicked_without_poller) {
    pollset->kicked_without_poller = false;
    return GRPC_ERROR_NONE;
  }
  if (pollset->current_pollable != &pollset->pollable) {
    gpr_mu_lock(&pollset->current_pollable->po.mu);
  }
  if (begin_worker(pollset, &worker, worker_hdl, &now, deadline)) {
    gpr_tls_set(&g_current_thread_pollset, (intptr_t)pollset);
    gpr_tls_set(&g_current_thread_worker, (intptr_t)&worker);
    GPR_ASSERT(!pollset->shutdown_closure);
    append_error(&error, pollable_materialize(worker.pollable), err_desc);
    if (worker.pollable != &pollset->pollable) {
      gpr_mu_unlock(&worker.pollable->po.mu);
    }
    gpr_mu_unlock(&pollset->pollable.po.mu);
    append_error(&error, pollset_epoll(exec_ctx, pollset, worker.pollable, now,
                                       deadline),
                 err_desc);
    grpc_exec_ctx_flush(exec_ctx);
    gpr_mu_lock(&pollset->pollable.po.mu);
    if (worker.pollable != &pollset->pollable) {
      gpr_mu_lock(&worker.pollable->po.mu);
    }
    gpr_tls_set(&g_current_thread_pollset, 0);
    gpr_tls_set(&g_current_thread_worker, 0);
    pollset_maybe_finish_shutdown(exec_ctx, pollset);
  }
  end_worker(exec_ctx, pollset, &worker, worker_hdl);
  if (worker.pollable != &pollset->pollable) {
    gpr_mu_unlock(&worker.pollable->po.mu);
  }
  return error;
}

static void unref_fd_no_longer_poller(grpc_exec_ctx *exec_ctx, void *arg,
                                      grpc_error *error) {
  grpc_fd *fd = arg;
  UNREF_BY(exec_ctx, fd, 2, "pollset_pollable");
}

/* expects pollsets locked, flag whether fd is locked or not */
static grpc_error *pollset_add_fd_locked(grpc_exec_ctx *exec_ctx,
                                         grpc_pollset *pollset, grpc_fd *fd,
                                         bool fd_locked) {
  static const char *err_desc = "pollset_add_fd";
  grpc_error *error = GRPC_ERROR_NONE;
  if (pollset->current_pollable == &g_empty_pollable) {
    if (GRPC_TRACER_ON(grpc_polling_trace))
      gpr_log(GPR_DEBUG,
              "PS:%p add fd %p; transition pollable from empty to fd", pollset,
              fd);
    /* empty pollable --> single fd pollable */
    append_error(&error, pollset_kick_all(pollset), err_desc);
    pollset->current_pollable = &fd->pollable;
    if (!fd_locked) gpr_mu_lock(&fd->pollable.po.mu);
    append_error(&error, fd_become_pollable_locked(fd), err_desc);
    if (!fd_locked) gpr_mu_unlock(&fd->pollable.po.mu);
    REF_BY(fd, 2, "pollset_pollable");
  } else if (pollset->current_pollable == &pollset->pollable) {
    if (GRPC_TRACER_ON(grpc_polling_trace))
      gpr_log(GPR_DEBUG, "PS:%p add fd %p; already multipolling", pollset, fd);
    append_error(&error, pollable_add_fd(pollset->current_pollable, fd),
                 err_desc);
  } else if (pollset->current_pollable != &fd->pollable) {
    grpc_fd *had_fd = (grpc_fd *)pollset->current_pollable;
    if (GRPC_TRACER_ON(grpc_polling_trace))
      gpr_log(GPR_DEBUG,
              "PS:%p add fd %p; transition pollable from fd %p to multipoller",
              pollset, fd, had_fd);
    append_error(&error, pollset_kick_all(pollset), err_desc);
    pollset->current_pollable = &pollset->pollable;
    if (append_error(&error, pollable_materialize(&pollset->pollable),
                     err_desc)) {
      pollable_add_fd(&pollset->pollable, had_fd);
      pollable_add_fd(&pollset->pollable, fd);
    }
    grpc_closure_sched(exec_ctx,
                       grpc_closure_create(unref_fd_no_longer_poller, had_fd,
                                           grpc_schedule_on_exec_ctx),
                       GRPC_ERROR_NONE);
  }
  return error;
}

static void pollset_add_fd(grpc_exec_ctx *exec_ctx, grpc_pollset *pollset,
                           grpc_fd *fd) {
  gpr_mu_lock(&pollset->pollable.po.mu);
  grpc_error *error = pollset_add_fd_locked(exec_ctx, pollset, fd, false);
  gpr_mu_unlock(&pollset->pollable.po.mu);
  GRPC_LOG_IF_ERROR("pollset_add_fd", error);
}

/*******************************************************************************
 * Pollset-set Definitions
 */

static grpc_pollset_set *pollset_set_create(void) {
  grpc_pollset_set *pss = gpr_zalloc(sizeof(*pss));
  po_init(&pss->po, PO_POLLSET_SET);
  return pss;
}

static void pollset_set_destroy(grpc_exec_ctx *exec_ctx,
                                grpc_pollset_set *pss) {
  po_destroy(&pss->po);
  gpr_free(pss);
}

static void pollset_set_add_fd(grpc_exec_ctx *exec_ctx, grpc_pollset_set *pss,
                               grpc_fd *fd) {
  po_join(exec_ctx, &pss->po, &fd->pollable.po);
}

static void pollset_set_del_fd(grpc_exec_ctx *exec_ctx, grpc_pollset_set *pss,
                               grpc_fd *fd) {}

static void pollset_set_add_pollset(grpc_exec_ctx *exec_ctx,
                                    grpc_pollset_set *pss, grpc_pollset *ps) {
  po_join(exec_ctx, &pss->po, &ps->pollable.po);
}

static void pollset_set_del_pollset(grpc_exec_ctx *exec_ctx,
                                    grpc_pollset_set *pss, grpc_pollset *ps) {}

static void pollset_set_add_pollset_set(grpc_exec_ctx *exec_ctx,
                                        grpc_pollset_set *bag,
                                        grpc_pollset_set *item) {
  po_join(exec_ctx, &bag->po, &item->po);
}

static void pollset_set_del_pollset_set(grpc_exec_ctx *exec_ctx,
                                        grpc_pollset_set *bag,
                                        grpc_pollset_set *item) {}

static void po_init(polling_obj *po, polling_obj_type type) {
  gpr_mu_init(&po->mu);
  po->type = type;
  po->group = NULL;
  po->next = po;
  po->prev = po;
}

static polling_group *pg_lock_latest(polling_group *pg) {
  /* assumes pg unlocked; consumes ref, returns ref */
  gpr_mu_lock(&pg->po.mu);
  while (pg->po.group != NULL) {
    polling_group *new_pg = pg_ref(pg->po.group);
    gpr_mu_unlock(&pg->po.mu);
    pg_unref(pg);
    pg = new_pg;
    gpr_mu_lock(&pg->po.mu);
  }
  return pg;
}

static void po_destroy(polling_obj *po) {
  if (po->group != NULL) {
    polling_group *pg = pg_lock_latest(po->group);
    po->prev->next = po->next;
    po->next->prev = po->prev;
    gpr_mu_unlock(&pg->po.mu);
    pg_unref(pg);
  }
  gpr_mu_destroy(&po->mu);
}

static polling_group *pg_ref(polling_group *pg) {
  gpr_ref(&pg->refs);
  return pg;
}

static void pg_unref(polling_group *pg) {
  if (gpr_unref(&pg->refs)) {
    po_destroy(&pg->po);
    gpr_free(pg);
  }
}

static int po_cmp(polling_obj *a, polling_obj *b) {
  if (a == b) return 0;
  if (a->type < b->type) return -1;
  if (a->type > b->type) return 1;
  if (a < b) return -1;
  assert(a > b);
  return 1;
}

static void po_join(grpc_exec_ctx *exec_ctx, polling_obj *a, polling_obj *b) {
  switch (po_cmp(a, b)) {
    case 0:
      return;
    case 1:
      GPR_SWAP(polling_obj *, a, b);
    /* fall through */
    case -1:
      gpr_mu_lock(&a->mu);
      gpr_mu_lock(&b->mu);

      if (a->group == NULL) {
        if (b->group == NULL) {
          polling_obj *initial_po[] = {a, b};
          pg_create(exec_ctx, initial_po, GPR_ARRAY_SIZE(initial_po));
          gpr_mu_unlock(&a->mu);
          gpr_mu_unlock(&b->mu);
        } else {
          polling_group *b_group = pg_ref(b->group);
          gpr_mu_unlock(&b->mu);
          gpr_mu_unlock(&a->mu);
          pg_join(exec_ctx, b_group, a);
        }
      } else if (b->group == NULL) {
        polling_group *a_group = pg_ref(a->group);
        gpr_mu_unlock(&a->mu);
        gpr_mu_unlock(&b->mu);
        pg_join(exec_ctx, a_group, b);
      } else if (a->group == b->group) {
        /* nothing to do */
        gpr_mu_unlock(&a->mu);
        gpr_mu_unlock(&b->mu);
      } else {
        polling_group *a_group = pg_ref(a->group);
        polling_group *b_group = pg_ref(b->group);
        gpr_mu_unlock(&a->mu);
        gpr_mu_unlock(&b->mu);
        pg_merge(exec_ctx, a_group, b_group);
      }
  }
}

static void pg_notify(grpc_exec_ctx *exec_ctx, polling_obj *a, polling_obj *b) {
  if (a->type == PO_FD && b->type == PO_POLLSET) {
    pollset_add_fd_locked(exec_ctx, (grpc_pollset *)b, (grpc_fd *)a, true);
  } else if (a->type == PO_POLLSET && b->type == PO_FD) {
    pollset_add_fd_locked(exec_ctx, (grpc_pollset *)a, (grpc_fd *)b, true);
  }
}

static void pg_broadcast(grpc_exec_ctx *exec_ctx, polling_group *from,
                         polling_group *to) {
  for (polling_obj *a = from->po.next; a != &from->po; a = a->next) {
    for (polling_obj *b = to->po.next; b != &to->po; b = b->next) {
      if (po_cmp(a, b) < 0) {
        gpr_mu_lock(&a->mu);
        gpr_mu_lock(&b->mu);
      } else {
        GPR_ASSERT(po_cmp(a, b) != 0);
        gpr_mu_lock(&b->mu);
        gpr_mu_lock(&a->mu);
      }
      pg_notify(exec_ctx, a, b);
      gpr_mu_unlock(&a->mu);
      gpr_mu_unlock(&b->mu);
    }
  }
}

static void pg_create(grpc_exec_ctx *exec_ctx, polling_obj **initial_po,
                      size_t initial_po_count) {
  /* assumes all polling objects in initial_po are locked */
  polling_group *pg = gpr_malloc(sizeof(*pg));
  po_init(&pg->po, PO_POLLING_GROUP);
  gpr_ref_init(&pg->refs, (int)initial_po_count);
  for (size_t i = 0; i < initial_po_count; i++) {
    GPR_ASSERT(initial_po[i]->group == NULL);
    initial_po[i]->group = pg;
  }
  for (size_t i = 1; i < initial_po_count; i++) {
    initial_po[i]->prev = initial_po[i - 1];
  }
  for (size_t i = 0; i < initial_po_count - 1; i++) {
    initial_po[i]->next = initial_po[i + 1];
  }
  initial_po[0]->prev = &pg->po;
  initial_po[initial_po_count - 1]->next = &pg->po;
  pg->po.next = initial_po[0];
  pg->po.prev = initial_po[initial_po_count - 1];
  for (size_t i = 1; i < initial_po_count; i++) {
    for (size_t j = 0; j < i; j++) {
      pg_notify(exec_ctx, initial_po[i], initial_po[j]);
    }
  }
}

static void pg_join(grpc_exec_ctx *exec_ctx, polling_group *pg,
                    polling_obj *po) {
  /* assumes neither pg nor po are locked; consumes one ref to pg */
  pg = pg_lock_latest(pg);
  /* pg locked */
  for (polling_obj *existing = pg->po.next /* skip pg - it's just a stub */;
       existing != &pg->po; existing = existing->next) {
    if (po_cmp(po, existing) < 0) {
      gpr_mu_lock(&po->mu);
      gpr_mu_lock(&existing->mu);
    } else {
      GPR_ASSERT(po_cmp(po, existing) != 0);
      gpr_mu_lock(&existing->mu);
      gpr_mu_lock(&po->mu);
    }
    /* pg, po, existing locked */
    if (po->group != NULL) {
      gpr_mu_unlock(&pg->po.mu);
      polling_group *po_group = pg_ref(po->group);
      gpr_mu_unlock(&po->mu);
      gpr_mu_unlock(&existing->mu);
      pg_merge(exec_ctx, pg, po_group);
      /* early exit: polling obj picked up a group during joining: we needed
         to do a full merge */
      return;
    }
    pg_notify(exec_ctx, po, existing);
    gpr_mu_unlock(&po->mu);
    gpr_mu_unlock(&existing->mu);
  }
  gpr_mu_lock(&po->mu);
  if (po->group != NULL) {
    gpr_mu_unlock(&pg->po.mu);
    polling_group *po_group = pg_ref(po->group);
    gpr_mu_unlock(&po->mu);
    pg_merge(exec_ctx, pg, po_group);
    /* early exit: polling obj picked up a group during joining: we needed
       to do a full merge */
    return;
  }
  po->group = pg;
  po->next = &pg->po;
  po->prev = pg->po.prev;
  po->prev->next = po->next->prev = po;
  gpr_mu_unlock(&pg->po.mu);
  gpr_mu_unlock(&po->mu);
}

static void pg_merge(grpc_exec_ctx *exec_ctx, polling_group *a,
                     polling_group *b) {
  for (;;) {
    if (a == b) {
      pg_unref(a);
      pg_unref(b);
      return;
    }
    if (a > b) GPR_SWAP(polling_group *, a, b);
    gpr_mu_lock(&a->po.mu);
    gpr_mu_lock(&b->po.mu);
    if (a->po.group != NULL) {
      polling_group *m2 = pg_ref(a->po.group);
      gpr_mu_unlock(&a->po.mu);
      gpr_mu_unlock(&b->po.mu);
      pg_unref(a);
      a = m2;
    } else if (b->po.group != NULL) {
      polling_group *m2 = pg_ref(b->po.group);
      gpr_mu_unlock(&a->po.mu);
      gpr_mu_unlock(&b->po.mu);
      pg_unref(b);
      b = m2;
    } else {
      break;
    }
  }
  polling_group **unref = NULL;
  size_t unref_count = 0;
  size_t unref_cap = 0;
  b->po.group = a;
  pg_broadcast(exec_ctx, a, b);
  pg_broadcast(exec_ctx, b, a);
  while (b->po.next != &b->po) {
    polling_obj *po = b->po.next;
    gpr_mu_lock(&po->mu);
    if (unref_count == unref_cap) {
      unref_cap = GPR_MAX(8, 3 * unref_cap / 2);
      unref = gpr_realloc(unref, unref_cap * sizeof(*unref));
    }
    unref[unref_count++] = po->group;
    po->group = pg_ref(a);
    // unlink from b
    po->prev->next = po->next;
    po->next->prev = po->prev;
    // link to a
    po->next = &a->po;
    po->prev = a->po.prev;
    po->next->prev = po->prev->next = po;
    gpr_mu_unlock(&po->mu);
  }
  gpr_mu_unlock(&a->po.mu);
  gpr_mu_unlock(&b->po.mu);
  for (size_t i = 0; i < unref_count; i++) {
    pg_unref(unref[i]);
  }
  gpr_free(unref);
  pg_unref(b);
}

/*******************************************************************************
 * Event engine binding
 */

static void shutdown_engine(void) {
  fd_global_shutdown();
  pollset_global_shutdown();
}

static const grpc_event_engine_vtable vtable = {
    .pollset_size = sizeof(grpc_pollset),

    .fd_create = fd_create,
    .fd_wrapped_fd = fd_wrapped_fd,
    .fd_orphan = fd_orphan,
    .fd_shutdown = fd_shutdown,
    .fd_is_shutdown = fd_is_shutdown,
    .fd_notify_on_read = fd_notify_on_read,
    .fd_notify_on_write = fd_notify_on_write,
    .fd_get_read_notifier_pollset = fd_get_read_notifier_pollset,
    .fd_get_workqueue = fd_get_workqueue,

    .pollset_init = pollset_init,
    .pollset_shutdown = pollset_shutdown,
    .pollset_destroy = pollset_destroy,
    .pollset_work = pollset_work,
    .pollset_kick = pollset_kick,
    .pollset_add_fd = pollset_add_fd,

    .pollset_set_create = pollset_set_create,
    .pollset_set_destroy = pollset_set_destroy,
    .pollset_set_add_pollset = pollset_set_add_pollset,
    .pollset_set_del_pollset = pollset_set_del_pollset,
    .pollset_set_add_pollset_set = pollset_set_add_pollset_set,
    .pollset_set_del_pollset_set = pollset_set_del_pollset_set,
    .pollset_set_add_fd = pollset_set_add_fd,
    .pollset_set_del_fd = pollset_set_del_fd,

    .workqueue_ref = workqueue_ref,
    .workqueue_unref = workqueue_unref,
    .workqueue_scheduler = workqueue_scheduler,

    .shutdown_engine = shutdown_engine,
};

const grpc_event_engine_vtable *grpc_init_epollex_linux(
    bool explicitly_requested) {
  if (!explicitly_requested) return NULL;

  if (!grpc_has_wakeup_fd()) {
    return NULL;
  }

  if (!grpc_is_epollexclusive_available()) {
    return NULL;
  }

  fd_global_init();

  if (!GRPC_LOG_IF_ERROR("pollset_global_init", pollset_global_init())) {
    pollset_global_shutdown();
    fd_global_shutdown();
    return NULL;
  }

  return &vtable;
}

#else /* defined(GRPC_LINUX_EPOLL) */
#if defined(GRPC_POSIX_SOCKET)
#include "src/core/lib/iomgr/ev_posix.h"
/* If GRPC_LINUX_EPOLL is not defined, it means epoll is not available. Return
 * NULL */
const grpc_event_engine_vtable *grpc_init_epollex_linux(
    bool explicitly_requested) {
  return NULL;
}
#endif /* defined(GRPC_POSIX_SOCKET) */

#endif /* !defined(GRPC_LINUX_EPOLL) */
