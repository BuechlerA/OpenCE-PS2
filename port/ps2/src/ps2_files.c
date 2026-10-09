/*
PS2_FILES.C

What the file layer (port/linux/src/xbox_files.c) needs that the EE's newlib
declares but does not have: pread and pwrite, the positioned reads and
writes the layer's ReadFile and WriteFile with an OVERLAPPED offset are.

Each is a seek, the read or write, and a seek back to where the descriptor
was, under one lock: ps2sdk's file devices have no positioned calls, and the
descriptor's position is shared with its plain reads and writes.
*/

#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

static pthread_mutex_t positioned_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t positioned(int descriptor, void *read_buffer, const void *write_buffer, size_t count,
	off_t offset)
{
	off_t position;
	ssize_t result = -1;
	int error = 0;

	pthread_mutex_lock(&positioned_lock);
	position = lseek(descriptor, 0, SEEK_CUR);
	if (position == (off_t)-1 || lseek(descriptor, offset, SEEK_SET) == (off_t)-1)
	{
		error = errno;
	}
	else
	{
		result = read_buffer ? read(descriptor, read_buffer, count) : write(descriptor, write_buffer, count);
		error = errno;
		lseek(descriptor, position, SEEK_SET);
	}
	pthread_mutex_unlock(&positioned_lock);
	errno = error;
	return result;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned(descriptor, buffer, NULL, count, offset);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned(descriptor, NULL, buffer, count, offset);
}
