/*
HOST_DEBUG.C

Reports of crashes (host_watch.c): the registers, and the frame records of
the guest's code, with their addresses as the guest sees them, which
llvm-symbolizer --obj=build/macos/halo_guest.elf finds the functions of
(port/macos/README.md, "Finding problems").
*/

#include "host.h"

/* a host address in the window, as the guest's; 0 if it is not one */
static uint64_t guest_address(uint64_t address)
{
	uint64_t offset = address - (uint64_t)(uintptr_t)host_window;

	return offset < 0x100000000ULL ? offset : 0;
}

static int in_image(uint64_t address)
{
	uint64_t guest = guest_address(address);

	return guest >= host_image.base && guest < host_image.end;
}

void host_debug_report_registers(const char *what, const uint64_t *x, uint64_t pc, uint64_t sp)
{
	uint64_t fp = x[29];
	int index;

	host_logf(HOST_LOG_ERROR, "%s: pc %016llx lr %016llx sp %016llx", what, (unsigned long long)pc,
		(unsigned long long)x[30], (unsigned long long)sp);
	if (in_image(pc) || in_image(x[30]))
	{
		host_logf(HOST_LOG_ERROR, "  in the guest image: llvm-symbolizer --obj=build/macos/halo_guest.elf 0x%llx 0x%llx",
			(unsigned long long)guest_address(pc), (unsigned long long)guest_address(x[30]));
	}
	for (index = 0; index < 31; index += 4)
	{
		host_logf(HOST_LOG_ERROR, "  x%-2d %016llx %016llx %016llx %016llx", index,
			(unsigned long long)x[index],
			(unsigned long long)(index + 1 < 31 ? x[index + 1] : 0),
			(unsigned long long)(index + 2 < 31 ? x[index + 2] : 0),
			(unsigned long long)(index + 3 < 31 ? x[index + 3] : 0));
	}
	/* the frame records, fp and lr (host addresses in the window), while
	they are guest memory */
	for (index = 0; index < 32 && fp && (fp & 7) == 0; index++)
	{
		const uint64_t *frame;
		uint64_t guest = guest_address(fp);

		if (!guest || !host_guest_mapped((uint32_t)guest, 16))
			break;
		frame = (const uint64_t *)(uintptr_t)fp;
		host_logf(HOST_LOG_ERROR, "  frame %2d: return %08llx%s", index,
			(unsigned long long)(in_image(frame[1]) ? guest_address(frame[1]) : frame[1]),
			in_image(frame[1]) ? "" : " (host)");
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
}
