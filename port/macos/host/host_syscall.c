/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h
sends every call here), which was built for Linux: its system call numbers,
flags, structures and errno values are Linux's (AArch64's), and are
translated to Darwin's here, as port/android/host/host_syscall.c passes them
to Android's kernel. Pointers arrive as guest addresses.

- files: open flags, the at-functions' special values, struct stat, the
  directory entries of getdents64 (read with readdir from a stream kept for
  the descriptor), fcntl;
- time: the guest's timespec and timeval have a 32-bit time_t, as in the
  MSVC runtime; the clocks are numbered differently; clock_nanosleep is
  made of nanosleep;
- futexes: Darwin's __ulock_wait and __ulock_wake, as its own C++ runtime
  uses them;
- memory mappings: host_memory.c;
- the standard output and error streams go to the host's log;
- /proc/self/exe is host_support_directory()'s halo (port/macos/README.md);
- process exit, and calls that have no meaning for the guest (signal
  handlers, which the host owns).
*/

#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ---------- Linux's numbers (AArch64, asm-generic) */

enum
{
	LINUX_getcwd = 17, LINUX_fcntl = 25, LINUX_ioctl = 29, LINUX_flock = 32, LINUX_mkdirat = 34,
	LINUX_unlinkat = 35, LINUX_renameat = 38, LINUX_statfs = 43, LINUX_fstatfs = 44, LINUX_ftruncate = 46,
	LINUX_faccessat = 48, LINUX_chdir = 49, LINUX_fchmod = 52, LINUX_fchmodat = 53, LINUX_openat = 56,
	LINUX_close = 57, LINUX_pipe2 = 59, LINUX_getdents64 = 61, LINUX_lseek = 62, LINUX_read = 63,
	LINUX_write = 64, LINUX_readv = 65, LINUX_writev = 66, LINUX_pread64 = 67, LINUX_pwrite64 = 68,
	LINUX_preadv = 69, LINUX_pwritev = 70, LINUX_pselect6 = 72, LINUX_ppoll = 73, LINUX_readlinkat = 78,
	LINUX_newfstatat = 79, LINUX_fstat = 80, LINUX_fsync = 82, LINUX_fdatasync = 83, LINUX_utimensat = 88,
	LINUX_exit = 93, LINUX_exit_group = 94, LINUX_set_tid_address = 96, LINUX_futex = 98,
	LINUX_nanosleep = 101, LINUX_clock_gettime = 113, LINUX_clock_getres = 114, LINUX_clock_nanosleep = 115,
	LINUX_sched_getaffinity = 123, LINUX_sched_yield = 124, LINUX_kill = 129, LINUX_tkill = 130,
	LINUX_tgkill = 131, LINUX_sigaltstack = 132, LINUX_rt_sigaction = 134, LINUX_rt_sigprocmask = 135,
	LINUX_uname = 160, LINUX_getrlimit = 163, LINUX_umask = 166, LINUX_gettimeofday = 169,
	LINUX_getpid = 172, LINUX_getppid = 173, LINUX_getuid = 174, LINUX_geteuid = 175, LINUX_getgid = 176,
	LINUX_getegid = 177, LINUX_gettid = 178, LINUX_sysinfo = 179, LINUX_brk = 214, LINUX_munmap = 215,
	LINUX_mremap = 216, LINUX_mmap = 222, LINUX_mprotect = 226, LINUX_madvise = 233,
	LINUX_prlimit64 = 261, LINUX_renameat2 = 276, LINUX_getrandom = 278, LINUX_membarrier = 283,
	LINUX_statx = 291, LINUX_dup = 23, LINUX_dup3 = 24,
};

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EACCESS 0x200

#define LINUX_O_ACCMODE 03
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_NOCTTY 0400
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DSYNC 010000
#define LINUX_O_DIRECTORY 040000
#define LINUX_O_NOFOLLOW 0100000
#define LINUX_O_CLOEXEC 02000000

