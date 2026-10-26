/*
HOST.H

Internals of the macOS port's host (the arm64 executable of CoreCE.app). See
port/macos/README.md for the overall design and
port/android/include/halo_android_abi.h for the guest contract, which the
two ports share.

The guest runs in a window of 4 GB of this process's address space: guest
address A is host address host_window + A. The guest's own code keeps the
window's base in x28 (tools/macos_asm_rebase.py), and its import stubs turn
the pointers it passes into host addresses (tools/macos_imports.py), so a
host function the guest calls takes ordinary pointers. What the host hands
back to the guest, and any pointer it finds inside guest memory, is a guest
address: GUEST_TO_HOST and HOST_TO_GUEST convert.
*/

#ifndef __HALO_MACOS_HOST_H
#define __HALO_MACOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_android_abi.h"

/* ---------- the window */

/* its base, 4 GB aligned, set by host_memory_initialize */
extern uint8_t *host_window;

#define GUEST_TO_HOST(type, address) ((type)(void *)(host_window + (uint32_t)(address)))
#define HOST_TO_GUEST(pointer) ((uint32_t)((const uint8_t *)(pointer) - host_window))
/* a guest address that may be 0 */
#define GUEST_TO_HOST_OR_NULL(type, address) \
	((uint32_t)(address) ? GUEST_TO_HOST(type, address) : (type)NULL)

/* ---------- logging (standard error, and the log file) */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* guest services also used inside the host */
void host_log(int priority, const char *text);
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- guest memory (host_memory.c)

The window's guest addresses below 0x00100000 are never mapped (null
pointers fault, with any small offset), nor are those of the 4 GB below and
above it. The Xbox contiguous memory (0x80000000, 128 MB) is mapped once,
readable and writable, and the image's range after it; the guest's other
mappings come from pages handed out here. */

/* reserves the window and maps the Xbox memory and the image's range;
returns 0 on success */
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
/* the host's page size, 16 KB on Apple silicon */
#define HOST_PAGE 0x4000ULL
/* page-granular guest memory, at a guest address; 0 on failure */
uint32_t host_guest_map(size_t size, int protection);
void host_guest_unmap(uint32_t address, size_t size);
/* the guest's mmap/munmap/mprotect (host_syscall.c): Linux's arguments and
results */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
/* 1 if [address, address + size) is guest memory that is mapped */
int host_guest_mapped(uint32_t address, uint32_t size);

/* the memory watch's and crashes' signal handlers (host_watch.c) */
void host_install_signal_handlers(void);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory; returns 0 on success */
int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c, host_entry.S) */

/* calls the guest function at address with up to four 32-bit arguments on
this thread, which must be on a guest stack (one made by
host_native_thread_create, or the main thread inside host_run_guest_main),
giving the thread a guest struct pthread first if it has none; returns the
guest's w0 */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) with its stack in guest
memory, so that it can call guest code; the stack is freed after it exits.
Returns 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on the calling thread (the process's main
thread, which Cocoa needs), on a stack of its own in guest memory; does
not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));
/* host_entry.S: guest code at window + function, with the window's base in
x28 (x27 and x28 are saved and restored around it) */
uint32_t host_enter_guest(uint8_t *window, uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* host_entry.S: function(argument) on another stack; returns its result */
void *host_call_on_stack(void *(*function)(void *), void *argument, void *stack_top);
/* the guest's thread pointer (its musl struct pthread) on this thread */
uint32_t host_get_tp(void);

/* ---------- errors (host_syscall.c) */

/* the guest's (Linux's) number for a Darwin errno value */
int host_linux_errno(int darwin);

/* ---------- debugging (host_debug.c) */

/* the guest frames of a crash or a hang, into the log */
void host_debug_report_registers(const char *what, const uint64_t *x, uint64_t pc, uint64_t sp);

/* ---------- import table (generated by tools/macos_imports.py) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- the app (host_main.m) */

/* the folder the game's own files go in, ~/Library/Application Support/CoreCE */
const char *host_support_directory(void);
/* the guest image and the files the app carries (Contents/Resources) */
const char *host_resource_directory(void);

#endif
