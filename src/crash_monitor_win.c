/* Optional Windows debugger: preserve second-chance/fail-fast exceptions outside
 * the failed process. Normal launches do not use this event loop. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "crash_win.h"

/* Quote argv with the Windows backslash-before-quote rules, including empty
 * arguments and a quoted path ending in a backslash. CreateProcess mutates it. */
static wchar_t *command_line(int argc, wchar_t **argv) {
    size_t capacity=1;
    for (int i=1; i<argc; ++i) {
        size_t length=wcslen(argv[i]);
        if (length>16380 || capacity+length*2+3>32767) return NULL;
        capacity+=length*2+3;
    }
    wchar_t *command=calloc(capacity, sizeof(*command));
    if (!command) return NULL;
    wchar_t *out=command;
    for (int i=1; i<argc; ++i) {
        if (i>1) *out++=L' ';
        *out++=L'"';
        const wchar_t *p=argv[i];
        for (;;) {
            unsigned slashes=0;
            while (*p==L'\\') { ++slashes; ++p; }
            unsigned count=slashes;
            if (!*p || *p==L'"') count*=2;
            if (*p==L'"') ++count;
            while (count--) *out++=L'\\';
            if (!*p) break;
            *out++=*p++;
        }
        *out++=L'"';
    }
    return command;
}

int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc<2) {
        fputs("Usage: bb-crash-monitor.exe <program> [arguments...]\n", stderr);
        return 1;
    }
    wchar_t *command=command_line(argc, argv);
    if (!command) {
        fputs("Crash monitor: command too long or allocation failed\n", stderr);
        return 1;
    }
    crash_win_init();
    STARTUPINFOW start={.cb=sizeof(start)};
    start.dwFlags=STARTF_USESTDHANDLES;
    start.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
    start.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process={0};
    if (!CreateProcessW(argv[1], command, NULL, NULL, TRUE,
            DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, NULL, NULL, &start, &process)) {
        fprintf(stderr, "Crash monitor: CreateProcess failed (Windows error 0x%08lx)\n", GetLastError());
        free(command);
        return 1;
    }
    free(command);
    CloseHandle(process.hThread);
    DebugSetProcessKillOnExit(FALSE);
    printf("Crash monitor: observing native process %lu\n", process.dwProcessId);
    int initial_breakpoint=1, captured=0;
    DWORD exit_code=1;
    for (;;) {
        DEBUG_EVENT event={0};
        if (!WaitForDebugEvent(&event, INFINITE)) {
            fprintf(stderr, "Crash monitor: event wait failed (Windows error 0x%08lx)\n", GetLastError());
            DebugActiveProcessStop(process.dwProcessId);
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &exit_code);
            break;
        }
        DWORD continuation=DBG_CONTINUE;
        if (event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_DEBUG_INFO *failure=&event.u.Exception;
            continuation=DBG_EXCEPTION_NOT_HANDLED;
            if (initial_breakpoint && failure->dwFirstChance &&
                    failure->ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT) {
                initial_breakpoint=0;
                continuation=DBG_CONTINUE;
            } else if (!failure->dwFirstChance && !captured) {
                /* First-chance exceptions still reach the game's/driver's own
                 * handlers, including GPU page tracking and ordinary C++ throws. */
                captured=1;
                fprintf(stderr, "Crash monitor: unhandled exception 0x%08lx at %p (thread %lu)\n",
                        failure->ExceptionRecord.ExceptionCode, failure->ExceptionRecord.ExceptionAddress,
                        event.dwThreadId);
                CONTEXT context={.ContextFlags=CONTEXT_ALL};
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId);
                if (thread && GetThreadContext(thread, &context)) {
                    EXCEPTION_RECORD record=failure->ExceptionRecord;
                    record.ExceptionRecord=NULL;
                    EXCEPTION_POINTERS exception={&record, &context};
                    crash_win_dump_external(process.hProcess, process.dwProcessId, event.dwThreadId, &exception);
                } else {
                    fprintf(stderr, "Crash monitor: thread context failed (Windows error 0x%08lx)\n", GetLastError());
                }
                if (thread) CloseHandle(thread);
            }
        } else if (event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            exit_code=event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            break;
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation)) {
            fprintf(stderr, "Crash monitor: continuation failed (Windows error 0x%08lx)\n", GetLastError());
            DebugActiveProcessStop(process.dwProcessId);
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &exit_code);
            break;
        }
    }
    printf("Crash monitor: native exit code=%lu (0x%08lx)\n", exit_code, exit_code);
    CloseHandle(process.hProcess);
    return (int)exit_code;
}