#define LINUX_F_DUPFD_CLOEXEC 1030

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_COMMAND 0x7f
#define LINUX_FUTEX_CLOCK_REALTIME 256

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* the AArch64 kernel's struct stat (port/android/guest/libc/arch/arm64_32/kstat.h) */
struct linux_stat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_seconds, st_atime_nanoseconds;
	int64_t st_mtime_seconds, st_mtime_nanoseconds;
	int64_t st_ctime_seconds, st_ctime_nanoseconds;
	uint32_t unused[2];
};

struct linux_dirent64
{
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

struct linux_utsname
{
	char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

struct linux_rlimit
{
	uint64_t current, maximum;
};

#define GUEST(type, value) GUEST_TO_HOST_OR_NULL(type, value)

/* Darwin's futexes (its libc++ waits on atomics with them) */
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100
#define ULF_NO_ERRNO 0x01000000
extern int __ulock_wait2(uint32_t operation, void *address, uint64_t value, uint64_t timeout_ns, uint64_t value2);
extern int __ulock_wake(uint32_t operation, void *address, uint64_t wake_value);

/* ---------- errno */

int host_linux_errno(int darwin)
{
	switch (darwin)
	{
	case EDEADLK: return 35;
	case EAGAIN: return 11;
	case EINPROGRESS: return 115;
	case EALREADY: return 114;
	case ENOTSOCK: return 88;
	case EDESTADDRREQ: return 89;
	case EMSGSIZE: return 90;
	case EPROTOTYPE: return 91;
	case ENOPROTOOPT: return 92;
	case EPROTONOSUPPORT: return 93;
	case ESOCKTNOSUPPORT: return 94;
	case ENOTSUP: return 95;
	case EOPNOTSUPP: return 95;
	case EPFNOSUPPORT: return 96;
	case EAFNOSUPPORT: return 97;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ENETRESET: return 102;
	case ECONNABORTED: return 103;
	case ECONNRESET: return 104;
	case ENOBUFS: return 105;
	case EISCONN: return 106;
	case ENOTCONN: return 107;
	case ESHUTDOWN: return 108;
	case ETOOMANYREFS: return 109;
	case ETIMEDOUT: return 110;
	case ECONNREFUSED: return 111;
	case ELOOP: return 40;
	case ENAMETOOLONG: return 36;
	case EHOSTDOWN: return 112;
	case EHOSTUNREACH: return 113;
	case ENOTEMPTY: return 39;
	case EUSERS: return 87;
	case EDQUOT: return 122;
	case ESTALE: return 116;
	case ENOLCK: return 37;
	case ENOSYS: return 38;
	case EOVERFLOW: return 75;
	case ECANCELED: return 125;
	case EIDRM: return 43;
	case ENOMSG: return 42;
	case EILSEQ: return 84;
	case EBADMSG: return 74;
	case EMULTIHOP: return 72;
	case ENODATA: return 61;
	case ENOLINK: return 67;
	case ENOSR: return 63;
	case ENOSTR: return 60;
	case EPROTO: return 71;
	case ETIME: return 62;
	case ENOTRECOVERABLE: return 131;
	case EOWNERDEAD: return 130;
	default:
		/* 1 to 34 are the same, but for EDEADLK */
		return darwin > 0 && darwin <= 34 ? darwin : 5;
	}
}

int host_errno(void)
{
	return host_linux_errno(errno);
}

static long result_of(long value)
{
	return value == -1 ? -host_linux_errno(errno) : value;
}

/* ---------- conversions */

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!value)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!result)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static int directory_descriptor(long long fd)
{
	return (int)fd == LINUX_AT_FDCWD ? AT_FDCWD : (int)fd;
}

