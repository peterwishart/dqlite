#include "uv_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../lib/assert.h"
#include "../raft.h"
#include "heap.h"

/* The raw-write path is factored behind a pluggable backend interface (the
 * raft_io_fs seam). Everything else in this file -- request status/completion
 * bookkeeping and the close/drain state machine -- is backend-agnostic and
 * shared. Each backend only supplies the platform-specific way to set up
 * resources, submit a single asynchronous write, and tear those resources down.
 *
 * Two backends are provided:
 *
 *   - "aio"        : Linux kernel AIO (io_submit/eventfd/uv_poll), the historical
 *                    fast path. Compiled only when DQLITE_HAVE_KAIO is defined.
 *   - "threadpool" : portable libuv threadpool + uv_fs_write. No Linux-only
 *                    primitives; the basis for the macOS/Windows port.
 *
 * A future Windows IOCP backend or a Linux RWF_NOWAIT fast-path plugs in purely
 * by adding another struct UvWriterBackend instance and selecting it in
 * UvWriterInit -- callers, the UvWriter/UvWriterReq types, and the shared
 * segment/prepare/finalize/truncate logic above this file stay unchanged. */
struct UvWriterBackend
{
	const char *name;
	/* Set up backend-specific resources on the (already zero-initialized)
	 * writer. On failure, fill errmsg and return a RAFT_* error. */
	int (*init)(struct UvWriter *w, char *errmsg);
	/* Submit a single asynchronous write. The common request fields
	 * (writer, len, status, cb, errmsg) have already been initialized by
	 * UvWriterSubmit; the backend fills in its own per-request state and
	 * schedules the write. Completion must eventually reach the shared
	 * uvWriterReqFinish(). */
	int (*submit)(struct UvWriter *w,
		      struct UvWriterReq *req,
		      const uv_buf_t bufs[],
		      unsigned n,
		      size_t offset);
	/* Begin backend-specific teardown. Asynchronous: the writer's close_cb
	 * fires later, once all handles are closed and in-flight requests have
	 * drained. */
	void (*close)(struct UvWriter *w);
	/* Free backend-specific resources. Called once from the shared cleanup
	 * path after all handles are closed. May be NULL if there is nothing to
	 * free. */
	void (*destroy)(struct UvWriter *w);
};

#if defined(DQLITE_HAVE_KAIO)
/* Whether the portable libuv-threadpool write backend has been forced via the
 * DQLITE_IO_BACKEND=threadpool environment variable. This backend avoids the
 * Linux-only kernel AIO / eventfd machinery and is the basis for the
 * macOS/Windows port (see PORT_DESIGN.md). It is the ONLY way to select the
 * portable backend on a kernel-AIO build: without it the AIO backend is used
 * even when fully async I/O is unavailable, exactly like upstream (see the
 * backend selection in UvWriterInit). On builds without kernel AIO the
 * portable backend is unconditional and this switch is meaningless. */
static bool uvWriterThreadpoolForced(void)
{
	const char *env = getenv("DQLITE_IO_BACKEND");
	bool forced = env != NULL && strcmp(env, "threadpool") == 0;
	if (forced) {
		/* This switch silently changes the write mechanism of a
		 * production binary, so honouring it must never be silent:
		 * warn once, unconditionally on stderr. The tracing paths
		 * (dqlite's LIBDQLITE_TRACE, raft's per-instance tracer) are
		 * opt-in and disabled in normal operation, so they cannot be
		 * relied on to reach an operator. A benign race on the flag
		 * can at worst print the warning twice. */
		static bool warned = false;
		if (!warned) {
			warned = true;
			fprintf(stderr,
				"dqlite: WARNING: DQLITE_IO_BACKEND=threadpool "
				"is set: using the portable libuv-threadpool "
				"write backend instead of Linux kernel AIO. "
				"This is a testing switch and may reduce I/O "
				"performance.\n");
		}
	}
	return forced;
}
#endif /* DQLITE_HAVE_KAIO */

/******************************************************************************
 *
 * Shared request bookkeeping (backend-agnostic).
 *
 *****************************************************************************/

/* Copy the error message from the request object to the writer object. */
static void uvWriterReqTransferErrMsg(struct UvWriterReq *req)
{
	ErrMsgPrintf(req->writer->errmsg, "%s", req->errmsg);
}

