/* Preserve exceptions/fatal assertions, guest callers and nearby objects on Windows.
 * DbgHelp runs on a dedicated thread, with a bounded wait on the faulting thread. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include "crash_win.h"

typedef BOOL (WINAPI *WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
static WriteDump write_dump;
static HANDLE requested, completed;
static EXCEPTION_RECORD record;
static CONTEXT context;
static EXCEPTION_POINTERS pointers = {&record, &context};
static DWORD fault_thread, dump_error;
static HANDLE fault_process;
static DWORD fault_pid;
static volatile LONG dump_claimed;
static char fatal_reason[256];
static wchar_t dump_directory[1024];
static char dump_path_utf8[4096];
static uintptr_t guest_base;
static size_t guest_size;

/* Manually mapped guest code has no PE module for DbgHelp to copy. A normal
 * minidump keeps only 256 bytes around the fault PC and omits guest heap objects.
 * Select at most 2 MiB of extra readable pages, without taking runtime locks or
 * allocating on a possibly damaged heap. Nothing here runs during gameplay. */
#define EXTRA_BYTES (2u*1024u*1024u)
#define MAX_PAGES 512u
#define MAX_FRAMES 32u
#define MAX_ROOT_PAGES 128u
static struct { uintptr_t address; unsigned object; } pages[MAX_PAGES];
static unsigned page_count, object_pages, page_cursor, frame_count;
static size_t system_page;
static ULONGLONG capture_deadline;

void crash_win_set_guest(const void *base, size_t size) {
    guest_base = (uintptr_t)base;
    guest_size = size;
}

static int read_memory(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(fault_process, (const void *)address, out, size, &got) && got == size;
}

static void add_page(uintptr_t address, unsigned object) {
    if (address < 65536 || page_count >= MAX_PAGES ||
            (page_count+1)*system_page > EXTRA_BYTES || GetTickCount64() >= capture_deadline) return;
    address -= address % system_page;
    for (unsigned i=0; i<page_count; ++i) if (pages[i].address == address) return;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQueryEx(fault_process, (const void *)address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readable)) return;
    /* Do not follow code or DLL globals as heap roots. Guest heaps are private
     * allocations; executable pages are captured explicitly for PCs/callers. */
    if (object && (info.Type == MEM_IMAGE ||
            (info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))) return;
    pages[page_count].address = address;
    pages[page_count++].object = object;
    object_pages += object != 0;
}

static void add_object(uintptr_t address) {
    if (address < 65536 || address > UINTPTR_MAX-system_page) return;
    /* Keep fields on either side of an interior pointer, including fields
     * crossing a page boundary. Never make an unreadable page readable. */
    add_page(address, 1);
    add_page(address-256, 1);
    add_page(address+512, 1);
}