static int open_flags(long long flags)
{
	int result = (int)(flags & LINUX_O_ACCMODE);

	if (flags & LINUX_O_CREAT) result |= O_CREAT;
	if (flags & LINUX_O_EXCL) result |= O_EXCL;
	if (flags & LINUX_O_NOCTTY) result |= O_NOCTTY;
	if (flags & LINUX_O_TRUNC) result |= O_TRUNC;
	if (flags & LINUX_O_APPEND) result |= O_APPEND;
	if (flags & LINUX_O_NONBLOCK) result |= O_NONBLOCK;
	if (flags & LINUX_O_DSYNC) result |= O_DSYNC;
	if (flags & LINUX_O_DIRECTORY) result |= O_DIRECTORY;
	if (flags & LINUX_O_NOFOLLOW) result |= O_NOFOLLOW;
	if (flags & LINUX_O_CLOEXEC) result |= O_CLOEXEC;
	return result;
}

/* F_GETFL's flags, back */
static long linux_status_flags(int flags)
{
	long result = flags & O_ACCMODE;

	if (flags & O_APPEND) result |= LINUX_O_APPEND;
	if (flags & O_NONBLOCK) result |= LINUX_O_NONBLOCK;
	return result;
}

static void stat_out(uint64_t address, const struct stat *information)
{
	struct linux_stat *result = GUEST(struct linux_stat *, address);

	if (!result)
		return;
	memset(result, 0, sizeof(*result));
	result->st_dev = (uint64_t)information->st_dev;
	result->st_ino = information->st_ino;
	result->st_mode = information->st_mode;
	result->st_nlink = information->st_nlink;
	result->st_uid = information->st_uid;
	result->st_gid = information->st_gid;
	result->st_rdev = (uint64_t)information->st_rdev;
	result->st_size = information->st_size;
	result->st_blksize = information->st_blksize;
	result->st_blocks = information->st_blocks;
	result->st_atime_seconds = information->st_atimespec.tv_sec;
	result->st_atime_nanoseconds = information->st_atimespec.tv_nsec;
	result->st_mtime_seconds = information->st_mtimespec.tv_sec;
	result->st_mtime_nanoseconds = information->st_mtimespec.tv_nsec;
	result->st_ctime_seconds = information->st_ctimespec.tv_sec;
	result->st_ctime_nanoseconds = information->st_ctimespec.tv_nsec;
}

/* /proc/self/exe: the folder the game's own files go in (port_config.c's
config.toml, xbox_files.c's extracted maps) is the executable's for the
guest */
static const char *self_executable(void)
{
	static char path[1024];

	if (!path[0])
		snprintf(path, sizeof(path), "%s/halo", host_support_directory());
	return path;
}

/* ---------- standard output and error */

static long write_log(int fd, const char *bytes, size_t size)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	(void)fd;
	pthread_mutex_lock(&lock);
	fwrite(bytes, 1, size, stderr);
	pthread_mutex_unlock(&lock);
	return (long)size;
}

static int guest_vector(uint64_t vector, int count, struct iovec *host_vector)
{
	const struct guest_iovec *guest = GUEST(const struct guest_iovec *, vector);
	int index;

	if (count < 0 || count > 64 || (count && !guest))
		return 0;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest[index].base);
		host_vector[index].iov_len = guest[index].length;
	}
	return 1;
}

static long guest_writev(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	int index;

	if (!guest_vector(vector, count, host_vector))
		return -22;
	if (fd == 1 || fd == 2)
	{
		long total = 0;

		for (index = 0; index < count; index++)
			total += write_log(fd, host_vector[index].iov_base, host_vector[index].iov_len);
		return total;
	}
	if (positional)
		return result_of(pwritev(fd, host_vector, count, offset));
	return result_of(writev(fd, host_vector, count));
}

static long guest_readv(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];

	if (!guest_vector(vector, count, host_vector))
		return -22;
	if (positional)
		return result_of(preadv(fd, host_vector, count, offset));
	return result_of(readv(fd, host_vector, count));
}