/* Set the request status according the given result code. */
static void uvWriterReqSetStatus(struct UvWriterReq *req, int result)
{
	if (result < 0) {
		ErrMsgPrintf(req->errmsg, "write failed: %d", result);
		req->status = RAFT_IOERR;
	} else if ((size_t)result < req->len) {
		ErrMsgPrintf(req->errmsg,
			     "short write: %d bytes instead of %zu", result,
			     req->len);
		req->status = RAFT_NOSPACE;
	} else {
		req->status = 0;
	}
}

/* Remove the request from the queue of inflight writes and invoke the request
 * callback if set. */
static void uvWriterReqFinish(struct UvWriterReq *req)
{
	queue_remove(&req->queue);
	if (req->status != 0) {
		uvWriterReqTransferErrMsg(req);
	}
	req->cb(req, req->status);
}

/* Callback run after a threadpool worker (either backend) has returned. It
 * normally invokes the write request callback. */
static void uvWriterAfterWorkCb(uv_work_t *work, int status)
{
	struct UvWriterReq *req = work->data; /* Write file request object */
	dqlite_assert(status == 0); /* We don't cancel worker requests */
	uvWriterReqFinish(req);
}

/* Return the total lengths of the given buffers. */
static size_t lenOfBufs(const uv_buf_t bufs[], unsigned n)
{
	size_t len = 0;
	unsigned i;
	for (i = 0; i < n; i++) {
		len += bufs[i].len;
	}
	return len;
}

/******************************************************************************
 *
 * Shared close / drain state machine (backend-agnostic).
 *
 *****************************************************************************/

static void uvWriterCleanUpAndFireCloseCb(struct UvWriter *w)
{
	dqlite_assert(w->closing);

	UvOsClose(w->fd);
	if (w->backend->destroy != NULL) {
		w->backend->destroy(w);
	}

	if (w->close_cb != NULL) {
		w->close_cb(w);
	}
}

static void uvWriterCheckCloseCb(struct uv_handle_s *handle)
{
	struct UvWriter *w = handle->data;
	w->check.data = NULL;
	/* The AIO backend closes an additional poller handle; wait for it too.
	 * For the threadpool backend event_poller.data is always NULL. */
	if (w->event_poller.data != NULL) {
		return;
	}
	uvWriterCleanUpAndFireCloseCb(w);
}

static void uvWriterCheckCb(struct uv_check_s *check)
{
	struct UvWriter *w = check->data;
	if (!queue_empty(&w->work_queue)) {
		return;
	}
	uv_close((struct uv_handle_s *)&w->check, uvWriterCheckCloseCb);
}

/* Wait for any in-flight threadpool writes to drain, then close the check
 * handle. Shared tail of every backend's close path. */
static void uvWriterDrainThenClose(struct UvWriter *w)
{
	if (!queue_empty(&w->work_queue)) {
		uv_check_start(&w->check, uvWriterCheckCb);
	} else {
		uv_close((struct uv_handle_s *)&w->check, uvWriterCheckCloseCb);
	}
}

/******************************************************************************
 *
 * Backend: kernel AIO (Linux only).
 *
 *****************************************************************************/

#if defined(DQLITE_HAVE_KAIO)
/* Wrapper around the low-level OS syscall, providing a better error message. */
static int uvWriterIoSetup(unsigned n, aio_context_t *ctx, char *errmsg)
{
	int rv;
	rv = UvOsIoSetup(n, ctx);
	if (rv != 0) {
		switch (rv) {
			case UV_EAGAIN:
				ErrMsgPrintf(errmsg,
					     "AIO events user limit exceeded");
				rv = RAFT_TOOMANY;
				break;
			default:
				UvOsErrMsg(errmsg, "io_setup", rv);
				rv = RAFT_IOERR;
				break;
		}
		return rv;
	}
	return 0;
}

/* Run blocking syscalls involved in a file write request.
 *
 * Perform a KAIO write request and synchronously wait for it to complete. */
