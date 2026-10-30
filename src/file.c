/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poor_loop_uring_array.h>

#include "file.h"
#include "logger.h"

poor_loop_group_attach(file_fd, struct file, handle, stat_op, on_stat);
poor_loop_group_attach(file_fd, struct file, handle, read_op, on_read);
poor_loop_group_attach(file_fd, struct file, handle, truncate_op, on_truncate);
poor_loop_group_attach(file_fd, struct file, handle, write_op, on_write);
poor_loop_group_attach(file_fd, struct file, handle, sync_op, on_sync);

static void finish(struct file *file)
{
	file->busy = false;
	file->listener->done(file, file->error);
}

static void read_next(struct file *file)
{
	io_uring_prep_read_array(need_sqe(poor_loop_group_get_sqe_or_submit(file->loop, &file->handle, &file->read_op)),
				 file->handle.fd, arrview_cfront(file->done, array_ptr(file->in, file->size)), -1);
}

static void write_next(struct file *file)
{
	if (file->done == file->size) {
		io_uring_prep_fsync(need_sqe(poor_loop_group_get_sqe_or_submit(file->loop, &file->handle, &file->sync_op)),
				    file->handle.fd, 0);
		return;
	}
	io_uring_prep_write_array(need_sqe(poor_loop_group_get_sqe_or_submit(file->loop, &file->handle, &file->write_op)),
				  file->handle.fd, arrview_cfront(file->done, array_ptr(file->out, file->size)), -1);
}

/* The file is identified before it is read or emptied. One to read that isn't there is empty. */
static void on_open(struct poor_loop *, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct file *file = container_of(op, struct file, open_op);

	if (cqe->res >= 0 && file->error)
		poor_loop_close_fd_or_submit(file->loop, cqe->res, IOSQE_ASYNC);
	if (cqe->res < 0 && !file->error && (file->writing || cqe->res != -ENOENT))
		file->error = -cqe->res;
	if (cqe->res < 0 || file->error) {
		finish(file);
		return;
	}
	poor_loop_fd_init(&file->handle, cqe->res);
	io_uring_prep_statx(need_sqe(poor_loop_group_get_sqe_or_submit(file->loop, &file->handle, &file->stat_op)),
			    file->handle.fd, "", AT_EMPTY_PATH, STATX_INO, &file->statx);
}

/* A read goes on without knowing the file, a write doesn't */
POOR_LOOP_GROUP_OP(on_stat, loop, handle, op, cqe)
{
	struct file *file = container_of(handle, struct file, handle);

	if (file->error)
		return;
	if (!cqe->res) {
		file->inode = file->statx.stx_ino;
		file->device = file->statx.stx_dev_major << 20 | file->statx.stx_dev_minor;
		file->error = file->listener->identified(file);
		file->identified = !file->error;
	} else if (file->writing) {
		file->error = -cqe->res;
	}
	if (file->error)
		return;
	if (!file->writing) {
		read_next(file);
		return;
	}
	io_uring_prep_ftruncate(need_sqe(poor_loop_group_get_sqe_or_submit(file->loop, &file->handle, &file->truncate_op)),
				file->handle.fd, 0);
}

POOR_LOOP_GROUP_OP(on_read, loop, handle, op, cqe)
{
	struct file *file = container_of(handle, struct file, handle);

	if (file->error)
		return;
	if (cqe->res < 0) {
		file->error = -cqe->res;
		return;
	}
	file->done += cqe->res;
	if (cqe->res && file->done < file->size)
		read_next(file);
}

POOR_LOOP_GROUP_OP(on_truncate, loop, handle, op, cqe)
{
	struct file *file = container_of(handle, struct file, handle);

	if (cqe->res < 0)
		file->error = -cqe->res;
	else
		write_next(file);
}

POOR_LOOP_GROUP_OP(on_write, loop, handle, op, cqe)
{
	struct file *file = container_of(handle, struct file, handle);

	if (cqe->res <= 0) {
		file->error = cqe->res ? -cqe->res : EIO;
		return;
	}
	file->done += cqe->res;
	write_next(file);
}

POOR_LOOP_GROUP_OP(on_sync, loop, handle, op, cqe)
{
	struct file *file = container_of(handle, struct file, handle);

	if (cqe->res < 0)
		file->error = -cqe->res;
}

POOR_LOOP_FD_DONE(file_fd, loop, handle)
{
	finish(container_of(handle, struct file, handle));
}

void file_init(struct file *file, struct poor_loop *loop, const struct file_listener *listener, const char *path)
{
	file->loop = loop;
	file->listener = listener;
	file->path = path;
	poor_loop_op_init(&file->open_op, on_open);
	poor_loop_op_init(&file->stat_op, on_stat);
	poor_loop_op_init(&file->read_op, on_read);
	poor_loop_op_init(&file->truncate_op, on_truncate);
	poor_loop_op_init(&file->write_op, on_write);
	poor_loop_op_init(&file->sync_op, on_sync);
	file->busy = file->identified = false;
}

static void start(struct file *file, size_t size, bool writing)
{
	file->size = size;
	file->done = 0;
	file->error = 0;
	file->writing = writing;
	file->busy = true;
	io_uring_prep_openat(need_sqe(poor_loop_get_sqe_or_submit(file->loop, &file->open_op)), AT_FDCWD, file->path,
			     writing ? O_WRONLY | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0666);
}

void file_read_impl(struct file *file, size_t capacity, char (*bytes)[capacity])
{
	file->in = *bytes;
	start(file, capacity, false);
}

void file_write_impl(struct file *file, size_t size, const char (*bytes)[size])
{
	file->out = *bytes;
	start(file, size, true);
}

void file_cancel(struct file *file)
{
	if (!file->busy || file->writing)
		return;
	file->error = ECANCELED;
	foreach_array_ref(((struct poor_loop_op *[]){ &file->open_op, &file->read_op }), op)
		if ((*op)->pending)
			io_uring_prep_cancel(need_sqe(poor_loop_get_untracked_sqe_or_submit(file->loop)), *op, 0);
}