/* ---------- directories

musl reads a directory with getdents64 on the descriptor its opendir
opened. Darwin's readdir reads it here, from a stream of a duplicate of the
descriptor, kept until the guest closes it. */

#define DIRECTORY_SLOTS 64

static struct
{
	int fd;
	DIR *stream;
	long position;
} directories[DIRECTORY_SLOTS];
static pthread_mutex_t directory_lock = PTHREAD_MUTEX_INITIALIZER;

static void directory_forget(int fd)
{
	int index;

	pthread_mutex_lock(&directory_lock);
	for (index = 0; index < DIRECTORY_SLOTS; index++)
	{
		if (directories[index].stream && directories[index].fd == fd)
		{
			closedir(directories[index].stream);
			directories[index].stream = NULL;
		}
	}
	pthread_mutex_unlock(&directory_lock);
}

static long guest_getdents64(int fd, uint64_t buffer, uint32_t size)
{
	uint8_t *out = GUEST(uint8_t *, buffer);
	uint32_t used = 0;
	int index, slot = -1;
	DIR *stream;

	if (!out)
		return -14;
	pthread_mutex_lock(&directory_lock);
	for (index = 0; index < DIRECTORY_SLOTS; index++)
	{
		if (directories[index].stream && directories[index].fd == fd)
			slot = index;
	}
	if (slot < 0)
	{
		int copy = dup(fd);

		for (index = 0; index < DIRECTORY_SLOTS && slot < 0; index++)
		{
			if (!directories[index].stream)
				slot = index;
		}
		stream = copy >= 0 && slot >= 0 ? fdopendir(copy) : NULL;
		if (!stream)
		{
			int error = errno;

			if (copy >= 0)
				close(copy);
			pthread_mutex_unlock(&directory_lock);
			return slot < 0 ? -24 : -host_linux_errno(error);
		}
		directories[slot].fd = fd;
		directories[slot].stream = stream;
		directories[slot].position = 0;
	}
	stream = directories[slot].stream;
	for (;;)
	{
		long position = telldir(stream);
		struct dirent *entry = readdir(stream);
		size_t length, record;
		struct linux_dirent64 *result;

		if (!entry)
			break;
		length = strlen(entry->d_name);
		record = (offsetof(struct linux_dirent64, d_name) + length + 1 + 7) & ~(size_t)7;
		if (used + record > size)
		{
			seekdir(stream, position);
			if (!used)
			{
				pthread_mutex_unlock(&directory_lock);
				return -22;
			}
			break;
		}
		result = (struct linux_dirent64 *)(out + used);
		result->d_ino = entry->d_ino;
		result->d_off = ++directories[slot].position;
		result->d_reclen = (uint16_t)record;
		result->d_type = entry->d_type;
		memcpy(result->d_name, entry->d_name, length + 1);
		used += (uint32_t)record;
	}
	pthread_mutex_unlock(&directory_lock);
	return used;
}

/* ---------- time */

static clockid_t darwin_clock(long long clock)
{
	switch (clock)
	{
	case 0: case 5: return CLOCK_REALTIME;
	case 2: return CLOCK_PROCESS_CPUTIME_ID;
	case 3: return CLOCK_THREAD_CPUTIME_ID;
	case 4: return CLOCK_MONOTONIC_RAW;
	default: return CLOCK_MONOTONIC;
	}
}

static long sleep_for(const struct timespec *request, uint64_t remaining)
{
	struct timespec left;
	long result = result_of(nanosleep(request, &left));

	if (result == -4)
		timespec_out(remaining, &left);
	return result;
}