static void uvWriterWorkCb(uv_work_t *work)
{
	struct UvWriterReq *req; /* Writer request object */
	struct UvWriter *w;      /* Writer object */
	aio_context_t ctx;       /* KAIO handle */
	struct iocb *iocbs;      /* Pointer to KAIO request object */
	struct io_event event;   /* KAIO response object */
	int n_events;
	int rv;

	req = work->data;
	w = req->writer;

	iocbs = &req->iocb;

	/* If more than one write in parallel is allowed, submit the AIO request
	 * using a dedicated context, to avoid synchronization issues between
	 * threads when multiple writes are submitted in parallel. This is
	 * suboptimal but in real-world users should use file systems and
	 * kernels with proper async write support. */
	if (w->n_events > 1) {
		ctx = 0;
		rv = uvWriterIoSetup(1 /* Maximum concurrent requests */, &ctx,
				     req->errmsg);
		if (rv != 0) {
			goto out;
		}
	} else {
		ctx = w->ctx;
	}

	/* Submit the request */
	rv = UvOsIoSubmit(ctx, 1, &iocbs);
	if (rv != 0) {
		/* UNTESTED: since we're not using NOWAIT and the parameters are
		 * valid, this shouldn't fail. */
		UvOsErrMsg(req->errmsg, "io_submit", rv);
		rv = RAFT_IOERR;
		goto out_after_io_setup;
	}

	/* Wait for the request to complete */
	n_events = UvOsIoGetevents(ctx, 1, 1, &event, NULL);
	dqlite_assert(n_events == 1);
	if (n_events != 1) {
		/* UNTESTED */
		rv = n_events >= 0 ? -1 : n_events;
	}

out_after_io_setup:
	if (w->n_events > 1) {
		UvOsIoDestroy(ctx);
	}

out:
	if (rv != 0) {
		req->status = rv;
	} else {
		uvWriterReqSetStatus(req, (int)event.res);
	}

	return;
}

/* Callback fired when the event fd associated with AIO write requests should be
 * ready for reading (i.e. when a write has completed). */
static void uvWriterPollCb(uv_poll_t *poller, int status, int events)
{
	struct UvWriter *w = poller->data;
	uint64_t completed; /* True if the write is complete */
	unsigned i;
	int n_events;
	int rv;

	dqlite_assert(w->event_fd >= 0);
	dqlite_assert(status == 0);
	if (status != 0) {
		/* UNTESTED libuv docs: If an error happens while polling,
		 * status will be < 0 and corresponds with one of the UV_E*
		 * error codes. */
		goto fail_requests;
	}

	dqlite_assert(events & UV_READABLE);

	/* Read the event file descriptor */
	rv = (int)read(w->event_fd, &completed, sizeof completed);
	if (rv != sizeof completed) {
		/* UNTESTED: According to eventfd(2) this is the only possible
		 * failure mode, meaning that epoll has indicated that the event
		 * FD is not yet ready. */
		dqlite_assert(errno == EAGAIN);
		return;
	}

	/* TODO: this assertion fails in unit tests */
	/* dqlite_assert(completed == 1); */

	/* Try to fetch the write responses.
	 *
	 * If we got here at least one write should have completed and io_events
	 * should return immediately without blocking. */
	n_events =
	    UvOsIoGetevents(w->ctx, 1, (long int)w->n_events, w->events, NULL);
	dqlite_assert(n_events >= 1);
	if (n_events < 1) {
		/* UNTESTED */
		status = n_events == 0 ? -1 : n_events;
		goto fail_requests;
	}

	for (i = 0; i < (unsigned)n_events; i++) {
		struct io_event *event = &w->events[i];
		struct UvWriterReq *req = *((void **)&event->data);

		/* If we got EAGAIN, it means it was not possible to perform the
		 * write asynchronously, so let's fall back to the threadpool.
		 */
		if (event->res == -EAGAIN) {
			req->iocb.aio_flags &= (unsigned)~IOCB_FLAG_RESFD;
			req->iocb.aio_resfd = 0;
			req->iocb.aio_rw_flags &= ~RWF_NOWAIT;
			dqlite_assert(req->work.data == NULL);
			req->work.data = req;
			rv = uv_queue_work(w->loop, &req->work, uvWriterWorkCb,
					   uvWriterAfterWorkCb);
			if (rv != 0) {
				/* UNTESTED: with the current libuv
				 * implementation this should never fail. */
				UvOsErrMsg(req->errmsg, "uv_queue_work", rv);
				req->status = RAFT_IOERR;
				goto finish;
			}
			continue;
		}

		uvWriterReqSetStatus(req, (int)event->res);

	finish:
		uvWriterReqFinish(req);
	}

	return;

fail_requests:
	while (!queue_empty(&w->poll_queue)) {
		queue *head;
		struct UvWriterReq *req;
		head = queue_head(&w->poll_queue);
		req = QUEUE_DATA(head, struct UvWriterReq, queue);
		uvWriterReqSetStatus(req, status);
		uvWriterReqFinish(req);
	}
}

