#include "runner.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <vector>

namespace tcyc {

namespace {
struct MonitorSpec {
    std::wstring exePath;
    std::vector<std::wstring> expectedArgs;
    bool argsParseOk = true;
};

std::wstring ToLowerString(std::wstring s) {
    for (auto& ch : s) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return s;
}

std::wstring EffectiveActionMode(const TaskConfig& task) {
    std::wstring mode = ToLowerString(task.actionMode);
    if (mode != L"program" && mode != L"command" && mode != L"shell") mode = L"program";
    return mode;
}

std::wstring NormalizePath(std::wstring s) {
    for (auto& ch : s) {
        if (ch == L'/') ch = L'\\';
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return s;
}

std::wstring GetSystemToolPath(const wchar_t* relativePath) {
    std::vector<wchar_t> buffer(MAX_PATH);
    UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0) return {};
    if (length >= buffer.size()) {
        buffer.resize(static_cast<size_t>(length) + 1);
        length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (length == 0 || length >= buffer.size()) return {};
    }
    std::wstring path(buffer.data(), length);
    path.push_back(L'\\');
    path.append(relativePath);
    return path;
}

bool IsDriveExePath(const std::wstring& path) {
    if (path.size() < 7) return false;
    const wchar_t drive = path[0];
    if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z'))) return false;
    if (path[1] != L':' || path[2] != L'\\') return false;
    const std::wstring lower = ToLowerString(path);
    return lower.substr(lower.size() - 4) == L".exe";
}

bool QueryProcessPath(DWORD pid, std::wstring& outPath) {
    outPath.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD size = static_cast<DWORD>(_countof(buf));
    BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &size);
    CloseHandle(h);
    if (!ok || size == 0) return false;
    outPath.assign(buf, size);
    return true;
}

bool TryParseArgsFromCmdline(const std::wstring& cmdLine, std::vector<std::wstring>& outArgs) {
    outArgs.clear();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv || argc <= 0) {
        if (argv) LocalFree(argv);
        return false;
    }
    for (int i = 1; i < argc; ++i) outArgs.emplace_back(argv[i]);
    LocalFree(argv);
    return true;
}

bool EqualArgTokensCaseInsensitive(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ToLowerString(a[i]) != ToLowerString(b[i])) return false;
    }
    return true;
}

bool QueryProcessCommandLine(DWORD pid, std::wstring& outCmdLine) {
    outCmdLine.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;

    auto cleanup = [&]() {
        CloseHandle(h);
    };

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        cleanup();
        return false;
    }

    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    auto ntQueryInformationProcess =
        reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!ntQueryInformationProcess) {
        cleanup();
        return false;
    }

    PROCESS_BASIC_INFORMATION pbi{};
    NTSTATUS st = ntQueryInformationProcess(h, ProcessBasicInformation, &pbi, sizeof(pbi), nullptr);
    if (st < 0 || !pbi.PebBaseAddress) {
        cleanup();
        return false;
    }

    PEB peb{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof(peb), &bytesRead) || bytesRead < sizeof(peb)) {
        cleanup();
        return false;
    }
    if (!peb.ProcessParameters) {
        cleanup();
        return false;
    }

    RTL_USER_PROCESS_PARAMETERS procParams{};
    if (!ReadProcessMemory(h, peb.ProcessParameters, &procParams, sizeof(procParams), &bytesRead) ||
        bytesRead < sizeof(procParams)) {
        cleanup();
        return false;
    }
    if (!procParams.CommandLine.Buffer || procParams.CommandLine.Length == 0) {
        cleanup();
        return false;
    }

    const size_t wcharCount = static_cast<size_t>(procParams.CommandLine.Length / sizeof(wchar_t));
    std::wstring cmdLine(wcharCount, L'\0');
    if (!ReadProcessMemory(
            h,
            procParams.CommandLine.Buffer,
            cmdLine.data(),
            procParams.CommandLine.Length,
            &bytesRead) ||
        bytesRead < procParams.CommandLine.Length) {
        cleanup();
        return false;
    }

    cleanup();
    outCmdLine.swap(cmdLine);
    return true;
}

bool AppendParsedArgs(const std::wstring& rawArgs, std::vector<std::wstring>& outArgs) {
    if (rawArgs.empty()) return true;
    std::vector<std::wstring> parsed;
    std::wstring synthetic = L"dummy.exe ";
    synthetic.append(rawArgs);
    if (!TryParseArgsFromCmdline(synthetic, parsed)) return false;
    outArgs.insert(outArgs.end(), parsed.begin(), parsed.end());
    return true;
}

