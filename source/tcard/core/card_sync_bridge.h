#pragma once
#include "card_sync.h"
#include "card_assets.h"
#include <aclapi.h>
#include <sddl.h>
#include "card_signal_acl.h"
#include "card_sync_network.h"
#include <wincrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace tcard_bridge {
// Public configured node metadata only; no TLS, key loading or synchronization authority.
struct SignalNode {tcard_net::Peer peer;std::wstring pin;};
inline bool ParseSignalNodes(const std::wstring& text,std::vector<SignalNode>& nodes)
{
    nodes.clear();if(text.size()>8192)return false;std::vector<std::wstring> tokens;
    if(!tcard_net::SplitList(text,tokens)||tokens.size()>32)return false;
    for(const auto& token:tokens){const auto at=token.find(L'@');
        if(at==std::wstring::npos||token.find(L'@',at+1)!=std::wstring::npos)return false;
        const auto endpoint=token.substr(0,at),pin=token.substr(at+1);std::vector<tcard_net::Peer> parsed;
        if(pin.size()!=64||pin.find_first_not_of(L"0123456789abcdef")!=std::wstring::npos||
            !tcard_net::ParsePeers(endpoint,0,parsed)||parsed.size()!=1||endpoint!=parsed[0].ip+L":"+std::to_wstring(parsed[0].port))return false;
        if(std::any_of(nodes.begin(),nodes.end(),[&](const SignalNode& node){return node.peer.address==parsed[0].address&&node.peer.port==parsed[0].port;}))return false;
        nodes.push_back({parsed[0],pin});
    }return true;
}
inline bool ParsePublishedSyncEndpoint(const std::wstring& text,tcard_net::Peer& peer)
{
    peer={};if(text.empty())return true;std::vector<tcard_net::Peer> parsed;
    if(text.size()>32||!tcard_net::ParsePeers(text,0,parsed)||parsed.size()!=1||text!=parsed[0].ip+L":"+std::to_wstring(parsed[0].port))return false;
    peer=parsed[0];return true;
}
inline std::wstring Quote(const std::wstring& value)
{
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}result.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;result+=c;}
    result.append(slashes*2,L'\\');return result+L"\"";
}
inline bool Available(const std::wstring& executable)
{
    tcard_asset::File file(CreateFileW(executable.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    return file&&tcard_asset::regular(file.value,false);
}
inline HANDLE Launch(const std::wstring& executable,const std::vector<std::wstring>& arguments,DWORD& pid)
{
    if(!Available(executable))return nullptr;
    std::wstring command=Quote(executable);for(const auto& argument:arguments)command+=L" "+Quote(argument);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))return nullptr;
    CloseHandle(child.hThread);pid=child.dwProcessId;return child.hProcess;
}
// IPC package files trust only the current Windows user (plus SYSTEM), never arbitrary local writers.
inline bool Protect(const std::wstring& directory)
{
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    tcard_asset::File tokenScope(token);DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
    std::vector<BYTE> storage(size);if(!size||!GetTokenInformation(token,TokenUser,storage.data(),size,&size))return false;
    auto user=reinterpret_cast<TOKEN_USER*>(storage.data());LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(user->User.Sid,&sid))return false;
    bool preserved=false;const bool safe=tcard_signal_acl::PreserveUnavailable(directory,user->User.Sid,preserved);
    if(preserved){LocalFree(sid);return safe;}if(!safe){LocalFree(sid);return false;}
    const auto sddl=tcard_signal_acl::Template(sid);LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return false;
    struct Descriptor{PSECURITY_DESCRIPTOR value;~Descriptor(){LocalFree(value);}} scope{descriptor};
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    if(!CreateDirectoryW(directory.c_str(),&security)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    tcard_asset::File checked(tcard_asset::open_dir(directory,false));if(!checked)return false;
    PSID owner=nullptr;PSECURITY_DESCRIPTOR existing=nullptr;
    if(GetNamedSecurityInfoW(directory.c_str(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&existing)!=ERROR_SUCCESS)return false;
    const bool owned=EqualSid(owner,user->User.Sid)!=FALSE;LocalFree(existing);if(!owned)return false;
    PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    return GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)&&present&&
        SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr)==ERROR_SUCCESS;
}
inline bool Publish(const std::wstring& directory,const std::wstring& leaf,const std::wstring& value)
{
    return tcard_asset::publish(directory,leaf,tcard_asset::encode(value),false);
}
inline bool Read(const std::wstring& directory,const std::wstring& leaf,std::wstring& value)
{
    value.clear();if(!tcard_asset::leaf(leaf))return false;
    tcard_asset::File root(tcard_asset::open_dir(directory,false));if(!root)return false;
    // IPC snapshots remain immutable through this handle while their names are atomically replaced.
    tcard_asset::File file(CreateFileW((directory+L"\\"+leaf).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file||!tcard_asset::regular(file.value,false))return false;LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.value,&size)||size.QuadPart<0||static_cast<ULONGLONG>(size.QuadPart)>tcard_asset::max_file+tcard_crypto::envelopeOverhead)return false;
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));DWORD count=0;
    if(!bytes.empty()&&(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size()))return false;
    std::vector<BYTE> plain;if(!tcard_storage::Unpack(directory+L"\\"+leaf,bytes,plain)||plain.size()>tcard_asset::max_file)return false;
    value=tcard_asset::decode(plain);return tcard_asset::encode(value)==plain;
}
// File identity only suppresses repeat reads of a snapshot already validated by Read.
// Same-user IPC still performs authenticated decryption after replacement and every minute.
struct Stamp {
    DWORD volume=0,indexHigh=0,indexLow=0,sizeHigh=0,sizeLow=0;
    FILETIME created{},written{};
    bool operator==(const Stamp& other)const{
        return volume==other.volume&&indexHigh==other.indexHigh&&indexLow==other.indexLow&&sizeHigh==other.sizeHigh&&sizeLow==other.sizeLow&&
            CompareFileTime(&created,&other.created)==0&&CompareFileTime(&written,&other.written)==0;
    }
};
inline bool Fingerprint(const std::wstring& directory,const std::wstring& leaf,Stamp& stamp)
{
    if(!tcard_asset::leaf(leaf))return false;
    tcard_asset::File root(tcard_asset::open_dir(directory,false));if(!root)return false;
    tcard_asset::File file(CreateFileW((directory+L"\\"+leaf).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};
    if(!file||!tcard_asset::regular(file.value,false)||!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1)return false;
    stamp={info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,info.nFileSizeHigh,info.nFileSizeLow,info.ftCreationTime,info.ftLastWriteTime};return true;
}
inline std::wstring Hash(const std::wstring& value){return tcard_asset::hash(tcard_asset::encode(value));}
inline std::wstring Token()
{
    GUID guid{};wchar_t text[40]{};if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,text,40))throw std::runtime_error("Control token unavailable");
    return std::wstring(text+1,36);
}
inline bool Seal(const std::wstring& plain,std::wstring& sealed,bool open=false)
{
    sealed.clear();std::vector<BYTE> bytes;
    if(open){if(plain.size()>65536||plain.size()%2)return false;
        for(size_t i=0;i<plain.size();i+=2){auto digit=[](wchar_t c){return c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:-1;};
            const int a=digit(plain[i]),b=digit(plain[i+1]);if(a<0||b<0)return false;bytes.push_back(static_cast<BYTE>((a<<4)|b));}
    }else bytes=tcard_asset::encode(plain);
    const wchar_t domain[]=L"TCard.KeyControl.v1";DATA_BLOB entropy{sizeof(domain),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(domain))};
    DATA_BLOB input{static_cast<DWORD>(bytes.size()),bytes.data()},output{};
    const bool okay=(open?CryptUnprotectData(&input,nullptr,&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output):
        CryptProtectData(&input,L"TCard key control",&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))!=FALSE;
    SecureZeroMemory(bytes.data(),bytes.size());if(!okay)return false;
    struct Scope{DATA_BLOB value;~Scope(){SecureZeroMemory(value.pbData,value.cbData);LocalFree(value.pbData);}} scope{output};
    if(open){std::vector<BYTE> decoded(output.pbData,output.pbData+output.cbData);sealed=tcard_asset::decode(decoded);
        const bool valid=tcard_asset::encode(sealed)==decoded;SecureZeroMemory(decoded.data(),decoded.size());return valid;}
    for(DWORD i=0;i<output.cbData;++i){sealed+=L"0123456789abcdef"[output.pbData[i]>>4];sealed+=L"0123456789abcdef"[output.pbData[i]&15];}return true;
}
inline std::wstring Storage(const std::wstring& profile)
{
    const auto path=profile+L"\\active.json";const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){if(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)return profile+L"\\replica";
        throw std::runtime_error("Active profile unavailable");}
    std::wstring text;if(!Read(profile,L"active.json",text))throw std::runtime_error("Active profile unreadable");
    winrt::Windows::Data::Json::JsonObject data;
    if(!winrt::Windows::Data::Json::JsonObject::TryParse(text,data)||data.GetNamedNumber(L"Version",0)!=1)throw std::runtime_error("Unsupported active profile");
    const std::wstring group=data.GetNamedString(L"Group",L"").c_str();
    if(group.size()!=36||group.find_first_not_of(L"0123456789abcdefABCDEF-")!=std::wstring::npos)throw std::runtime_error("Invalid active group");
    return profile+L"\\groups\\"+group;
}
inline std::wstring Mailbox(const std::wstring& profile)
{
    const auto storage=Storage(profile);return storage==profile+L"\\replica"?profile+L"\\ui":storage+L"\\ui";
}
}