static long guest_clock_nanosleep(long long clock, int flags, uint64_t request_address, uint64_t remaining)
{
	struct timespec request, now;

	if (!timespec_in(request_address, &request))
		return -14;
	/* TIMER_ABSTIME */
	if (flags & 1)
	{
		clock_gettime(darwin_clock(clock), &now);
		request.tv_sec -= now.tv_sec;
		request.tv_nsec -= now.tv_nsec;
		if (request.tv_nsec < 0)
		{
			request.tv_nsec += 1000000000L;
			request.tv_sec--;
		}
		if (request.tv_sec < 0)
			return 0;
		remaining = 0;
	}
	return sleep_for(&request, remaining);
}

/* ---------- futexes */

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2,
	uint32_t value3)
{
	void *word = GUEST(void *, address);
	int command = operation & LINUX_FUTEX_COMMAND;

	(void)address2;
	(void)value3;
	if (!word)
		return -14;
	switch (command)
	{
	case LINUX_FUTEX_WAIT:
	case LINUX_FUTEX_WAIT_BITSET:
	{
		struct timespec wait;
		uint64_t nanoseconds = 0;
		int result;

		if (timespec_in(timeout, &wait))
		{
			if (command == LINUX_FUTEX_WAIT_BITSET)
			{
				/* (an absolute time, on the monotonic clock or the real one) */
				struct timespec now;

				clock_gettime(operation & LINUX_FUTEX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
				wait.tv_sec -= now.tv_sec;
				wait.tv_nsec -= now.tv_nsec;
				if (wait.tv_nsec < 0)
				{
					wait.tv_nsec += 1000000000L;
					wait.tv_sec--;
				}
				if (wait.tv_sec < 0)
					return -110;
			}
			nanoseconds = (uint64_t)wait.tv_sec * 1000000000ULL + (uint64_t)wait.tv_nsec;
			/* (0 would wait for ever) */
			if (!nanoseconds)
				return -110;
		}
		result = __ulock_wait2(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, word, value, nanoseconds, 0);
		if (result >= 0)
			return 0;
		if (result == -ETIMEDOUT)
			return -110;
		if (result == -EINTR)
			return -4;
		return -11;
	}
	case LINUX_FUTEX_WAKE:
	case LINUX_FUTEX_WAKE_BITSET:
		if (!value)
			return 0;
		__ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO | (value > 1 ? ULF_WAKE_ALL : 0), word, 0);
		return 1;
	case LINUX_FUTEX_REQUEUE:
	case LINUX_FUTEX_CMP_REQUEUE:
		/* those requeued are woken instead, to wait again (a spurious
		wakeup, which every futex user allows) */
		if (command == LINUX_FUTEX_CMP_REQUEUE && *(volatile uint32_t *)word != value3)
			return -11;
		__ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO | ULF_WAKE_ALL, word, 0);
		return 1;
	default:
		host_logf(HOST_LOG_WARN, "guest futex operation %d is not supported", operation);
		return -38;
	}
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c, long long d, long long e,
	long long f)
{
	switch (number)
	{
	case LINUX_read:
		return result_of(read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case LINUX_write:
		if (a == 1 || a == 2)
			return write_log((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case LINUX_writev:
		return guest_writev((int)a, (uint64_t)b, (int)c, 0, 0);
	case LINUX_pwritev:
		return guest_writev((int)a, (uint64_t)b, (int)c, d, 1);
	case LINUX_readv:
		return guest_readv((int)a, (uint64_t)b, (int)c, 0, 0);
	case LINUX_preadv:
		return guest_readv((int)a, (uint64_t)b, (int)c, d, 1);
	case LINUX_pread64:
		return result_of(pread((int)a, GUEST(void *, b), (size_t)(uint32_t)c, d));
	case LINUX_pwrite64:
		return result_of(pwrite((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, d));
	case LINUX_openat:
		return result_of(openat(directory_descriptor(a), GUEST(const char *, b), open_flags(c), (int)d));
	case LINUX_close:
		directory_forget((int)a);
		/* (the standard streams stay the host's) */
		if (a >= 0 && a <= 2)
			return 0;
		return result_of(close((int)a));
	case LINUX_lseek:
		return result_of(lseek((int)a, b, (int)c));
	case LINUX_getdents64:
		return guest_getdents64((int)a, (uint64_t)b, (uint32_t)c);
	case LINUX_fstat:
	{
		struct stat information;
		long result = result_of(fstat((int)a, &information));

		if (!result)
			stat_out((uint64_t)b, &information);
		return result;
	}
	case LINUX_newfstatat:
	{
		struct stat information;
		const char *path = GUEST(const char *, b);
		long result;

		/* (AT_EMPTY_PATH: the descriptor itself) */
		if ((d & 0x1000) && path && !*path)
			result = result_of(fstat((int)a, &information));
		else
			result = result_of(fstatat(directory_descriptor(a), path, &information,
				(d & LINUX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0));
		if (!result)
			stat_out((uint64_t)c, &information);
		return result;
	}
	case LINUX_unlinkat:
		return result_of(unlinkat(directory_descriptor(a), GUEST(const char *, b),
			(c & LINUX_AT_REMOVEDIR) ? AT_REMOVEDIR : 0));
	case LINUX_renameat:
		return result_of(renameat(directory_descriptor(a), GUEST(const char *, b), directory_descriptor(c),
			GUEST(const char *, d)));
	case LINUX_renameat2:
		/* (no flags: RENAME_NOREPLACE and the others are not used) */
		if (e)
			return -22;
		return result_of(renameat(directory_descriptor(a), GUEST(const char *, b), directory_descriptor(c),
			GUEST(const char *, d)));
	case LINUX_mkdirat:
		return result_of(mkdirat(directory_descriptor(a), GUEST(const char *, b), (mode_t)c));
	case LINUX_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case LINUX_fchmodat:
		return result_of(fchmodat(directory_descriptor(a), GUEST(const char *, b), (mode_t)c, 0));
	case LINUX_faccessat:
		return result_of(faccessat(directory_descriptor(a), GUEST(const char *, b), (int)c,
			(d & LINUX_AT_EACCESS) ? AT_EACCESS : 0));
	case LINUX_ftruncate:
		return result_of(ftruncate((int)a, b));
	case LINUX_fsync:
	case LINUX_fdatasync:
		return result_of(fsync((int)a));
	case LINUX_fcntl:
		switch ((int)b)
		{
		case F_DUPFD:
			return result_of(fcntl((int)a, F_DUPFD, (int)c));
		case LINUX_F_DUPFD_CLOEXEC:
			return result_of(fcntl((int)a, F_DUPFD_CLOEXEC, (int)c));
		case F_GETFD:
		case F_SETFD:
			return result_of(fcntl((int)a, (int)b, (int)c));
		case F_GETFL:
		{
			int flags = fcntl((int)a, F_GETFL);

			return flags < 0 ? result_of(flags) : linux_status_flags(flags);
		}
		case F_SETFL:
			return result_of(fcntl((int)a, F_SETFL, open_flags(c) & (O_APPEND | O_NONBLOCK)));
		default:
			return -22;
		}
	case LINUX_getcwd:
	{
		char *buffer = GUEST(char *, a);

		if (!getcwd(buffer, (size_t)(uint32_t)b))
			return -host_linux_errno(errno);
		return (long long)strlen(buffer) + 1;
	}
	case LINUX_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case LINUX_readlinkat:
	{
		const char *path = GUEST(const char *, b);
		char *buffer = GUEST(char *, c);

		if (path && !strcmp(path, "/proc/self/exe"))
		{
			size_t length = strlen(self_executable());

			if (length > (size_t)(uint32_t)d)
				length = (size_t)(uint32_t)d;
			memcpy(buffer, self_executable(), length);
			return (long long)length;
		}
		return result_of(readlinkat(directory_descriptor(a), path, buffer, (size_t)(uint32_t)d));
	}
	case LINUX_dup:
		return result_of(dup((int)a));
	case LINUX_dup3:
		if (dup2((int)a, (int)b) < 0)
			return -host_linux_errno(errno);
		if (c & LINUX_O_CLOEXEC)
			fcntl((int)b, F_SETFD, FD_CLOEXEC);
		return b;
	case LINUX_pipe2:
	{
		int32_t *result = GUEST(int32_t *, a);
		int pipes[2];

		if (pipe(pipes) != 0)
			return -host_linux_errno(errno);
		if (b & LINUX_O_CLOEXEC)
		{
			fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
			fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
		}
		if (b & LINUX_O_NONBLOCK)
		{
			fcntl(pipes[0], F_SETFL, O_NONBLOCK);
			fcntl(pipes[1], F_SETFL, O_NONBLOCK);
		}
		result[0] = pipes[0];
		result[1] = pipes[1];
		return 0;
	}
	case LINUX_flock:
		return result_of(flock((int)a, (int)b));
	case LINUX_utimensat:
	{
		struct timespec times[2];
		const struct guest_timespec *guest_times = GUEST(const struct guest_timespec *, c);

		if (guest_times)
		{
			times[0].tv_sec = guest_times[0].seconds;
			times[0].tv_nsec = guest_times[0].nanoseconds;
			times[1].tv_sec = guest_times[1].seconds;
			times[1].tv_nsec = guest_times[1].nanoseconds;
			/* UTIME_NOW and UTIME_OMIT are Linux's (1 << 30) - 1 and - 2 */
			if (times[0].tv_nsec == 0x3fffffff) times[0].tv_nsec = UTIME_NOW;
			if (times[0].tv_nsec == 0x3ffffffe) times[0].tv_nsec = UTIME_OMIT;
			if (times[1].tv_nsec == 0x3fffffff) times[1].tv_nsec = UTIME_NOW;
			if (times[1].tv_nsec == 0x3ffffffe) times[1].tv_nsec = UTIME_OMIT;
		}
		return result_of(utimensat(directory_descriptor(a), GUEST(const char *, b), guest_times ? times : NULL,
			(d & LINUX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0));
	}
	case LINUX_umask:
		return umask((mode_t)a);

	case LINUX_clock_gettime:
	case LINUX_clock_getres:
	{
		struct timespec value;
		long result = number == LINUX_clock_gettime ? clock_gettime(darwin_clock(a), &value) :
			clock_getres(darwin_clock(a), &value);

		if (result == 0)
			timespec_out((uint64_t)b, &value);
		return result_of(result);
	}
	case LINUX_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case LINUX_nanosleep:
	{
		struct timespec request;

		if (!timespec_in((uint64_t)a, &request))
			return -14;
		return sleep_for(&request, (uint64_t)b);
	}
	case LINUX_clock_nanosleep:
		return guest_clock_nanosleep(a, (int)b, (uint64_t)c, (uint64_t)d);
	case LINUX_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case LINUX_ppoll:
	{
		struct timespec timeout;
		int milliseconds = -1;

		if (timespec_in((uint64_t)c, &timeout))
			milliseconds = (int)(timeout.tv_sec * 1000 + (timeout.tv_nsec + 999999) / 1000000);
		/* (struct pollfd and the POLL* bits are the same) */
		return result_of(poll(GUEST(struct pollfd *, a), (nfds_t)(uint32_t)b, milliseconds));
	}

	case LINUX_mmap:
		return host_guest_mmap((uint64_t)(uint32_t)a, (uint64_t)(uint32_t)b, (int)c, (int)d, (int)e, f);
	case LINUX_munmap:
		return host_guest_munmap((uint64_t)(uint32_t)a, (uint64_t)(uint32_t)b);
	case LINUX_mprotect:
		return host_guest_mprotect((uint64_t)(uint32_t)a, (uint64_t)(uint32_t)b, (int)c);
	case LINUX_madvise:
		/* (only ever to give memory back early: nothing is lost by not) */
		return 0;
	case LINUX_mremap:
	case LINUX_brk:
		/* musl then falls back to mmap and copying */
		return -12;

	case LINUX_exit:
	case LINUX_exit_group:
		host_exit((int)a);

	case LINUX_set_tid_address:
	case LINUX_gettid:
	{
		uint64_t thread = 0;

		pthread_threadid_np(NULL, &thread);
		return (long long)(uint32_t)thread;
	}
	case LINUX_getpid:
		return getpid();
	case LINUX_getppid:
		return getppid();
	case LINUX_getuid:
		return getuid();
	case LINUX_geteuid:
		return geteuid();
	case LINUX_getgid:
		return getgid();
	case LINUX_getegid:
		return getegid();
	case LINUX_sched_yield:
		sched_yield();
		return 0;
	case LINUX_sched_getaffinity:
	{
		/* every core, a bit each (sysconf's processor count) */
		uint8_t *mask = GUEST(uint8_t *, c);
		int cores = 0, index;
		size_t length = sizeof(cores);

		sysctlbyname("hw.activecpu", &cores, &length, NULL, 0);
		if (cores < 1)
			cores = 1;
		if (!mask || (uint32_t)b < 8)
			return -22;
		memset(mask, 0, (uint32_t)b);
		for (index = 0; index < cores && index < (int)(uint32_t)b * 8; index++)
			mask[index / 8] |= (uint8_t)(1 << (index % 8));
		return 8;
	}
	case LINUX_getrandom:
		arc4random_buf(GUEST(void *, a), (size_t)(uint32_t)b);
		return (uint32_t)b;
	case LINUX_kill:
	case LINUX_tkill:
	case LINUX_tgkill:
	{
		long long signal_number = number == LINUX_tgkill ? c : b;

		/* raise() and abort(): SIGABRT is 6 on both */
		if (signal_number == 6)
			host_fatal("the game aborted");
		if (signal_number == 0)
			return 0;
		return -1;
	}
	case LINUX_rt_sigprocmask:
	case LINUX_rt_sigaction:
	case LINUX_sigaltstack:
	case LINUX_membarrier:
		/* the host owns signal handling */
		return 0;
	case LINUX_uname:
	{
		struct linux_utsname *result = GUEST(struct linux_utsname *, a);
		struct utsname name;

		if (!result || uname(&name) != 0)
			return -14;
		memset(result, 0, sizeof(*result));
		snprintf(result->sysname, sizeof(result->sysname), "%s", name.sysname);
		snprintf(result->nodename, sizeof(result->nodename), "%s", name.nodename);
		snprintf(result->release, sizeof(result->release), "%s", name.release);
		snprintf(result->version, sizeof(result->version), "%s", name.version);
		snprintf(result->machine, sizeof(result->machine), "%s", name.machine);
		return 0;
	}
	case LINUX_prlimit64:
	case LINUX_getrlimit:
	{
		struct linux_rlimit *result = GUEST(struct linux_rlimit *, number == LINUX_prlimit64 ? d : b);
		int resource = (int)(number == LINUX_prlimit64 ? b : a);

		if (number == LINUX_prlimit64 && c)
			return -1;
		if (result)
		{
			/* (the stack's the thread's own; descriptors the host's) */
			struct rlimit limit;

			result->current = result->maximum = ~0ULL;
			if (resource == 7 && getrlimit(RLIMIT_NOFILE, &limit) == 0)
			{
				result->current = limit.rlim_cur;
				result->maximum = limit.rlim_max;
			}
			else if (resource == 3)
			{
				result->current = result->maximum = 16 * 1024 * 1024;
			}
		}
		return 0;
	}
	case LINUX_ioctl:
		return -25;
	case LINUX_statx:
	case LINUX_statfs:
	case LINUX_fstatfs:
	case LINUX_pselect6:
	case LINUX_sysinfo:
		return -38;

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -38;
	}
}
