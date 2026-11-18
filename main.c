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
// exactly; only the placement is made deterministic. Both probes reached the
// landing stub on the tested Intel host; other processors were not tested.
// The probe RECORDS the observed destination
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
#include <wchar.h>

#define REGION_SIZE        0x11000u
#define BRANCH_DISPLACEMENT 0x10000u
#define STUB_SIZE          12u
#define PROBE_TIMEOUT_MS   15000u

#define EXIT_LANDING 10
#define EXIT_FAULT   11
#define EXIT_VERIFY  12
#define EXIT_SETUP   13

static BYTE* gRegion = NULL;
static UINT_PTR gTargetOffset = 0;
static const char* gProbeName = "probe";

static void RecordLanding(const char* probeName)
{
	printf("[+] %s: landing stub reached at region+0x%llX - branch displacement honored\n",
		probeName,
		(unsigned long long)gTargetOffset);
	fflush(NULL);
	ExitProcess(EXIT_LANDING);
}

// Target of the machine-generated landing stub (mov rax, <abs>; jmp rax).
static void RecordLandingThunk(void)
{
	RecordLanding(gProbeName);
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
	if (gRegion == NULL || address < (UINT_PTR)gRegion || address >= (UINT_PTR)gRegion + REGION_SIZE)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	UINT_PTR offset = address - (UINT_PTR)gRegion;
	printf("[-] Fault inside experiment region: code 0x%08lX at region+0x%llX\n",
		(unsigned long)code,
		(unsigned long long)offset);
	if (offset < gTargetOffset)
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

static BOOL BuildExperiment(BOOL prefixed, const char* probeName)
{
	gRegion = (BYTE*)VirtualAlloc(NULL, REGION_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (gRegion == NULL)
	{
		fprintf(stderr, "[-] %s: VirtualAlloc: E%lu\n", probeName, GetLastError());
		return FALSE;
	}
	memset(gRegion, 0xFF, REGION_SIZE);

	DWORD branchLength = prefixed ? 6u : 5u;
	DWORD displacement = BRANCH_DISPLACEMENT;
	if (prefixed)
	{
		gRegion[0] = 0x66;
		gRegion[1] = 0xE9;
		memcpy(gRegion + 2, &displacement, sizeof(displacement));
	}
	else
	{
		gRegion[0] = 0xE9;
		memcpy(gRegion + 1, &displacement, sizeof(displacement));
	}

	gTargetOffset = (UINT_PTR)branchLength + BRANCH_DISPLACEMENT;
	if (gTargetOffset + STUB_SIZE > REGION_SIZE)
	{
		fprintf(stderr, "[-] %s: layout does not fit the region\n", probeName);
		return FALSE;
	}

	// Landing stub: mov rax, &RecordLandingThunk; jmp rax. The thunk is a
	// compiled static function that calls RecordLanding with the selected
	// probe name.
	BYTE* stub = gRegion + gTargetOffset;
	stub[0] = 0x48;
	stub[1] = 0xB8;
	UINT_PTR landing = (UINT_PTR)&RecordLandingThunk;
	memcpy(stub + 2, &landing, sizeof(landing));
	stub[10] = 0xFF;
	stub[11] = 0xE0;

	if (!FlushInstructionCache(GetCurrentProcess(), gRegion, REGION_SIZE))
	{
		fprintf(stderr, "[-] %s: FlushInstructionCache: E%lu\n", probeName, GetLastError());
		return FALSE;
	}
	return TRUE;
}

static BOOL VerifyEmittedBytes(BOOL prefixed, const char* probeName)
{
	DWORD branchLength = prefixed ? 6u : 5u;
	DWORD expectedOffset = prefixed ? 2u : 1u;
	DWORD displacement = 0;

	printf("[+] %s: emitted bytes at region+0:\n    ", probeName);
	for (DWORD i = 0; i < 16; i++)
	{
		printf("%02X ", gRegion[i]);
	}
	printf("\n");
	printf("[+] %s: emitted bytes at region+0x%llX (landing stub):\n    ",
		probeName, (unsigned long long)gTargetOffset);
	for (UINT_PTR i = gTargetOffset; i < gTargetOffset + STUB_SIZE; i++)
	{
		printf("%02X ", gRegion[i]);
	}
	printf("\n");

	BOOL ok = TRUE;
	if (prefixed && gRegion[0] != 0x66)
	{
		ok = FALSE;
	}
	if (gRegion[prefixed ? 1 : 0] != 0xE9)
	{
		ok = FALSE;
	}
	memcpy(&displacement, gRegion + expectedOffset, sizeof(displacement));
	if (displacement != BRANCH_DISPLACEMENT)
	{
		ok = FALSE;
	}
	if (gRegion[gTargetOffset] != 0x48 || gRegion[gTargetOffset + 1] != 0xB8 ||
		gRegion[gTargetOffset + 10] != 0xFF || gRegion[gTargetOffset + 11] != 0xE0)
	{
		ok = FALSE;
	}
	for (UINT_PTR i = branchLength; i < gTargetOffset; i++)
	{
		if (gRegion[i] != 0xFF)
		{
			ok = FALSE;
			break;
		}
	}

	if (!ok)
	{
		fprintf(stderr, "[-] %s: emitted-byte verification FAILED\n", probeName);
		return FALSE;
	}

	printf("[+] %s: verification passed - branch %s rel32=0x%X from region+0, "
		"filler 0xFF up to region+0x%llX, landing stub at region+0x%llX (branch end + 0x%X)\n",
		probeName,
		prefixed ? "66 E9" : "E9",
		displacement,
		(unsigned long long)gTargetOffset,
		(unsigned long long)gTargetOffset,
		BRANCH_DISPLACEMENT);
	return TRUE;
}

static int RunProbe(BOOL prefixed)
{
	gProbeName = prefixed ? "prefixed probe" : "baseline probe";
	AddVectoredExceptionHandler(1, RegionFaultHandler);

	if (!BuildExperiment(prefixed, gProbeName))
	{
		return EXIT_SETUP;
	}
	if (!VerifyEmittedBytes(prefixed, gProbeName))
	{
		return EXIT_VERIFY;
	}

	printf("[+] %s: executing branch at %p (expected target %p)\n",
		gProbeName,
		(LPCVOID)gRegion,
		(LPCVOID)(gRegion + gTargetOffset));
	fflush(NULL);

	typedef void (*BRANCH_ENTRY)(void);
	BRANCH_ENTRY entry = (BRANCH_ENTRY)gRegion;
	entry();

	// Neither the landing stub nor the fault handler returned; control flow
	// left the experiment in an unplanned way.
	fprintf(stderr, "[-] %s: branch call returned - unrecorded control flow\n", gProbeName);
	return EXIT_SETUP;
}

typedef struct
{
	const char* name;
	BOOL prefixed;
	DWORD exitCode;
	BOOL timedOut;
} PROBE_RESULT;

static BOOL RunChildProbe(const PROBE_RESULT* template, WCHAR* selfPath, PROBE_RESULT* result)
{
	WCHAR commandLine[512];
	_snwprintf_s(commandLine, _countof(commandLine), _TRUNCATE,
		L"\"%ls\" --probe %hs", selfPath, template->name);

	SECURITY_ATTRIBUTES pipeAttributes = { sizeof(pipeAttributes), NULL, TRUE };
	HANDLE readPipe = NULL;
	HANDLE writePipe = NULL;
	if (!CreatePipe(&readPipe, &writePipe, &pipeAttributes, 0))
	{
		fprintf(stderr, "[-] CreatePipe: E%lu\n", GetLastError());
		return FALSE;
	}
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW startup = { sizeof(startup) };
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdOutput = writePipe;
	startup.hStdError = writePipe;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION process = { 0 };
	BOOL created = CreateProcessW(selfPath, commandLine, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
	CloseHandle(writePipe);
	if (!created)
	{
		fprintf(stderr, "[-] CreateProcessW(%ls): E%lu\n", commandLine, GetLastError());
		CloseHandle(readPipe);
		return FALSE;
	}

	DWORD wait = WaitForSingleObject(process.hProcess, PROBE_TIMEOUT_MS);
	result->timedOut = wait == WAIT_TIMEOUT;
	if (result->timedOut)
	{
		TerminateProcess(process.hProcess, 1);
		WaitForSingleObject(process.hProcess, 5000);
		result->exitCode = 1;
	}
	else
	{
		GetExitCodeProcess(process.hProcess, &result->exitCode);
	}

	char buffer[1024];
	DWORD bytesRead = 0;
	printf("--- %hs child output ---\n", template->name);
	while (ReadFile(readPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0';
		fputs(buffer, stdout);
	}
	printf("--- end of %hs child output ---\n", template->name);

	CloseHandle(readPipe);
	CloseHandle(process.hProcess);
	CloseHandle(process.hThread);
	return TRUE;
}

static void ClassifyResult(const PROBE_RESULT* result)
{
	if (result->timedOut)
	{
		printf("[=] %hs: TIMEOUT - child exceeded %lu ms and was terminated (unbounded hang recorded)\n",
			result->name, PROBE_TIMEOUT_MS);
		return;
	}
	switch (result->exitCode)
	{
		case EXIT_LANDING:
			printf("[=] %hs: OBSERVED - branch reached the landing stub (displacement honored)\n", result->name);
			break;
		case EXIT_FAULT:
			printf("[=] %hs: OBSERVED - fault inside the experiment region (offset recorded above)\n", result->name);
			break;
		case EXIT_VERIFY:
			printf("[=] %hs: INVALID - emitted-byte verification failed; no branch was executed\n", result->name);
			break;
		case EXIT_SETUP:
			printf("[=] %hs: INVALID - probe setup failed; no branch was executed\n", result->name);
			break;
		default:
			printf("[=] %hs: UNEXPECTED - child exit code 0x%08lX (crash outside the recorded region or other failure)\n",
				result->name, (unsigned long)result->exitCode);
			break;
	}
}

int wmain(int argc, wchar_t* argv[])
{
	if (argc == 3 && wcscmp(argv[1], L"--probe") == 0)
	{
		if (wcscmp(argv[2], L"prefixed") == 0)
		{
			return RunProbe(TRUE);
		}
		if (wcscmp(argv[2], L"baseline") == 0)
		{
			return RunProbe(FALSE);
		}
		fprintf(stderr, "Usage: %ls --probe prefixed|baseline\n", argv[0]);
		return EXIT_SETUP;
	}
	if (argc != 1)
	{
		fprintf(stderr, "Usage: %ls            (run both probes in isolated children)\n", argv[0]);
		fprintf(stderr, "       %ls --probe prefixed|baseline   (child mode)\n", argv[0]);
		return EXIT_SETUP;
	}

	WCHAR selfPath[MAX_PATH] = { 0 };
	if (GetModuleFileNameW(NULL, selfPath, MAX_PATH) == 0)
	{
		fprintf(stderr, "[-] GetModuleFileNameW: E%lu\n", GetLastError());
		return EXIT_SETUP;
	}

	printf("OperandPrefixProbe - operand-size-prefixed relative-branch experiment\n");
	printf("Host: Windows x64. Results apply only to this CPU/environment.\n\n");

	PROBE_RESULT probes[2] = { 0 };
	probes[0].name = "baseline";
	probes[0].prefixed = FALSE;
	probes[1].name = "prefixed";
	probes[1].prefixed = TRUE;

	for (int i = 0; i < 2; i++)
	{
		if (!RunChildProbe(&probes[i], selfPath, &probes[i]))
		{
			return EXIT_SETUP;
		}
		ClassifyResult(&probes[i]);
		printf("\n");
	}

	printf("Summary: baseline exit 0x%02lX%s, prefixed exit 0x%02lX%s.\n",
		(unsigned long)probes[0].exitCode, probes[0].timedOut ? " (timeout)" : "",
		(unsigned long)probes[1].exitCode, probes[1].timedOut ? " (timeout)" : "");
	printf("This is an observation record only. It does not identify a VM, a hypervisor\n");
	printf("or a CPU vendor; any such claim would need separate cross-vendor evidence.\n");
	return EXIT_SUCCESS;
}
