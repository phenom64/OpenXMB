/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Run the relocated application under the native debugger API. Only text stack
// traces are retained; process memory dumps and runner environment data are not.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
void print_stack(HANDLE process, HANDLE thread)
{
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if(!GetThreadContext(thread, &context)) return;
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Rip, 0, AddrModeFlat};
    frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    std::puts("Native stack trace:");
    for(int depth = 0; depth < 48; ++depth) {
        const auto address = frame.AddrPC.Offset;
        if(!address) break;
        DWORD64 displacement{};
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        SymGetModuleInfo64(process, address, &module);
        if(SymFromAddr(process, address, &displacement, symbol)) {
            std::printf("  %s!%s + 0x%llx\n", module.ModuleName, symbol->Name, displacement);
        } else {
            std::printf("  %s + 0x%llx (0x%llx)\n", module.ModuleName,
                        address - module.BaseOfImage, address);
        }
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD offset{};
        if(SymGetLineFromAddr64(process, address, &offset, &line))
            std::printf("    %s:%lu\n", line.FileName, line.LineNumber);
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                        nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    }
    std::fflush(stdout);
}
}

int wmain(int argc, wchar_t** argv)
{
    if(argc == 2 && std::wstring(argv[1]) == L"--crash") {
        RaiseException(0xE042584D, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 1;
    }
    if(argc != 2) return 2;
    const bool self_test = std::wstring(argv[1]) == L"--self-test";
    std::vector<wchar_t> own_path(32768);
    GetModuleFileNameW(nullptr, own_path.data(), static_cast<DWORD>(own_path.size()));
    const auto executable = self_test ? own_path.data() : argv[1];
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " +
                           (self_test ? L"--crash" : L"--help");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if(!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                       DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &startup, &child)) {
        std::printf("Could not launch startup probe: %lu\n", GetLastError());
        return 2;
    }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                  SYMOPT_FAIL_CRITICAL_ERRORS);
    bool recorded_crash = false;
    DWORD exit_status = 1;
    DEBUG_EVENT event{};
    while(WaitForDebugEvent(&event, 30000)) {
        DWORD status = DBG_CONTINUE;
        switch(event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            SymInitialize(child.hProcess, nullptr, TRUE);
            if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            SymLoadModuleEx(child.hProcess, event.u.LoadDll.hFile, nullptr, nullptr,
                            reinterpret_cast<DWORD64>(event.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
            if(event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            break;
        case UNLOAD_DLL_DEBUG_EVENT:
            SymUnloadModule64(child.hProcess,
                              reinterpret_cast<DWORD64>(event.u.UnloadDll.lpBaseOfDll));
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            CloseHandle(event.u.CreateThread.hThread);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& exception = event.u.Exception;
            const auto code = exception.ExceptionRecord.ExceptionCode;
            if(code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) break;
            status = DBG_EXCEPTION_NOT_HANDLED;
            if(!exception.dwFirstChance || code == 0xC0000409) {
                recorded_crash = true;
                std::printf("Windows startup exception 0x%08lX at %p, first chance: %lu\n",
                            code, exception.ExceptionRecord.ExceptionAddress, exception.dwFirstChance);
                if(exception.ExceptionRecord.NumberParameters)
                    std::printf("Exception detail: 0x%llx\n",
                                exception.ExceptionRecord.ExceptionInformation[0]);
                const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                               FALSE, event.dwThreadId);
                if(thread) { print_stack(child.hProcess, thread); CloseHandle(thread); }
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exit_status = event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
            SymCleanup(child.hProcess);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            std::printf("Startup probe exit: 0x%08lX\n", exit_status);
            return self_test ? (recorded_crash && exit_status != 0 ? 0 : 1) : (exit_status == 0 ? 0 : 1);
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }
    std::puts("Startup probe timed out or the debugger API failed");
    TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 1;
}
