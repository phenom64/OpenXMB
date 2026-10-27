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
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct DiagnosticTag { virtual ~DiagnosticTag() = default; };
struct DiagnosticException : DiagnosticTag, std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::wstring quote_argument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for(const auto character : argument) {
        if(character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    return result + L'"';
}

void load_symbols(HANDLE process, HANDLE file, const void* base)
{
    std::vector<wchar_t> path(32768);
    const auto size = file ? GetFinalPathNameByHandleW(file, path.data(),
        static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED) : 0;
    std::wstring name = size && size < path.size() ? path.data() : L"";
    if(name.starts_with(L"\\\\?\\")) name.erase(0, 4);
    SetLastError(ERROR_SUCCESS);
    if(!SymLoadModuleExW(process, file, name.empty() ? nullptr : name.c_str(), nullptr,
                         reinterpret_cast<DWORD64>(base), 0, nullptr, 0) && GetLastError())
        std::printf("Symbol loading failed: %lu, base %p\n", GetLastError(), base);
}

template<class T> bool read_value(HANDLE process, ULONG_PTR address, T& value)
{
    SIZE_T read{};
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value,
                             sizeof(value), &read) && read == sizeof(value);
}

std::string read_text(HANDLE process, ULONG_PTR address)
{
    std::string text;
    for(unsigned i = 0; i < 2048; ++i) {
        char character{};
        if(!read_value(process, address + i, character)) return {};
        if(!character) return text;
        if(static_cast<unsigned char>(character) < 32 && character != '\n' && character != '\t') return {};
        text += character;
    }
    return {};
}

bool print_cpp_exception(HANDLE process, const EXCEPTION_RECORD& exception)
{
    if(exception.ExceptionCode != 0xE06D7363 || exception.NumberParameters != 4) return false;
    // The MSVC x64 exception record identifies the thrown object and relative
    // RTTI. Read only its type name and standard exception message, never a dump.
    const auto image = exception.ExceptionInformation[3];
    const auto info = exception.ExceptionInformation[2];
    auto standard_exception = exception.ExceptionInformation[1];
    std::uint32_t array{}, count{};
    if(read_value(process, info + 12, array) && read_value(process, image + array, count) && count <= 64) {
        for(std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t type{}, descriptor{};
            if(!read_value(process, image + array + 4 * (i + 1), type) ||
               !read_value(process, image + type + 4, descriptor)) continue;
            const auto name = read_text(process, image + descriptor + 2 * sizeof(void*));
            if(i == 0 && !name.empty()) std::printf("C++ exception type: %s\n", name.c_str());
            std::int32_t displacement{}, virtual_base{};
            if(name.find("AVexception@std@@") != std::string::npos &&
               read_value(process, image + type + 8, displacement) &&
               read_value(process, image + type + 12, virtual_base) && virtual_base == -1)
                standard_exception += displacement;
        }
    }
    ULONG_PTR message{};
    if(read_value(process, standard_exception + sizeof(void*), message)) {
        const auto text = read_text(process, message);
        if(!text.empty()) std::printf("C++ exception message: %s\n", text.c_str());
        return text == "OpenXMB native startup probe fixture";
    }
    return false;
}

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
#ifdef _MSC_VER
        throw DiagnosticException("OpenXMB native startup probe fixture");
#else
        RaiseException(0xE042584D, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 1;
#endif
    }
    if(argc < 2) return 2;
    const bool self_test = argc == 2 && std::wstring(argv[1]) == L"--self-test";
    std::vector<wchar_t> own_path(32768);
    GetModuleFileNameW(nullptr, own_path.data(), static_cast<DWORD>(own_path.size()));
    const auto executable = self_test ? own_path.data() : argv[1];
    std::wstring command = quote_argument(executable);
    if(self_test || argc == 2) command += self_test ? L" --crash" : L" --help";
    else for(int i = 2; i < argc; ++i) command += L" " + quote_argument(argv[i]);
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
    [[maybe_unused]] bool recorded_fixture_message = false;
    DWORD exit_status = 1;
    DEBUG_EVENT event{};
    while(WaitForDebugEvent(&event, 30000)) {
        DWORD status = DBG_CONTINUE;
        switch(event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if(!SymInitialize(child.hProcess, nullptr, FALSE))
                std::printf("Symbol initialization failed: %lu\n", GetLastError());
            load_symbols(child.hProcess, event.u.CreateProcessInfo.hFile,
                         event.u.CreateProcessInfo.lpBaseOfImage);
            if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            load_symbols(child.hProcess, event.u.LoadDll.hFile, event.u.LoadDll.lpBaseOfDll);
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
            recorded_fixture_message |= print_cpp_exception(child.hProcess, exception.ExceptionRecord);
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
            if(self_test) {
#ifdef _MSC_VER
                if(!recorded_fixture_message) return 1;
#endif
                return recorded_crash && exit_status != 0 ? 0 : 1;
            }
            return exit_status == 0 ? 0 : 1;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }
    std::puts("Startup probe timed out or the debugger API failed");
    TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 1;
}
