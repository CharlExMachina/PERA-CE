/*
HOST_MEMORY.C

The guest's address space for the macOS port: a window of 4 GB of the
host's, at a 4 GB boundary (host.h), with 4 GB on each side of it that is
never mapped, so that a guest address a little out of bounds faults rather
than reaching the host's own memory.

In the window, by guest address:

- below 0x00100000: never mapped (null pointers);
- 0x80000000 to 0x88000000: the Xbox contiguous memory
  (port/linux/src/xbox_memory.c), mapped readable and writable at start-up;
- the image's range after it (host_loader.c);
- 0x40000000 to 0x80000000: left for mappings the guest asks for at fixed
  addresses (the Custom Edition tag cache);
- the rest: pages handed out for the guest's other mappings (malloc,
  stacks), at 16 KB, Apple silicon's page size.

The guest's code was written for 4 KB pages. xbox_memory.c maps, unmaps and
protects the Xbox memory a block (4 KB pages) at a time, which here would
take the neighbouring blocks in the same 16 KB page with them: inside the
Xbox memory those calls only clear what they map, and protection is left
alone (the memory watch, host_watch.c, protects pages there itself).
Elsewhere a request that covers only part of a 16 KB page changes nothing
of that page but what it can without touching the rest.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *host_window;

#define GIGABYTES(count) ((uint64_t)(count) << 30)
#define PAGE_COUNT (GIGABYTES(4) / HOST_PAGE)

/* Linux's mmap flags, as the guest passes them */
#define LINUX_MAP_SHARED 0x01
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

enum
{
	_page_free,
	_page_used,
	/* never handed out: the null guard, the Xbox memory, the image */
	_page_reserved,
	/* only for requests at a fixed address */
	_page_fixed_only,
};

static uint8_t page_state[PAGE_COUNT];
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
/* where the next search for free pages starts */
static uint64_t search_hint;

#define NULL_GUARD 0x00100000ULL
#define FIXED_ONLY_START 0x40000000ULL
#define FIXED_ONLY_END 0x80000000ULL
#define TOP_GUARD 0xf0000000ULL

