#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <intrin.h>

static DWORD WINAPI fail_fast(void *unused) {
    (void)unused;
    EXCEPTION_RECORD record={0};
    record.ExceptionCode=0xc0000409;
    record.ExceptionAddress=(void *)(uintptr_t)&fail_fast;
    RaiseFailFastException(&record, NULL, 0);
    return 99;
}
static LONG CALLBACK handled(EXCEPTION_POINTERS *exception) {
    return exception->ExceptionRecord->ExceptionCode==0xe0424343 ?
        EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    if (argc<2) return 1;
    wchar_t forced[64];
    DWORD length=GetEnvironmentVariableW(L"BB_TEST_CRASH_STOP", forced, 64);
    const wchar_t *mode=length && length<64 ? forced : argv[1];
    if (!wcscmp(mode, L"failfast")) return (int)fail_fast(NULL);
    if (!wcscmp(mode, L"worker")) {
        HANDLE thread=CreateThread(NULL, 0, fail_fast, NULL, 0, NULL);
        if (!thread) return 2;
        WaitForSingleObject(thread, INFINITE);
        return 99;
    }
    if (!wcscmp(mode, L"fastfail-intrinsic")) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    if (!wcscmp(mode, L"av")) {
        volatile int *pointer=(volatile int *)(uintptr_t)1;
        *pointer=1;
        return 99;
    }
    if (!wcscmp(mode, L"handled")) {
        AddVectoredExceptionHandler(1, handled);
        RaiseException(0xe0424343, 0, 0, NULL);
        puts("PASS: handled first-chance exception resumed");
        return 7;
    }
    if (!wcscmp(mode, L"argv")) {
        char buffer[2048];
        for (int i=2; i<argc; ++i) {
            if (!WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buffer, sizeof(buffer), NULL, NULL)) return 2;
            printf("arg=%s\n", buffer);
        }
        return 0;
    }
    return 7;
}