static void uvWriterPollerCloseCb(struct uv_handle_s *handle)
{
	struct UvWriter *w = handle->data;
	w->event_poller.data = NULL;

	/* Cancel all pending requests. */
	while (!queue_empty(&w->poll_queue)) {
		queue *head;
		struct UvWriterReq *req;
		head = queue_head(&w->poll_queue);
		req = QUEUE_DATA(head, struct UvWriterReq, queue);
		dqlite_assert(req->work.data == NULL);
		req->status = RAFT_CANCELED;
		uvWriterReqFinish(req);
	}

	if (w->check.data != NULL) {
		return;
	}

	uvWriterCleanUpAndFireCloseCb(w);
}

static int uvWriterAioInit(struct UvWriter *w, char *errmsg)
{
	int rv;

	/* Setup the AIO context. */
	rv = uvWriterIoSetup(w->n_events, &w->ctx, errmsg);
	if (rv != 0) {
		goto err;
	}

	/* Initialize the array of reusable event objects. */
	w->events = RaftHeapCalloc(w->n_events, sizeof *w->events);
	if (w->events == NULL) {
		/* UNTESTED: todo */
		ErrMsgOom(errmsg);
		rv = RAFT_NOMEM;
		goto err_after_io_setup;
	}

	/* Create an event file descriptor to get notified when a write has
	 * completed. */
	rv = UvOsEventfd(0, UV_FS_O_NONBLOCK);
	if (rv < 0) {
		/* UNTESTED: should fail only with ENOMEM */
		UvOsErrMsg(errmsg, "eventfd", rv);
		rv = RAFT_IOERR;
		goto err_after_events_alloc;
	}
	w->event_fd = rv;

	rv = uv_poll_init(w->loop, &w->event_poller, w->event_fd);
	if (rv != 0) {
		/* UNTESTED: with the current libuv implementation this should
		 * never fail. */
		UvOsErrMsg(errmsg, "uv_poll_init", rv);
		rv = RAFT_IOERR;
		goto err_after_event_fd;
	}
	w->event_poller.data = w;

	rv = uv_check_init(w->loop, &w->check);
	if (rv != 0) {
		/* UNTESTED: with the current libuv implementation this should
		 * never fail. */
		UvOsErrMsg(errmsg, "uv_check_init", rv);
		rv = RAFT_IOERR;
		goto err_after_event_fd;
	}
	w->check.data = w;

	rv = uv_poll_start(&w->event_poller, UV_READABLE, uvWriterPollCb);
	if (rv != 0) {
		/* UNTESTED: with the current libuv implementation this should
		 * never fail. */
		UvOsErrMsg(errmsg, "uv_poll_start", rv);
		rv = RAFT_IOERR;
		goto err_after_event_fd;
	}

	return 0;

err_after_event_fd:
	UvOsClose(w->event_fd);
err_after_events_alloc:
	RaftHeapFree(w->events);
err_after_io_setup:
	UvOsIoDestroy(w->ctx);
err:
	dqlite_assert(rv != 0);
	return rv;
}

