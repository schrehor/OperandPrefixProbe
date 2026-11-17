// OperandPrefixProbe - operand-size-prefixed relative-branch experiment.
//
// Faithful rewrite of the OverridePrefix reference (Intel C++ Compiler 2023,
// x86 naked/__asm). MSVC x64 has no inline assembly, so the experiment is
// emitted as DATA into a controlled VirtualAlloc region instead of relying on
// accidental compiler/linker adjacency:
//
//   offset 0          : 66 E9 <rel32>  (prefixed probe)  or  E9 <rel32>  (baseline)
//   branch end..T-1   : 0xFF filler
//   T = branch end + 0x10000 : landing stub  48 B8 <abs64> FF E0
//                              (mov rax, RecordLanding; jmp rax)
//
// The displacement 0x00010000 and the filler byte 0xFF match the reference
// exactly; only the placement is made deterministic. On documented Intel/AMD
// x86-64 behavior the 66h prefix is ignored for JMP rel32, so both probes
// should reach the landing stub. The probe RECORDS the observed destination
// or fault; it never claims the outcome identifies a VM or a CPU vendor.
//
// Child exit codes:
//   10 landing stub reached
//   11 fault inside the experiment region (offset recorded)
//   12 emitted-byte verification failed
//   13 setup failed
//
// Default (parent) mode runs both probes in isolated child processes with a
// bounded timeout and classifies each observed outcome.

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGION_SIZE        0x11000u
#define BRANCH_DISPLACEMENT 0x10000u
#define STUB_SIZE          12u
#define PROBE_TIMEOUT_MS   15000u

#define EXIT_LANDING 10
#define EXIT_FAULT   11
#define EXIT_VERIFY  12
#define EXIT_SETUP   13

static BYTE* g_region = NULL;
static UINT_PTR g_targetOffset = 0;
static const char* g_probeName = "probe";

static void RecordLanding(const char* probeName)
{
	printf("[+] %s: landing stub reached at region+0x%llX - branch displacement honored\n",
		probeName,
		(unsigned long long)g_targetOffset);
	fflush(NULL);
	ExitProcess(EXIT_LANDING);
}

// Target of the machine-generated landing stub (mov rax, <abs>; jmp rax).
static void RecordLandingThunk(void)
{
	RecordLanding(g_probeName);
}

static LONG CALLBACK RegionFaultHandler(PEXCEPTION_POINTERS exception)
{
	DWORD code = exception->ExceptionRecord->ExceptionCode;
	UINT_PTR address = (UINT_PTR)exception->ExceptionRecord->ExceptionAddress;

	BOOL relevant =
		code == EXCEPTION_ACCESS_VIOLATION ||
		code == EXCEPTION_ILLEGAL_INSTRUCTION ||
		code == EXCEPTION_PRIV_INSTRUCTION;
	if (!relevant)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (g_region == NULL || address < (UINT_PTR)g_region || address >= (UINT_PTR)g_region + REGION_SIZE)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	UINT_PTR offset = address - (UINT_PTR)g_region;
	printf("[-] Fault inside experiment region: code 0x%08lX at region+0x%llX\n",
		(unsigned long)code,
		(unsigned long long)offset);
	if (offset < g_targetOffset)
	{
		printf("[-] Fault landed in the 0xFF filler - the branch did NOT reach the intended target\n");
	}
	else
	{
		printf("[-] Fault landed at or past the intended target offset\n");
	}
	fflush(NULL);
	ExitProcess(EXIT_FAULT);
	return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
	printf("OperandPrefixProbe skeleton\n");
	return 0;
}