static void add_code(uintptr_t address) {
    if (fault_process != GetCurrentProcess()) {
        /* No guest registration exists in the observer. Capture readable private
         * executable pages around PCs/callers, under the same 2 MiB budget. */
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQueryEx(fault_process, (const void *)address, &info, sizeof(info)) ||
                info.Type != MEM_PRIVATE || !(info.Protect &
                (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return;
        uintptr_t first = address > 8192 ? address-8192 : 0;
        first -= first % system_page;
        for (unsigned i=0; i<5 && first<=UINTPTR_MAX-system_page; ++i, first+=system_page) add_page(first, 0);
        return;
    }
    if (!guest_base || address < guest_base || address-guest_base >= guest_size) return;
    uintptr_t first = address-guest_base > 8192 ? address-8192 : guest_base;
    uintptr_t last = guest_base+guest_size;
    if (address <= UINTPTR_MAX-8192 && address+8192 < last) last = address+8192;
    first -= first % system_page;
    for (uintptr_t p=first; p<last && p<=UINTPTR_MAX-system_page; p+=system_page) add_page(p, 0);
}

static void select_memory(void) {
    page_count = object_pages = page_cursor = frame_count = 0;
    capture_deadline = GetTickCount64()+1500;
    add_code((uintptr_t)context.Rip);
    uintptr_t frame = (uintptr_t)context.Rbp;
    for (; frame_count<MAX_FRAMES && GetTickCount64()<capture_deadline; ++frame_count) {
        uintptr_t pair[2];
        if (!read_memory(frame, pair, sizeof(pair))) break;
        add_code(pair[1]);
        /* A corrupt chain must not cause an unbounded walk or address wrap. */
        if (pair[0] <= frame || pair[0]-frame > 1024*1024) break;
        frame = pair[0];
    }
    const uintptr_t registers[] = {context.Rax,context.Rbx,context.Rcx,context.Rdx,
        context.Rsi,context.Rdi,context.R8,context.R9,context.R10,context.R11,
        context.R12,context.R13,context.R14,context.R15};
    for (unsigned i=0; i<sizeof(registers)/sizeof(registers[0]); ++i) add_object(registers[i]);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
        add_object((uintptr_t)record.ExceptionInformation[1]);
    /* Stack locals often contain the object from which a now-null field was
     * loaded. Normal minidumps preserve the stack, but not its pointed-to data. */
    uintptr_t words[32];
    uintptr_t stack = (uintptr_t)context.Rsp;
    for (size_t offset=0; offset<16384 && stack<=UINTPTR_MAX-offset &&
            object_pages<MAX_ROOT_PAGES && GetTickCount64()<capture_deadline; offset+=sizeof(words)) {
        if (!read_memory(stack+offset, words, sizeof(words))) break;
        for (unsigned j=0; j<sizeof(words)/sizeof(words[0]); ++j) add_object(words[j]);
    }
    /* One further pointer level. Interior pointers may address an object in
     * the middle of a page, so scan the whole selected root page. */
    unsigned roots = page_count;
    uintptr_t object_words[512];
    size_t bytes = system_page < sizeof(object_words) ? system_page : sizeof(object_words);
    for (unsigned i=0; i<roots && GetTickCount64()<capture_deadline; ++i) {
        if (!pages[i].object || !read_memory(pages[i].address, object_words, bytes)) continue;
        for (size_t j=0; j<bytes/sizeof(object_words[0]); ++j) add_object(object_words[j]);
    }
}

static BOOL CALLBACK dump_callback(void *unused, MINIDUMP_CALLBACK_INPUT *input,
        MINIDUMP_CALLBACK_OUTPUT *output) {
    (void)unused;
    if (input->CallbackType == MemoryCallback) {
        if (page_cursor >= page_count) return FALSE;
        output->MemoryBase = pages[page_cursor++].address;
        output->MemorySize = (ULONG)system_page;
    } else if (input->CallbackType == ReadMemoryFailureCallback) {
        /* Other guest threads may release an optional page during collection.
         * Retain the exception and readable pages when that happens. */
        output->Status = S_OK;
    } else if (input->CallbackType == CancelCallback) {
        output->CheckCancel = FALSE;
        output->Cancel = FALSE;
    }
    return TRUE;
}

static DWORD WINAPI dump_worker(void *unused) {
    (void)unused;
    WaitForSingleObject(requested, INFINITE);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[1200];
    _snwprintf(path, sizeof(path)/sizeof(path[0]),
        L"%ls\\bb-crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", dump_directory,
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        fault_pid);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, dump_path_utf8, sizeof(dump_path_utf8), NULL, NULL);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        dump_error = GetLastError();
    } else {
        MINIDUMP_EXCEPTION_INFORMATION info = {fault_thread, &pointers, FALSE};
        select_memory();
        char description[768];
        int length = snprintf(description, sizeof(description),
            "BBPORT_GUEST_CONTEXT_V1\nimage_base=0x%llx\nimage_size=0x%llx\n"
            "page_size=%zu\nextra_pages=%u\nextra_limit=%u\ncaller_frames=%u\nreason=%s\n",
            (unsigned long long)guest_base, (unsigned long long)guest_size,
            system_page, page_count, EXTRA_BYTES, frame_count, fatal_reason);
        MINIDUMP_USER_STREAM stream = {CommentStreamA, (ULONG)length+1, description};
        MINIDUMP_USER_STREAM_INFORMATION streams = {1, &stream};
        MINIDUMP_CALLBACK_INFORMATION callback = {dump_callback, NULL};
        if (!write_dump(fault_process, fault_pid, file,
                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo,
                &info, &streams, &callback)) {
            /* Optional context must not cost us the original minimal dump. */
            LARGE_INTEGER start = {.QuadPart=0};
            if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) || !SetEndOfFile(file) ||
                    !write_dump(fault_process, fault_pid, file,
                        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules, &info, NULL, NULL)) {
                dump_error = GetLastError();
            }
        }
        if (dump_error) {
            CloseHandle(file);
            DeleteFileW(path); // discard only this newly created, incomplete dump
        } else {
            CloseHandle(file);
        }
    }
    SetEvent(completed);
    return 0;
}