static int uvWriterAioSubmit(struct UvWriter *w,
			     struct UvWriterReq *req,
			     const uv_buf_t bufs[],
			     unsigned n,
			     size_t offset)
{
	struct iocb *iocbs = &req->iocb;
	int rv;

	/* TODO: at the moment we are not leveraging the support for concurrent
	 *       writes, so ensure that we're getting write requests
	 *       sequentially. */
	if (w->n_events == 1) {
		dqlite_assert(queue_empty(&w->poll_queue));
		dqlite_assert(queue_empty(&w->work_queue));
	}

	dqlite_assert(w->event_fd >= 0);
	dqlite_assert(w->ctx != 0);

	req->work.data = NULL;
	memset(&req->iocb, 0, sizeof req->iocb);

	req->iocb.aio_fildes = (uint32_t)w->fd;
	req->iocb.aio_lio_opcode = IOCB_CMD_PWRITEV;
	req->iocb.aio_reqprio = 0;
	*((void **)(&req->iocb.aio_buf)) = (void *)bufs;
	req->iocb.aio_nbytes = n;
	req->iocb.aio_offset = (int64_t)offset;
	*((void **)(&req->iocb.aio_data)) = (void *)req;

#if defined(RWF_HIPRI)
	/* High priority request, if possible */
	/* TODO: do proper kernel feature detection for this one. */
	/* req->iocb.aio_rw_flags |= RWF_HIPRI; */
#endif

#if defined(RWF_DSYNC)
	/* Use per-request synchronous I/O if available. Otherwise, we have
	 * opened the file with O_DSYNC. */
	/* TODO: do proper kernel feature detection for this one. */
	/* req->iocb.aio_rw_flags |= RWF_DSYNC; */
#endif

	/* If io_submit can be run in a 100% non-blocking way, we'll try to
	 * write without using the threadpool. */
	if (w->async) {
		req->iocb.aio_flags |= IOCB_FLAG_RESFD;
		req->iocb.aio_resfd = (uint32_t)w->event_fd;
		req->iocb.aio_rw_flags |= RWF_NOWAIT;
	}

	/* Try to submit the write request asynchronously */
	if (w->async) {
		queue_insert_tail(&w->poll_queue, &req->queue);
		rv = UvOsIoSubmit(w->ctx, 1, &iocbs);

		/* If no error occurred, we're done, the write request was
		 * submitted. */
		if (rv == 0) {
			goto done;
		}

		queue_remove(&req->queue);

		/* Check the reason of the error. */
		switch (rv) {
			case UV_EAGAIN:
				break;
			default:
				/* Unexpected error */
				UvOsErrMsg(w->errmsg, "io_submit", rv);
				rv = RAFT_IOERR;
				goto err;
		}

		/* Submitting the write would block, or NOWAIT is not
		 * supported. Let's run this request in the threadpool. */
		req->iocb.aio_flags &= (unsigned)~IOCB_FLAG_RESFD;
		req->iocb.aio_resfd = 0;
		req->iocb.aio_rw_flags &= ~RWF_NOWAIT;
	}

	/* If we got here it means we need to run io_submit in the threadpool.
	 */
	queue_insert_tail(&w->work_queue, &req->queue);
	req->work.data = req;
	rv = uv_queue_work(w->loop, &req->work, uvWriterWorkCb,
			   uvWriterAfterWorkCb);
	if (rv != 0) {
		/* UNTESTED: with the current libuv implementation this can't
		 * fail. */
		req->work.data = NULL;
		queue_remove(&req->queue);
		UvOsErrMsg(w->errmsg, "uv_queue_work", rv);
		rv = RAFT_IOERR;
		goto err;
	}

done:
	return 0;

err:
	dqlite_assert(rv != 0);
	return rv;
}

static void uvWriterAioClose(struct UvWriter *w)
{
	int rv;

	/* We can close the event file descriptor right away, but we shouldn't
	 * close the main file descriptor or destroy the AIO context since there
	 * might be threadpool requests in flight. */
	UvOsClose(w->event_fd);

	rv = uv_poll_stop(&w->event_poller);
	dqlite_assert(rv == 0); /* Can this ever fail? */

	uv_close((struct uv_handle_s *)&w->event_poller, uvWriterPollerCloseCb);

	/* If we have requests executing in the threadpool, we need to wait for
	 * them. That's done in the check callback. */
	uvWriterDrainThenClose(w);
}

static void uvWriterAioDestroy(struct UvWriter *w)
{
	RaftHeapFree(w->events);
	UvOsIoDestroy(w->ctx);
}

static const struct UvWriterBackend uvWriterAioBackend = {
	.name = "aio",
	.init = uvWriterAioInit,
	.submit = uvWriterAioSubmit,
	.close = uvWriterAioClose,
	.destroy = uvWriterAioDestroy,
};
#endif /* DQLITE_HAVE_KAIO */

/******************************************************************************
 *
 * Backend: portable libuv threadpool (Linux/macOS/Windows).
 *
 *****************************************************************************/

#if defined(_WIN32)
#include <io.h> /* _get_osfhandle */

/* Windows durability (route B, see README.md "Durability on Windows"):
 * detect whether the OS handle backing a writer's fd was opened write-through
 * (FILE_FLAG_WRITE_THROUGH), i.e. whether O_DSYNC semantics were requested at
 * open time via the compat O_DSYNC -> UV_FS_O_DSYNC mapping. The query is
 * NtQueryInformationFile(FileModeInformation); it lives in ntdll, which dqlite
 * does not link against, so resolve it dynamically (ntdll is always loaded). */
