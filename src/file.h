/* SPDX-License-Identifier: MIT */
#ifndef FILE_H
#define FILE_H

#include <linux/stat.h>
#include <poor_array.h>
#include <poor_loop_fd.h>

poor_loop_fd_declare(file_fd);

struct file;

struct file_listener {
	/* The file is 'inode' on 'device'. Returns 0 to go on, or the error to end with. */
	int (*identified)(struct file *file);
	/* A read or a write ended, the file is closed: 'error' is 0, or what failed */
	void (*done)(struct file *file, int error);
};

/*
 * The file at 'path', which is read whole into memory of the caller, or written from it, one or the other at a time
 * while 'busy'. 'done' bytes of 'size' are read or written. The file is 'inode' on 'device' once it is 'identified',
 * a file that the listener refuses isn't.
 */
struct file {
	struct poor_loop *loop;
	const struct file_listener *listener;
	const char *path;
	file_fd handle;
	struct poor_loop_op open_op, stat_op, read_op, truncate_op, write_op, sync_op;
	struct statx statx;
	union {
		char *in;
		const char *out;
	};
	size_t done, size;
	uint64_t inode;
	uint32_t device;
	int error;
	bool busy, writing, identified;
};

/* 'path' stays as long as the file */
void file_init(struct file *file, struct poor_loop *loop, const struct file_listener *listener, const char *path);
/* Read the file into 'bytes', as much as fits. A file that isn't there reads as empty. */
void file_read_impl(struct file *file, size_t capacity, char (*bytes)[capacity]);
#define file_read(file, bytes) file_read_impl(file, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* Write 'bytes' over the file, or into a new one. A file is emptied only once it is identified. */
void file_write_impl(struct file *file, size_t size, const char (*bytes)[size]);
#define file_write(file, bytes) file_write_impl(file, ARRAY_SIZE(bytes), &auto_arr(bytes))
/* A read that goes on ends early, with ECANCELED */
void file_cancel(struct file *file);

#endif
