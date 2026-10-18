#pragma once
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <vector>

struct TcapAppChoice {
    std::wstring name;
    std::wstring label;
    bool executable = false;
    bool browse = false;
};

struct TcapComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~TcapComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

inline std::vector<TcapAppChoice> tcap_list_apps(const wchar_t* extension) {
    TcapComScope com;
    std::vector<TcapAppChoice> choices;
    Microsoft::WRL::ComPtr<IEnumAssocHandlers> enumerator;
    if (FAILED(SHAssocEnumHandlers(extension, ASSOC_FILTER_RECOMMENDED, &enumerator))) return choices;
    Microsoft::WRL::ComPtr<IAssocHandler> handler;
    while (enumerator->Next(1, &handler, nullptr) == S_OK) {
        PWSTR name = nullptr, label = nullptr;
        const HRESULT nameResult = handler->GetName(&name);
        const HRESULT labelResult = handler->GetUIName(&label);
        if (SUCCEEDED(nameResult) && name && *name && SUCCEEDED(labelResult) && label && *label) {
            bool duplicate = false;
            for (const auto& choice : choices) {
                if (_wcsicmp(choice.name.c_str(), name) == 0) { duplicate = true; break; }
            }
            if (!duplicate) choices.push_back({name, label});
        }
        CoTaskMemFree(name);
        CoTaskMemFree(label);
        handler.Reset();
    }
    return choices;
}

inline HRESULT tcap_choose_app(HWND owner, const std::wstring& title, std::wstring& executable) {
    TcapComScope com;
    if (FAILED(com.result)) return com.result;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) return result;
    FILEOPENDIALOGOPTIONS options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const COMDLG_FILTERSPEC types[] = {{L"Executable (*.exe)", L"*.exe"}};
    if (SUCCEEDED(result)) result = dialog->SetFileTypes(1, types);
    if (SUCCEEDED(result)) result = dialog->SetTitle(title.c_str());
    if (SUCCEEDED(result)) result = dialog->Show(owner);
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<IShellItem> item;
    result = dialog->GetResult(&item);
    PWSTR path = nullptr;
    if (SUCCEEDED(result)) result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (SUCCEEDED(result) && path) {
        if (_wcsicmp(std::filesystem::path(path).extension().c_str(), L".exe") == 0) executable = path;
        else result = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
    }
    CoTaskMemFree(path);
    return result;
}

inline HRESULT tcap_open_image(const std::wstring& filename, const std::wstring& appName,
                               const std::wstring& executable = {}) {
    if (!executable.empty()) {
        const auto application = std::filesystem::absolute(std::filesystem::path(executable));
        const auto directory = application.parent_path().wstring();
        // Pass the application explicitly and quote the single image argument, including spaces and Unicode.
        std::wstring command = L"\"" + application.wstring() + L"\" \"" + filename + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            directory.c_str(), &startup, &process)) {
            const DWORD error = GetLastError();
            return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return S_OK;
    }
    // Invoke the Shell handler so packaged apps such as Photos work as well as desktop apps.
    if (appName.empty()) {
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        execute.lpVerb = L"open";
        execute.lpFile = filename.c_str();
        execute.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&execute)) return S_OK;
        const DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    Microsoft::WRL::ComPtr<IEnumAssocHandlers> enumerator;
    const auto extension = std::filesystem::path(filename).extension().wstring();
    HRESULT result = SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_NONE, &enumerator);
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<IAssocHandler> selected, handler;
    while (enumerator->Next(1, &handler, nullptr) == S_OK) {
        PWSTR name = nullptr;
        if (SUCCEEDED(handler->GetName(&name)) && name && _wcsicmp(name, appName.c_str()) == 0) selected = handler;
        CoTaskMemFree(name);
        handler.Reset();
        if (selected) break;
    }
    if (!selected) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    PIDLIST_ABSOLUTE item = nullptr;
    result = SHParseDisplayName(filename.c_str(), nullptr, &item, 0, nullptr);
    if (FAILED(result)) return result;
    PIDLIST_ABSOLUTE folder = ILCloneFull(item);
    if (!folder) { CoTaskMemFree(item); return E_OUTOFMEMORY; }
    ILRemoveLastID(folder);
    PCUITEMID_CHILD child = ILFindLastID(item);
    Microsoft::WRL::ComPtr<IDataObject> data;
    result = SHCreateDataObject(folder, 1, &child, nullptr, IID_PPV_ARGS(&data));
    if (SUCCEEDED(result)) result = selected->Invoke(data.Get());
    CoTaskMemFree(folder);
    CoTaskMemFree(item);
    return result;
}