struct uvWriterNtIoStatusBlock
{
	union {
		LONG Status;
		PVOID Pointer;
	} u;
	ULONG_PTR Information;
};
struct uvWriterNtFileModeInformation
{
	ULONG Mode;
};
#define UV__NT_FILE_MODE_INFORMATION 16      /* FileModeInformation */
#define UV__NT_FILE_WRITE_THROUGH 0x00000002 /* FILE_WRITE_THROUGH */
typedef LONG(WINAPI *uvWriterNtQueryInformationFileFn)(
    HANDLE,
    struct uvWriterNtIoStatusBlock *,
    PVOID,
    ULONG,
    ULONG /* FILE_INFORMATION_CLASS */);

/* Return whether the file handle backing fd was opened with write-through
 * (O_DSYNC) semantics. If the answer cannot be determined, err on the side of
 * durability and report true (the portable write path will then issue a
 * redundant-at-worst flush). */
static bool uvWriterFdRequestedDsync(uv_file fd)
{
	uvWriterNtQueryInformationFileFn query;
	struct uvWriterNtIoStatusBlock iosb;
	struct uvWriterNtFileModeInformation info = {0};
	HANDLE handle;
	LONG status;

	handle = (HANDLE)_get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE) {
		return true;
	}
	query = (uvWriterNtQueryInformationFileFn)(void *)GetProcAddress(
	    GetModuleHandleA("ntdll.dll"), "NtQueryInformationFile");
	if (query == NULL) {
		return true;
	}
	status = query(handle, &iosb, &info, sizeof info,
		       UV__NT_FILE_MODE_INFORMATION);
	if (status != 0) {
		return true;
	}
	return (info.Mode & UV__NT_FILE_WRITE_THROUGH) != 0;
}
#endif /* _WIN32 */

/* Threadpool worker for the portable backend: perform a plain blocking write
 * via libuv's cross-platform uv_fs_write (pwritev on POSIX, WriteFile on
 * Windows). Runs off the loop thread; uvWriterAfterWorkCb finishes the request
 * back on the loop thread. */
static void uvWriterWorkCbPortable(uv_work_t *work)
{
	struct UvWriterReq *req = work->data;
	struct UvWriter *w = req->writer;
	int rv = UvOsWrite(w->fd, req->tp_bufs, req->tp_nbufs, req->tp_offset);
#if defined(_WIN32)
	/* Windows durability (route B): FILE_FLAG_WRITE_THROUGH (route A, the
	 * O_DSYNC -> UV_FS_O_DSYNC mapping in the compat prelude) pushes the
	 * write past the OS file cache, but the storage device may still hold
	 * it in its own volatile cache -- NTFS requests FUA for write-through
	 * writes, and consumer SATA disks commonly ignore FUA. Follow every
	 * write on an O_DSYNC writer with an explicit fdatasync (libuv
	 * implements it as FlushFileBuffers, which sends a cache-flush command
	 * to the device), giving the same guarantee O_DSYNC provides on Linux.
	 * Still on the threadpool worker, so the loop thread is unaffected.
	 * Linux is untouched: there O_DSYNC is real and this block does not
	 * compile. */
	if (rv >= 0 && w->sync) {
		int frv = UvOsFdatasync(w->fd);
		if (frv != 0) {
			rv = frv;
		}
	}
#endif
	uvWriterReqSetStatus(req, rv);
}

static int uvWriterThreadpoolInit(struct UvWriter *w, char *errmsg)
{
	/* No kernel AIO context, eventfd, or poller. Writes run as blocking
	 * uv_fs_write calls in the libuv threadpool. Only the check handle
	 * (used to drain in-flight work on close) is needed. */
	int rv = uv_check_init(w->loop, &w->check);
	if (rv != 0) {
		UvOsErrMsg(errmsg, "uv_check_init", rv);
		return RAFT_IOERR;
	}
	w->check.data = w;
	return 0;
}

static int uvWriterThreadpoolSubmit(struct UvWriter *w,
				    struct UvWriterReq *req,
				    const uv_buf_t bufs[],
				    unsigned n,
				    size_t offset)
{
	int rv;

	req->work.data = req;
	req->tp_bufs = bufs;
	req->tp_nbufs = n;
	req->tp_offset = (int64_t)offset;

	queue_insert_tail(&w->work_queue, &req->queue);
	rv = uv_queue_work(w->loop, &req->work, uvWriterWorkCbPortable,
			   uvWriterAfterWorkCb);
	if (rv != 0) {
		/* UNTESTED: with the current libuv implementation this
		 * can't fail. */
		req->work.data = NULL;
		queue_remove(&req->queue);
		UvOsErrMsg(w->errmsg, "uv_queue_work", rv);
		return RAFT_IOERR;
	}
	return 0;
}

