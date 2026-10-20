/*
HOST_THREAD.C

Threads that run guest code (as port/android/host/host_thread.c).

Guest (ILP32) code keeps stack addresses in 32-bit registers, so every
thread that runs it needs its stack in guest memory. The guest's own
threads, and the host's that run guest callbacks (SDL's audio callback,
host_sdl.c), are created here with stacks given to pthread_create. The
process's main thread, which must stay the one Cocoa's event loop runs on,
moves onto a stack in guest memory to run the game (host_run_guest_main).
The guest's thread pointer (its musl struct pthread) is kept per thread in
host TLS.

Thread stacks are freed by a reaper thread once the thread has fully exited.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUARD_SIZE HOST_PAGE
#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static __thread uint32_t guest_tp;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

/* ---------- stacks */

/* a stack of size bytes in guest memory, with a guard page below it; its
bottom (a host address), and the mapping it is part of */
static uint8_t *stack_allocate(size_t size, uint32_t *mapping, size_t *mapping_size)
{
	size_t total = size + GUARD_SIZE;
	uint32_t base = host_guest_map(total, PROT_READ | PROT_WRITE);

	if (!base)
		return NULL;
	mprotect(GUEST_TO_HOST(void *, base), GUARD_SIZE, PROT_NONE);
	*mapping = base;
	*mapping_size = total;
	return GUEST_TO_HOST(uint8_t *, base) + GUARD_SIZE;
}

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);

	return sp - (uint64_t)(uintptr_t)host_window < 0x100000000ULL;
}

/* ---------- calling into the guest */

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		host_enter_guest(host_window, host_image.header->thread_attach, 0, 0, 0, 0);
	return host_enter_guest(host_window, function, a, b, c, d);
}

static void *guest_main(void *boot)
{
	host_enter_guest(host_window, host_image.header->start, (uint32_t)(uintptr_t)boot, 0, 0, 0);
	host_fatal("the guest returned from __guest_start");
}

void host_run_guest_main(uint32_t boot)
{
	uint32_t mapping;
	size_t mapping_size;
	uint8_t *stack = stack_allocate(MAIN_STACK_SIZE, &mapping, &mapping_size);

	if (!stack)
		host_fatal("cannot allocate the game's stack");
	host_call_on_stack(guest_main, (void *)(uintptr_t)boot, stack + MAIN_STACK_SIZE);
	host_fatal("the guest returned from __guest_start");
}

/* ---------- guest threads */

struct thread_start
{
	void *(*function)(void *);
	void *argument;
	uint32_t mapping;
	size_t mapping_size;
};

struct finished_thread
{
	struct finished_thread *next;
	pthread_t thread;
	uint32_t mapping;
	size_t mapping_size;
};

static pthread_mutex_t reaper_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reaper_condition = PTHREAD_COND_INITIALIZER;
static struct finished_thread *finished_threads;
static int reaper_started;

static void *reaper(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct finished_thread *finished;

		pthread_mutex_lock(&reaper_lock);
		while (!finished_threads)
			pthread_cond_wait(&reaper_condition, &reaper_lock);
		finished = finished_threads;
		finished_threads = finished->next;
		pthread_mutex_unlock(&reaper_lock);
		pthread_join(finished->thread, NULL);
		host_guest_unmap(finished->mapping, finished->mapping_size);
		free(finished);
	}
	return NULL;
}

static void *thread_main(void *context)
{
	struct thread_start start = *(struct thread_start *)context;
	struct finished_thread *finished;

	free(context);
	start.function(start.argument);
	/* (a thread the guest made has let its struct pthread go by now; one
	of the host's that ran guest code, the audio thread, has it still) */
	if (guest_tp)
		host_enter_guest(host_window, host_image.header->thread_detach, 0, 0, 0, 0);
	guest_tp = 0;

	finished = calloc(1, sizeof(*finished));
	finished->thread = pthread_self();
	finished->mapping = start.mapping;
	finished->mapping_size = start.mapping_size;
	pthread_mutex_lock(&reaper_lock);
	finished->next = finished_threads;
	finished_threads = finished;
	pthread_cond_signal(&reaper_condition);
	pthread_mutex_unlock(&reaper_lock);
	return NULL;
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	pthread_attr_t attributes;
	pthread_t thread;
	uint8_t *stack;
	int error;

	if (!start)
		return ENOMEM;
	pthread_mutex_lock(&reaper_lock);
	if (!reaper_started)
	{
		pthread_t reaper_thread;

		if (pthread_create(&reaper_thread, NULL, reaper, NULL) == 0)
		{
			pthread_detach(reaper_thread);
			reaper_started = 1;
		}
	}
	pthread_mutex_unlock(&reaper_lock);

	/* (pthread_attr_setstack wants whole pages) */
	stack_size = (stack_size + 0xffff) & ~(size_t)0xffff;
	stack = stack_allocate(stack_size, &start->mapping, &start->mapping_size);
	if (!stack)
	{
		free(start);
		return EAGAIN;
	}
	start->function = function;
	start->argument = argument;
	pthread_attr_init(&attributes);
	pthread_attr_setstack(&attributes, stack, stack_size);
	error = pthread_create(&thread, &attributes, thread_main, start);
	pthread_attr_destroy(&attributes);
	if (error)
	{
		host_guest_unmap(start->mapping, start->mapping_size);
		free(start);
	}
	return error;
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size);
}