void crash_win_init(void) {
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    system_page = system_info.dwPageSize;
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *pe = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    printf("Runtime build: PE timestamp=%08lx, image_size=0x%lx\n",
        pe->FileHeader.TimeDateStamp, pe->OptionalHeader.SizeOfImage);
    wchar_t setting[8];
    if (GetEnvironmentVariableW(L"BB_CRASH_DUMP", setting, 8) && setting[0] == L'0') {
        puts("Crash reporting: disabled (BB_CRASH_DUMP=0)");
        return;
    }
    DWORD length = GetEnvironmentVariableW(L"BB_CRASH_DIR", dump_directory, 1024);
    if (length >= 1024) { fputs("Crash reporting: BB_CRASH_DIR is too long\n", stderr); return; }
    if (!length) {
        CreateDirectoryW(L"logs", NULL);
        wcscpy(dump_directory, L"logs\\crashes");
    }
    CreateDirectoryW(dump_directory, NULL);
    /* Preload before entering the guest/driver, avoiding LoadLibrary in a fault handler. */
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp) {
        fprintf(stderr, "Crash reporting: cannot load system DbgHelp (Windows error 0x%08lx)\n", GetLastError());
        return;
    }
    write_dump = (WriteDump)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (!write_dump) { fputs("Crash reporting: MiniDumpWriteDump unavailable\n", stderr); return; }
    requested = CreateEventW(NULL, FALSE, FALSE, NULL);
    completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!requested || !completed) {
        fprintf(stderr, "Crash reporting: event creation failed (Windows error 0x%08lx)\n", GetLastError());
        if (requested) CloseHandle(requested);
        if (completed) CloseHandle(completed);
        requested = completed = NULL;
        return;
    }
    HANDLE thread = CreateThread(NULL, 0, dump_worker, NULL, 0, NULL);
    if (!thread) {
        fprintf(stderr, "Crash reporting: writer thread creation failed (Windows error 0x%08lx)\n", GetLastError());
        CloseHandle(requested);
        CloseHandle(completed);
        requested = completed = NULL;
        return;
    }
    CloseHandle(thread);
    puts("Crash reporting: dedicated dump writer ready");
}

static void submit_dump(EXCEPTION_POINTERS *exception, const char *reason,
        HANDLE process, DWORD pid, DWORD tid) {
    if (!requested || !completed) return;
    /* First failure owns the snapshot. Other fatal threads wait for the same
     * completed file instead of overwriting context or terminating mid-write. */
    if (InterlockedCompareExchange(&dump_claimed, 1, 0) == 0) {
        record = *exception->ExceptionRecord;
        context = *exception->ContextRecord;
        snprintf(fatal_reason, sizeof(fatal_reason), "%s", reason);
        fault_process = process;
        fault_pid = pid;
        fault_thread = tid;
        SetEvent(requested);
    }
    if (WaitForSingleObject(completed, 10000) != WAIT_OBJECT_0) {
        fprintf(stderr, "Crash dump: timed out after 10 seconds\n");
    } else if (dump_error) {
        fprintf(stderr, "Crash dump: failed (Windows error 0x%08lx)\n", dump_error);
    } else {
        fprintf(stderr, "Crash dump: %s\n", dump_path_utf8);
    }
}

void crash_win_dump(EXCEPTION_POINTERS *exception) {
    submit_dump(exception, "Windows exception", GetCurrentProcess(), GetCurrentProcessId(), GetCurrentThreadId());
}

void crash_win_dump_external(HANDLE process, DWORD pid, DWORD tid, EXCEPTION_POINTERS *exception) {
    submit_dump(exception, "External crash monitor: unhandled Windows exception", process, pid, tid);
}

__attribute__((noinline)) void crash_win_dump_fatal(const char *reason) {
    if (!requested || !completed) return;
    CONTEXT current;
    RtlCaptureContext(&current);
    EXCEPTION_RECORD fatal = {0};
    fatal.ExceptionCode = BB_CRASH_ASSERT_EXCEPTION;
    fatal.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    fatal.ExceptionAddress = (void *)(uintptr_t)current.Rip;
    EXCEPTION_POINTERS exception = {&fatal, &current};
    submit_dump(&exception, reason ? reason : "Fatal stop", GetCurrentProcess(), GetCurrentProcessId(), GetCurrentThreadId());
}
#endif