static void uvWriterThreadpoolClose(struct UvWriter *w)
{
	/* No eventfd/poller to tear down. Just wait for any in-flight
	 * threadpool writes to drain via the check handle. */
	uvWriterDrainThenClose(w);
}

static const struct UvWriterBackend uvWriterThreadpoolBackend = {
	.name = "threadpool",
	.init = uvWriterThreadpoolInit,
	.submit = uvWriterThreadpoolSubmit,
	.close = uvWriterThreadpoolClose,
	.destroy = NULL,
};

/******************************************************************************
 *
 * Public interface (backend-agnostic).
 *
 *****************************************************************************/

int UvWriterInit(struct UvWriter *w,
		 struct uv_loop_s *loop,
		 uv_file fd,
		 bool direct /* Whether to use direct I/O */,
		 bool async /* Whether async I/O is available */,
		 unsigned max_concurrent_writes,
		 char *errmsg)
{
	void *data = w->data;
	int rv;
	bool threadpool;

	memset(w, 0, sizeof *w);
	w->data = data;
	w->loop = loop;
	w->fd = fd;
#if defined(_WIN32)
	/* Windows durability: remember whether this fd was opened with O_DSYNC
	 * (write-through) semantics, so that the portable write path can follow
	 * each write with an explicit fdatasync. All production writers qualify
	 * (their fds come from UvFsAllocateFile, which requests O_DSYNC). */
	w->sync = uvWriterFdRequestedDsync(fd);
#endif
	w->async = async;
	w->n_events = max_concurrent_writes;
	w->event_fd = -1;
	w->event_poller.data = NULL;
	w->check.data = NULL;
	w->close_cb = NULL;
	queue_init(&w->poll_queue);
	queue_init(&w->work_queue);
	w->closing = false;
	w->errmsg = errmsg;

	/* Select the raw-write backend. On builds with kernel AIO (Linux) the
	 * AIO backend is always the default, matching upstream: when fully
	 * async I/O is unsupported (async == false, e.g. tmpfs or ZFS, where
	 * RWF_NOWAIT submission fails the probe) it runs the very same
	 * io_submit + io_getevents pair blocking in the libuv threadpool
	 * (uvWriterWorkCb), NOT plain pwritev. The portable threadpool backend
	 * is reachable on such builds only by explicit opt-in via
	 * DQLITE_IO_BACKEND=threadpool, so the production Linux write
	 * mechanism is unchanged by the port. Builds without kernel AIO
	 * (Windows, macOS, DQLITE_DISABLE_KAIO) have only the portable
	 * backend. */
#if defined(DQLITE_HAVE_KAIO)
	threadpool = uvWriterThreadpoolForced();
	w->backend =
	    threadpool ? &uvWriterThreadpoolBackend : &uvWriterAioBackend;
#else
	threadpool = true;
	w->backend = &uvWriterThreadpoolBackend;
#endif
	w->threadpool = threadpool;

	/* Set direct I/O if available. */
	if (direct) {
		rv = UvOsSetDirectIo(w->fd);
		if (rv != 0) {
			UvOsErrMsg(errmsg, "fcntl", rv);
			return RAFT_IOERR;
		}
	}

	return w->backend->init(w, errmsg);
}

void UvWriterClose(struct UvWriter *w, UvWriterCloseCb cb)
{
	dqlite_assert(!w->closing);
	w->closing = true;
	w->close_cb = cb;
	w->backend->close(w);
}

int UvWriterSubmit(struct UvWriter *w,
		   struct UvWriterReq *req,
		   const uv_buf_t bufs[],
		   unsigned n,
		   size_t offset,
		   UvWriterReqCb cb)
{
	dqlite_assert(!w->closing);
	dqlite_assert(w->fd >= 0);
	dqlite_assert(req != NULL);
	dqlite_assert(bufs != NULL);
	dqlite_assert(n > 0);

	req->writer = w;
	req->len = lenOfBufs(bufs, n);
	req->status = -1;
	req->cb = cb;
	memset(req->errmsg, 0, sizeof req->errmsg);

	return w->backend->submit(w, req, bufs, n, offset);
}