static uint64_t round_up(uint64_t value)
{
	return (value + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
}

static uint64_t round_down(uint64_t value)
{
	return value & ~(HOST_PAGE - 1);
}

static void mark(uint64_t address, uint64_t size, int state)
{
	uint64_t page;

	for (page = address / HOST_PAGE; page < (address + size) / HOST_PAGE && page < PAGE_COUNT; page++)
		page_state[page] = (uint8_t)state;
}

static int in_xbox_memory(uint64_t address, uint64_t size)
{
	return address >= HALO_GUEST_WINDOW_BASE && address + size <= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE;
}

int host_memory_initialize(uint32_t image_base, uint32_t image_size)
{
	/* 16 GB of address space, of which the 4 GB aligned window in the middle
	is the guest's */
	uint64_t size = GIGABYTES(16);
	uint8_t *reservation = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	uint64_t window;

	if (reservation == MAP_FAILED)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the guest's address space: %s", strerror(errno));
		return -1;
	}
	window = ((uint64_t)(uintptr_t)reservation + GIGABYTES(4) + GIGABYTES(4) - 1) & ~(GIGABYTES(4) - 1);
	host_window = (uint8_t *)(uintptr_t)window;
	memset(page_state, _page_free, sizeof(page_state));
	mark(0, NULL_GUARD, _page_reserved);
	mark(FIXED_ONLY_START, FIXED_ONLY_END - FIXED_ONLY_START, _page_fixed_only);
	mark(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, _page_reserved);
	mark(image_base, round_up(image_size), _page_reserved);
	mark(TOP_GUARD, GIGABYTES(4) - TOP_GUARD, _page_reserved);
	search_hint = (uint64_t)image_base + round_up(image_size);
	if (mmap(host_window + HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED ||
		mmap(host_window + image_base, round_up(image_size), PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
	{
		host_logf(HOST_LOG_ERROR, "cannot map the Xbox memory and the image: %s", strerror(errno));
		return -1;
	}
	host_logf(HOST_LOG_INFO, "guest window at %p", (void *)host_window);
	return 0;
}

/* ---------- pages */

/* the first run of count free pages from the hint on (wrapping round); its
first page, or 0 */
static uint64_t find_free(uint64_t count)
{
	uint64_t start = search_hint / HOST_PAGE, page, run = 0, pass;

	for (pass = 0; pass < 2; pass++)
	{
		uint64_t first = pass ? NULL_GUARD / HOST_PAGE : start;
		uint64_t last = pass ? start + count : PAGE_COUNT;

		if (last > PAGE_COUNT)
			last = PAGE_COUNT;
		run = 0;
		for (page = first; page < last; page++)
		{
			if (page_state[page] != _page_free)
			{
				run = 0;
				continue;
			}
			if (++run == count)
				return page + 1 - count;
		}
	}
	return 0;
}

uint32_t host_guest_map(size_t size, int protection)
{
	uint64_t length = round_up(size), first;
	uint64_t address;

	if (!length || length > GIGABYTES(1))
		return 0;
	pthread_mutex_lock(&memory_lock);
	first = find_free(length / HOST_PAGE);
	if (first)
	{
		address = first * HOST_PAGE;
		mark(address, length, _page_used);
		search_hint = address + length;
	}
	pthread_mutex_unlock(&memory_lock);
	if (!first)
		return 0;
	if (mmap(host_window + address, length, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
	{
		host_guest_unmap((uint32_t)address, length);
		return 0;
	}
	return (uint32_t)address;
}

/* gives the memory of whole pages back, keeping their address space */
static void release(uint64_t address, uint64_t length)
{
	mmap(host_window + address, length, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
}

void host_guest_unmap(uint32_t address, size_t size)
{
	uint64_t start = round_down(address), end = round_up((uint64_t)address + size), page;

	pthread_mutex_lock(&memory_lock);
	for (page = start / HOST_PAGE; page < end / HOST_PAGE && page < PAGE_COUNT; page++)
	{
		if (page_state[page] == _page_used)
		{
			release(page * HOST_PAGE, HOST_PAGE);
			page_state[page] = page * HOST_PAGE >= FIXED_ONLY_START && page * HOST_PAGE < FIXED_ONLY_END ?
				_page_fixed_only : _page_free;
		}
	}
	pthread_mutex_unlock(&memory_lock);
}

int host_guest_mapped(uint32_t address, uint32_t size)
{
	uint64_t page, end = (uint64_t)address + size;
	int mapped = 1;

	if (in_xbox_memory(address, size) || (address >= host_image.base && end <= host_image.end))
		return 1;
	pthread_mutex_lock(&memory_lock);
	for (page = address / HOST_PAGE; page < round_up(end) / HOST_PAGE && mapped; page++)
		mapped = page < PAGE_COUNT && page_state[page] == _page_used;
	pthread_mutex_unlock(&memory_lock);
	return mapped;
}

/* ---------- the guest's memory system calls */

static int darwin_map_flags(int flags)
{
	return (flags & LINUX_MAP_SHARED ? MAP_SHARED : MAP_PRIVATE) | (flags & LINUX_MAP_ANONYMOUS ? MAP_ANON : 0);
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	uint64_t start, end, page;
	int anonymous = (flags & LINUX_MAP_ANONYMOUS) != 0;

	if (!size || size > GIGABYTES(2))
		return -EINVAL;
	if (!(flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)))
	{
		uint32_t result = host_guest_map(length, PROT_NONE);

		if (!result)
			return -ENOMEM;
		if (mmap(host_window + result, length, protection, darwin_map_flags(flags) | MAP_FIXED,
			anonymous ? -1 : fd, anonymous ? 0 : offset) == MAP_FAILED)
		{
			int error = errno;

			host_guest_unmap(result, length);
			return -host_linux_errno(error);
		}
		return result;
	}

	if (address + size > GIGABYTES(4) || address < NULL_GUARD)
		return -ENOMEM;
	/* the Xbox memory is always mapped: a block mapped there is cleared,
	as fresh pages would be */
	if (in_xbox_memory(address, size))
	{
		if (anonymous && protection != PROT_NONE)
			memset(host_window + address, 0, size);
		return (long)address;
	}
	if (!anonymous)
	{
		/* (a file at a fixed address: whole pages only) */
		if ((address | offset) & (HOST_PAGE - 1))
			return -EINVAL;
	}
	start = round_down(address);
	end = round_up(address + size);
	pthread_mutex_lock(&memory_lock);
	for (page = start / HOST_PAGE; page < end / HOST_PAGE; page++)
	{
		int state = page_state[page];

		if (state == _page_reserved || (state == _page_used && (flags & LINUX_MAP_FIXED_NOREPLACE)))
		{
			pthread_mutex_unlock(&memory_lock);
			return state == _page_reserved ? -ENOMEM : -EEXIST;
		}
	}
	for (page = start / HOST_PAGE; page < end / HOST_PAGE; page++)
	{
		uint64_t page_address = page * HOST_PAGE;
		uint64_t from = page_address > address ? page_address : address;
		uint64_t to = page_address + HOST_PAGE < address + size ? page_address + HOST_PAGE : address + size;

		if (page_state[page] == _page_used && (from != page_address || to != page_address + HOST_PAGE))
		{
			/* part of a page in use: what is asked for is cleared, and made
			at least as accessible */
			if (anonymous)
			{
				mprotect(host_window + page_address, HOST_PAGE, PROT_READ | PROT_WRITE | protection);
				memset(host_window + from, 0, to - from);
			}
			continue;
		}
		if (mmap(host_window + page_address, HOST_PAGE, anonymous ? protection | (from != page_address ||
			to != page_address + HOST_PAGE ? PROT_READ | PROT_WRITE : 0) : protection,
			darwin_map_flags(flags) | MAP_FIXED, anonymous ? -1 : fd,
			anonymous ? 0 : offset + (int64_t)(page_address - address)) == MAP_FAILED)
		{
			int error = errno;

			pthread_mutex_unlock(&memory_lock);
			return -host_linux_errno(error);
		}
		page_state[page] = _page_used;
	}
	pthread_mutex_unlock(&memory_lock);
	(void)length;
	return (long)address;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t start, end;

	if (!size || address + size > GIGABYTES(4))
		return -EINVAL;
	if (in_xbox_memory(address, size))
		return 0;
	if (address < host_image.end && address + size > host_image.base)
		return -EINVAL;
	/* only the pages it covers whole */
	start = round_up(address);
	end = round_down(address + size);
	if (end > start)
		host_guest_unmap((uint32_t)start, end - start);
	return 0;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t start, end;

	if (!size)
		return 0;
	if (address + size > GIGABYTES(4) || address < NULL_GUARD)
		return -ENOMEM;
	if (in_xbox_memory(address, size) || (address < host_image.end && address + size > host_image.base))
		return 0;
	/* whole pages take the protection; a page covered in part only gains
	access, never loses it */
	start = round_up(address);
	end = round_down(address + size);
	if (end > start && mprotect(host_window + start, end - start, protection) != 0)
		return -host_linux_errno(errno);
	if ((protection & (PROT_READ | PROT_WRITE)) == (PROT_READ | PROT_WRITE))
	{
		if (round_down(address) < start)
			mprotect(host_window + round_down(address), HOST_PAGE, protection);
		if (end < round_up(address + size) && end >= start)
			mprotect(host_window + end, HOST_PAGE, protection);
	}
	return 0;
}
