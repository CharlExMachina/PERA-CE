/*
HOST_WATCH.C

Write tracking of the guest's Xbox memory (the interface of
port/linux/src/memory_watch.c, which port/android/guest/runtime/
guest_memory_watch.c forwards here), and the reports of crashes.

As on Android (port/android/host/host_memory.c), the renderer write-protects
the pages behind what it caches (textures, vertices) and the first write to
each is caught: the fault's handler records a new generation for the page,
makes it writable again and lets the write proceed. The pages are the
host's, 16 KB: a range the renderer protects or asks about covers the whole
pages it touches, so a write next to it makes it stale too, which costs an
upload but is never wrong. macOS reports a write to a read-only page as
SIGBUS, an unmapped one as SIGSEGV; both are handled.

Any other fault is reported in the log, with the guest's registers and
frame records (host_debug.c), and passed on to the default handler.
*/

#include "host.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / HOST_PAGE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / HOST_PAGE;
}

static void *page_address(uint64_t page)
{
	return host_window + HALO_GUEST_WINDOW_BASE + page * HOST_PAGE;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	mprotect(page_address(page), HOST_PAGE, PROT_READ | PROT_WRITE);
}

/* ---------- faults */

static struct sigaction previous_segv, previous_bus, previous_ill;

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	const struct __darwin_arm_thread_state64 *state = &ucontext->uc_mcontext->__ss;
	uint64_t registers[31];
	uint64_t address = (uint64_t)(uintptr_t)information->si_addr;
	int index;

	for (index = 0; index < 29; index++)
		registers[index] = state->__x[index];
	registers[29] = state->__fp;
	registers[30] = state->__lr;
	if (address - (uint64_t)(uintptr_t)host_window < 0x100000000ULL)
		host_logf(HOST_LOG_ERROR, "signal %d at guest address %08llx", signal_number,
			(unsigned long long)(address - (uint64_t)(uintptr_t)host_window));
	else
		host_logf(HOST_LOG_ERROR, "signal %d at address %016llx", signal_number, (unsigned long long)address);
	host_debug_report_registers("crash", registers, state->__pc, state->__sp);
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the previous
	(or default) handler */
}

/* a write to a watched page: TRUE once it may go ahead */
static int watched_fault(siginfo_t *information)
{
	uint64_t address = (uint64_t)(uintptr_t)information->si_addr - (uint64_t)(uintptr_t)host_window;

	if (watch_active && in_window(address))
	{
		uint64_t page = watch_page(address);

		if (page_protected[page])
		{
			mark_written(page);
			return 1;
		}
	}
	return 0;
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	if (watched_fault(information))
		return;
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
}

static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
	if (watched_fault(information))
		return;
	report_crash(signal_number, information, context);
	chain(&previous_bus, signal_number, information, context);
}

static void ill_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(&previous_ill, signal_number, information, context);
}

void host_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
	/* a write to a closed connection fails rather than ending the game */
	signal(SIGPIPE, SIG_IGN);
}

/* ---------- the memory watch */

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

/* the watch pages a guest range covers, clipped to the Xbox memory; FALSE
if it covers none */
static int watch_pages(uint32_t address, uint32_t size, uint64_t *first, uint64_t *last)
{
	uint64_t start = address, end = (uint64_t)address + size;

	if (!size || end <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return 0;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	if (end > (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		end = (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE;
	*first = watch_page(start);
	*last = watch_page(end - 1);
	return 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !watch_pages(address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect(page_address(page), HOST_PAGE, PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!watch_pages(address, size, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

/* memory_watch_prepare_write: unprotects the pages the host itself is about
to write into (a write by the kernel, read() into a buffer, would fail with
EFAULT rather than fault) */
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !watch_pages(address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

/* memory_watch_forget: the pages of a range were remapped or reprotected, so
treat them as written and unwatched */
void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_pages(address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
		{
			page_protected[page] = 0;
			mprotect(page_address(page), HOST_PAGE, PROT_READ | PROT_WRITE);
		}
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

void host_memory_watch_begin_frame(void)
{
}