bool BuildMonitorSpec(const TaskConfig& task, MonitorSpec& spec) {
    spec = {};
    if (task.actionPath.empty()) return false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(task.actionPath.c_str(), &argc);
    if (!argv || argc <= 0) {
        if (argv) LocalFree(argv);
        return false;
    }

    spec.exePath = NormalizePath(argv[0]);
    if (!IsDriveExePath(spec.exePath)) {
        LocalFree(argv);
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        spec.expectedArgs.emplace_back(argv[i]);
    }
    LocalFree(argv);
    if (!AppendParsedArgs(task.actionArgs, spec.expectedArgs)) {
        spec.argsParseOk = false;
    }
    return true;
}

} // namespace

bool CanMonitorTask(const TaskConfig& task) {
    MonitorSpec spec;
    return BuildMonitorSpec(task, spec);
}

bool IsTaskProcessRunning(const TaskConfig& task, ProcessMatchMode* outMode, bool forceCmdlineReadFailForTest) {
    if (outMode) *outMode = ProcessMatchMode::None;
    MonitorSpec spec;
    if (!BuildMonitorSpec(task, spec)) return false;
    const bool requireArgsMatch = task.watchdogRequireArgsMatch && !spec.expectedArgs.empty();
    if (requireArgsMatch && !spec.argsParseOk) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            std::wstring p;
            if (!QueryProcessPath(pe.th32ProcessID, p)) continue;
            if (NormalizePath(p) != spec.exePath) continue;

            if (!requireArgsMatch) {
                found = true;
                if (outMode) *outMode = ProcessMatchMode::PathOnly;
                break;
            }

            std::wstring cmdLine;
            if (forceCmdlineReadFailForTest || !QueryProcessCommandLine(pe.th32ProcessID, cmdLine)) {
                found = true;
                if (outMode) *outMode = ProcessMatchMode::PathOnlyFallbackArgsUnavailable;
                break;
            }

            std::vector<std::wstring> actualArgs;
            if (!TryParseArgsFromCmdline(cmdLine, actualArgs)) {
                found = true;
                if (outMode) *outMode = ProcessMatchMode::PathOnlyFallbackArgsUnavailable;
                break;
            }

            if (EqualArgTokensCaseInsensitive(spec.expectedArgs, actualArgs)) {
                found = true;
                if (outMode) *outMode = ProcessMatchMode::PathAndArgs;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool LaunchTask(const TaskConfig& task, std::wstring& outError) {
    outError.clear();
    if (task.actionPath.empty()) {
        outError = L"ActionPath is empty";
        return false;
    }

    const std::wstring mode = EffectiveActionMode(task);
    std::wstring app;
    std::wstring cmdLine;
    if (mode == L"program") {
        app = task.actionPath;
        cmdLine = L"\"";
        cmdLine.append(task.actionPath);
        cmdLine.append(L"\"");
        if (!task.actionArgs.empty()) {
            cmdLine.push_back(L' ');
            cmdLine.append(task.actionArgs);
        }
    } else if (mode == L"command") {
        app = GetSystemToolPath(L"cmd.exe");
        if (app.empty()) {
            outError = L"GetSystemDirectoryW failed for cmd.exe";
            return false;
        }
        std::wstring merged = task.actionPath;
        if (!task.actionArgs.empty()) {
            merged.push_back(L' ');
            merged.append(task.actionArgs);
        }
        cmdLine = L"cmd.exe /C ";
        cmdLine.append(merged);
    } else { // shell
        app = GetSystemToolPath(L"WindowsPowerShell\\v1.0\\powershell.exe");
        if (app.empty()) {
            outError = L"GetSystemDirectoryW failed for powershell.exe";
            return false;
        }
        std::wstring merged = task.actionPath;
        if (!task.actionArgs.empty()) {
            merged.push_back(L' ');
            merged.append(task.actionArgs);
        }
        cmdLine = L"powershell.exe -NoProfile -Command ";
        cmdLine.append(merged);
    }

    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    LPCWSTR cwd = task.actionCwd.empty() ? nullptr : task.actionCwd.c_str();
    BOOL ok = CreateProcessW(
        app.c_str(),
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd,
        &si,
        &pi);
    if (!ok) {
        wchar_t msg[128] = {0};
        swprintf_s(msg, L"CreateProcessW failed: %lu", GetLastError());
        outError = msg;
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace tcyc
