#pragma once
#include <windows.h>
#include <userenv.h>
#include <objbase.h>
#include <aclapi.h>
#include <sddl.h>
#include <string>
#include <stop_token>
#include <vector>
#include <stdexcept>
#include <utility>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <wincrypt.h>
#include <memory>
#pragma comment(lib, "crypt32.lib")

// This prerequisite launcher carries public control handles only. It is not a signaling broker.
namespace tcard_signal_sandbox {
constexpr DWORD waitMilliseconds=15000;
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& h)noexcept:value(std::exchange(h.value,nullptr)){}
    Handle& operator=(Handle&& h)noexcept{if(this!=&h){Reset();value=std::exchange(h.value,nullptr);}return *this;}
    ~Handle(){Reset();}
    void Reset(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=nullptr;}
    explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
inline void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
inline std::wstring SidText(PSID sid)
{
    LPWSTR text=nullptr;Require(ConvertSidToStringSidW(sid,&text)!=FALSE,"Sandbox SID conversion failed");
    std::wstring result(text);LocalFree(text);return result;
}
inline constexpr wchar_t productName[]=L"TCard.Signaling.v1";
class Profile {
    struct ProductTag{};
    std::wstring name_,sidText_,directory_;
    PSID sid_=nullptr;
    bool owned_=false,persistent_=false,created_=false,ready_=false;
    DWORD volume_=0,indexHigh_=0,indexLow_=0;
    void ValidateFolder(){
        LPWSTR raw=nullptr;const auto status=GetAppContainerFolderPath(sidText_.c_str(),&raw);
        if(FAILED(status)||!raw)throw std::runtime_error("Product signal profile folder failed HRESULT="+std::to_string(static_cast<unsigned long>(status)));
        struct Folder{LPWSTR value;~Folder(){CoTaskMemFree(value);}} folder{raw};std::wstring candidate(raw);
        while(candidate.size()>3&&candidate.back()==L'\\')candidate.pop_back();
        wchar_t absolute[32768]{};const auto size=GetFullPathNameW(candidate.c_str(),32768,absolute,nullptr);
        if(!size){const DWORD error=GetLastError();throw std::runtime_error("Product signal profile path failed Win32="+std::to_string(error));}
        Require(size<32768&&_wcsicmp(absolute,candidate.c_str())==0,"Product signal profile path is not canonical absolute");
        // Reject any traversed directory link rather than granting access through an unknown alias.
        size_t begin=3;
        if(candidate.rfind(L"\\\\",0)==0){
            const auto server=candidate.find(L'\\',2),share=server==std::wstring::npos?server:candidate.find(L'\\',server+1);
            Require(server!=std::wstring::npos&&share!=std::wstring::npos,"Product signal UNC profile path has no share");begin=share+1;
        }
        for(size_t at=candidate.find(L'\\',begin);at!=std::wstring::npos;at=candidate.find(L'\\',at+1)){
            const auto part=candidate.substr(0,at);const auto attributes=GetFileAttributesW(part.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();throw std::runtime_error("Product signal profile ancestor failed Win32="+std::to_string(error));}
            Require((attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Product signal profile ancestor is not a regular directory");
        }
        Handle directory(CreateFileW(candidate.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!directory){const DWORD error=GetLastError();throw std::runtime_error("Product signal profile open failed Win32="+std::to_string(error));}
        BY_HANDLE_FILE_INFORMATION information{};if(!GetFileInformationByHandle(directory.value,&information)){
            const DWORD error=GetLastError();throw std::runtime_error("Product signal profile identity failed Win32="+std::to_string(error));}
        Require((information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Product signal profile is not a regular directory");
        if(!directory_.empty())Require(_wcsicmp(directory_.c_str(),candidate.c_str())==0&&volume_==information.dwVolumeSerialNumber&&
            indexHigh_==information.nFileIndexHigh&&indexLow_==information.nFileIndexLow,"Product signal profile directory identity changed");
        PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
        const auto error=GetSecurityInfo(directory.value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor);
        if(error!=ERROR_SUCCESS)throw std::runtime_error("Product signal profile owner failed Win32="+std::to_string(error));
        struct Security{PSECURITY_DESCRIPTOR value;~Security(){LocalFree(value);}} security{descriptor};
        HANDLE rawToken=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&rawToken)){
            const DWORD tokenError=GetLastError();throw std::runtime_error("Product signal user token failed Win32="+std::to_string(tokenError));}
        Handle token(rawToken);DWORD bytes=0;
        GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);const DWORD sizeError=GetLastError();
        if(!bytes)throw std::runtime_error("Product signal token sizing failed Win32="+std::to_string(sizeError));
        Require(bytes>=sizeof(TOKEN_USER)&&bytes<=65536,"Product signal user token size invalid");
        std::vector<BYTE> data(bytes);if(!GetTokenInformation(token.value,TokenUser,data.data(),bytes,&bytes)){
            const DWORD tokenError=GetLastError();throw std::runtime_error("Product signal user SID failed Win32="+std::to_string(tokenError));}
        const auto user=reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
        Require(owner&&IsValidSid(owner)&&user&&IsValidSid(user)&&EqualSid(owner,user),"Product signal profile has a foreign owner");
        if(directory_.empty()){directory_=std::move(candidate);volume_=information.dwVolumeSerialNumber;indexHigh_=information.nFileIndexHigh;indexLow_=information.nFileIndexLow;}
    }
    explicit Profile(ProductTag):name_(productName),persistent_(true){
        // Resolution and profile creation are separate from synchronization authentication/credentials.
        const auto derive=DeriveAppContainerSidFromAppContainerName(productName,&sid_);
        if(FAILED(derive)||!sid_)throw std::runtime_error("Product signal SID derivation failed HRESULT="+std::to_string(static_cast<unsigned long>(derive)));
        try{
            sidText_=SidText(sid_);PSID returned=nullptr;
            const auto status=CreateAppContainerProfile(productName,L"TCard signaling",L"Persistent public signaling state",nullptr,0,&returned);
            struct Returned{PSID value;~Returned(){if(value)FreeSid(value);}} created{returned};
            if(SUCCEEDED(status)){created_=true;Require(returned&&IsValidSid(returned)&&EqualSid(sid_,returned),"Product signal created SID mismatch");}
            else if(status!=HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))throw std::runtime_error("Product signal profile creation failed HRESULT="+std::to_string(static_cast<unsigned long>(status)));
            ValidateFolder();ready_=true;
        }catch(...){
            // Persistent profiles are never erased as a launch/validation cleanup side effect.
            std::fwprintf(stderr,L"Persistent signal profile retained after validation failure: %ls created=%u\n",productName,created_?1u:0u);
            FreeSid(sid_);sid_=nullptr;throw;
        }
    }
public:
    explicit Profile(const std::wstring& name):name_(name){
        Require(_wcsicmp(name.c_str(),productName)!=0,"Persistent identity requires the product profile branch");
        // Never adopt an existing disposable profile: only this successful creation authorizes removal.
        const auto hr=CreateAppContainerProfile(name.c_str(),L"TCard synthetic isolation",L"Disposable isolation prerequisite",nullptr,0,&sid_);
        Require(SUCCEEDED(hr),"Sandbox profile creation failed or profile already exists");owned_=true;created_=true;
        try{sidText_=SidText(sid_);ready_=true;}catch(...){
            if(FAILED(DeleteAppContainerProfile(name_.c_str())))std::fwprintf(stderr,L"Retained owned sandbox profile after SID conversion failure: %ls\n",name_.c_str());
            FreeSid(sid_);sid_=nullptr;owned_=false;throw;
        }
    }
    static Profile Product(){return Profile(ProductTag{});}
    Profile(const Profile&)=delete;
    ~Profile(){if(sid_)FreeSid(sid_);}
    PSID Sid()const{return sid_;}
    const std::wstring& Text()const{return sidText_;}
    const std::wstring& Directory()const{return directory_;}
    bool Owned()const{return owned_;}
    bool Persistent()const{return persistent_;}
    bool Created()const{return created_;}
    bool Ready()const{return ready_&&sid_&&!sidText_.empty();}
    void VerifyProduct(){
        Require(persistent_&&Ready(),"Product profile verification requires a ready persistent profile");
        PSID expected=nullptr;const auto status=DeriveAppContainerSidFromAppContainerName(productName,&expected);
        if(FAILED(status)||!expected)throw std::runtime_error("Product signal SID recheck failed HRESULT="+std::to_string(static_cast<unsigned long>(status)));
        struct Expected{PSID value;~Expected(){FreeSid(value);}} cleanup{expected};
        Require(IsValidSid(expected)&&EqualSid(expected,sid_),"Product signal SID changed");ValidateFolder();
    }
    void Remove(){Require(owned_&&!persistent_&&SUCCEEDED(DeleteAppContainerProfile(name_.c_str())),"Owned disposable sandbox profile cleanup failed");owned_=false;ready_=false;}
};
inline bool CheckToken(HANDLE process,const std::wstring& expected)
{
    HANDLE raw=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&raw))return false;Handle token(raw);
    DWORD isContainer=0,size=0;
    if(!GetTokenInformation(token.value,TokenIsAppContainer,&isContainer,sizeof(isContainer),&size)||isContainer!=1)return false;
    GetTokenInformation(token.value,TokenAppContainerSid,nullptr,0,&size);
    if(size<sizeof(TOKEN_APPCONTAINER_INFORMATION)||size>65536)return false;
    std::vector<BYTE> buffer(size);
    if(!GetTokenInformation(token.value,TokenAppContainerSid,buffer.data(),size,&size))return false;
    const auto sid=reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION*>(buffer.data())->TokenAppContainer;
    PSID target=nullptr;if(!ConvertStringSidToSidW(expected.c_str(),&target))return false;
    const bool matches=sid&&IsValidSid(sid)&&EqualSid(sid,target);LocalFree(target);return matches;
}
// The production node and its private-denial fixture must carry exactly the reviewed network capabilities.
inline bool CheckSignalToken(HANDLE process,const std::wstring& expected)
{
    if(!CheckToken(process,expected))return false;HANDLE raw=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&raw))return false;
    Handle token(raw);DWORD bytes=0;GetTokenInformation(token.value,TokenCapabilities,nullptr,0,&bytes);
    if(bytes<offsetof(TOKEN_GROUPS,Groups)||bytes>65536)return false;std::vector<BYTE> data(bytes);
    if(!GetTokenInformation(token.value,TokenCapabilities,data.data(),bytes,&bytes))return false;
    const auto groups=reinterpret_cast<TOKEN_GROUPS*>(data.data());
    if(groups->GroupCount!=2||bytes<offsetof(TOKEN_GROUPS,Groups)+2*sizeof(SID_AND_ATTRIBUTES))return false;
    const WELL_KNOWN_SID_TYPE types[]={WinCapabilityInternetClientServerSid,WinCapabilityPrivateNetworkClientServerSid};
    for(const auto type:types){BYTE buffer[SECURITY_MAX_SID_SIZE]{};DWORD size=sizeof(buffer);if(!CreateWellKnownSid(type,nullptr,buffer,&size))return false;
        unsigned found=0;for(DWORD i=0;i<groups->GroupCount;++i)if(groups->Groups[i].Sid&&IsValidSid(groups->Groups[i].Sid)&&EqualSid(groups->Groups[i].Sid,buffer)&&
            (groups->Groups[i].Attributes&SE_GROUP_ENABLED))++found;if(found!=1)return false;}
    return true;
}
inline bool PrivilegesSafe()
{
    HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))return false;Handle token(raw);DWORD size=0;
    GetTokenInformation(token.value,TokenPrivileges,nullptr,0,&size);if(size<offsetof(TOKEN_PRIVILEGES,Privileges)||size>65536)return false;
    std::vector<BYTE> data(size);if(!GetTokenInformation(token.value,TokenPrivileges,data.data(),size,&size))return false;
    const auto privileges=reinterpret_cast<TOKEN_PRIVILEGES*>(data.data());
    if(size<offsetof(TOKEN_PRIVILEGES,Privileges)||privileges->PrivilegeCount>(size-offsetof(TOKEN_PRIVILEGES,Privileges))/sizeof(LUID_AND_ATTRIBUTES))return false;
    const wchar_t* names[]={SE_DEBUG_NAME,SE_IMPERSONATE_NAME,SE_TCB_NAME,SE_TAKE_OWNERSHIP_NAME,SE_RESTORE_NAME};
    for(const auto name:names){LUID id{};if(!LookupPrivilegeValueW(nullptr,name,&id))return false;
        for(DWORD i=0;i<privileges->PrivilegeCount;++i)if(privileges->Privileges[i].Luid.LowPart==id.LowPart&&
            privileges->Privileges[i].Luid.HighPart==id.HighPart&&(privileges->Privileges[i].Attributes&SE_PRIVILEGE_ENABLED))return false;}
    return true;
}
inline std::wstring Quote(const std::wstring& input)
{
    std::wstring output=L"\"";size_t slashes=0;
    for(const auto c:input){if(c==L'\\'){++slashes;continue;}output.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;output+=c;}
    output.append(slashes*2,L'\\');return output+L"\"";
}
struct Packet {DWORD kind=0,code=0,pid=0;};
constexpr DWORD readyPacket=1,resultPacket=2;
inline bool WritePacket(HANDLE pipe,const Packet& packet)
{
    DWORD written=0;return WriteFile(pipe,&packet,sizeof(packet),&written,nullptr)&&written==sizeof(packet);
}
inline bool ReadPacket(HANDLE pipe,Packet& packet,DWORD timeout=waitMilliseconds)
{
    const auto end=GetTickCount64()+timeout;
    while(GetTickCount64()<end){DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))return false;
        if(available>=sizeof(packet)){DWORD read=0;return ReadFile(pipe,&packet,sizeof(packet),&read,nullptr)&&read==sizeof(packet);}Sleep(5);}
    return false;
}
inline char ReadControl(HANDLE pipe,DWORD timeout=waitMilliseconds)
{
    const auto end=GetTickCount64()+timeout;
    while(GetTickCount64()<end){DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))return 0;
        if(available){char command=0;DWORD read=0;return ReadFile(pipe,&command,1,&read,nullptr)&&read==1?command:0;}Sleep(5);}
    return 0;
}
struct Child {
    Handle process,control,result;
    DWORD pid=0;
    FILETIME created{};
    Child()=default;
    Child(const Child&)=delete;
    Child(Child&&)=default;
    ~Child(){control.Reset();if(process&&WaitForSingleObject(process.value,waitMilliseconds)!=WAIT_OBJECT_0)
        std::fprintf(stderr,"Retained sandbox PID=%lu created=%08lx%08lx requires normal shutdown; no force used\n",pid,created.dwHighDateTime,created.dwLowDateTime);}
    bool Command(char command){DWORD written=0;return control&&WriteFile(control.value,&command,1,&written,nullptr)&&written==1;}
    bool Wait(DWORD& code,DWORD timeout=waitMilliseconds){
        return process&&WaitForSingleObject(process.value,timeout)==WAIT_OBJECT_0&&GetExitCodeProcess(process.value,&code);}
    bool Exited()const{return process&&WaitForSingleObject(process.value,0)==WAIT_OBJECT_0;}
};
enum class ProbeMode {Synthetic,Protection,Signal,ProtectionSignal};
inline void Launch(Child& child,const std::wstring& executable,const Profile& profile,const std::vector<std::wstring>& arguments,ProbeMode mode=ProbeMode::Synthetic)
{
    Require(profile.Ready()&&!child.process,"Sandbox launch requires a verified profile and empty child");
    Require(mode==ProbeMode::Synthetic||mode==ProbeMode::Protection||mode==ProbeMode::Signal||mode==ProbeMode::ProtectionSignal,"Sandbox probe mode is invalid");
    if(profile.Persistent())Require(!profile.Directory().empty(),"Product signal profile directory was not verified");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE inputRead=nullptr,inputWrite=nullptr,outputRead=nullptr,outputWrite=nullptr;
    Require(CreatePipe(&inputRead,&inputWrite,&security,4096)!=FALSE,"Sandbox control pipe creation failed");
    Handle childInput(inputRead),parentInput(inputWrite);
    Require(CreatePipe(&outputRead,&outputWrite,&security,4096)!=FALSE,"Sandbox result pipe creation failed");
    Handle parentOutput(outputRead),childOutput(outputWrite);
    Require(SetHandleInformation(parentInput.value,HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(parentOutput.value,HANDLE_FLAG_INHERIT,0),"Sandbox pipe inheritance restriction failed");
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,2,0,&bytes);std::vector<BYTE> data(bytes);
    auto list=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(data.data());
    if(!InitializeProcThreadAttributeList(list,2,0,&bytes)){const DWORD error=GetLastError();throw std::runtime_error("Sandbox process attributes failed Win32="+std::to_string(error));}
    struct Attributes{PPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}} attributes{list};
    const bool signal=mode==ProbeMode::Signal||mode==ProbeMode::ProtectionSignal;
    Require(!signal||profile.Persistent(),"Signal capability mode requires the verified product profile");
    BYTE capabilityData[2][SECURITY_MAX_SID_SIZE]{};SID_AND_ATTRIBUTES capability[2]{};
    const WELL_KNOWN_SID_TYPE types[]={WinCapabilityInternetClientServerSid,WinCapabilityPrivateNetworkClientServerSid};
    const DWORD capabilityCount=signal?2:1;for(DWORD i=0;i<capabilityCount;++i){DWORD size=sizeof(capabilityData[i]);
        Require(CreateWellKnownSid(types[i],nullptr,capabilityData[i],&size)!=FALSE,"Sandbox network capability failed");capability[i]={capabilityData[i],SE_GROUP_ENABLED};}
    SECURITY_CAPABILITIES capabilities{profile.Sid(),capability,capabilityCount,0};HANDLE allowed[]={childInput.value,childOutput.value};
    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,&capabilities,sizeof(capabilities),nullptr,nullptr)){
        const DWORD error=GetLastError();throw std::runtime_error("Sandbox security-capabilities attribute failed Win32="+std::to_string(error));}
    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed,sizeof(allowed),nullptr,nullptr)){
        const DWORD error=GetLastError();throw std::runtime_error("Sandbox handle-list attribute failed Win32="+std::to_string(error));}
    const auto probe=mode==ProbeMode::Signal?L" --signal-node ":mode==ProbeMode::ProtectionSignal?L" --signal-protection-signal-probe ":mode==ProbeMode::Protection?L" --signal-protection-probe ":L" --signal-sandbox-probe ";
    std::wstring command=Quote(executable)+probe+Quote(profile.Text())+L" "+
        std::to_wstring(reinterpret_cast<ULONG_PTR>(childInput.value))+L" "+std::to_wstring(reinterpret_cast<ULONG_PTR>(childOutput.value));
    for(const auto& argument:arguments)command+=L" "+Quote(argument);
    wchar_t windows[32768]{};const auto count=GetWindowsDirectoryW(windows,32768);Require(count&&count<32768,"Sandbox system directory unavailable");
    const auto directory=executable.substr(0,executable.find_last_of(L"\\/"));
    // Microsoft AppContainer profile contract names LOCALAPPDATA and reroutes TEMP/TMP.
    // These three missing-key candidates are not claimed to be a proven minimal runtime environment.
    LPWSTR folderRaw=nullptr;const auto folderStatus=GetAppContainerFolderPath(profile.Text().c_str(),&folderRaw);
    if(FAILED(folderStatus)||!folderRaw)throw std::runtime_error("Sandbox owned profile folder failed HRESULT="+std::to_string(static_cast<unsigned long>(folderStatus)));
    struct Folder{LPWSTR value;~Folder(){CoTaskMemFree(value);}} folderMemory{folderRaw};const std::wstring folder(folderRaw),temporary=folder+L"\\Temp";
    const auto folderAttributes=GetFileAttributesW(folder.c_str());
    if(folderAttributes==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();throw std::runtime_error("Sandbox owned profile attributes failed Win32="+std::to_string(error));}
    Require((folderAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(folderAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Sandbox owned profile is not a regular directory");
    if(!CreateDirectoryW(temporary.c_str(),nullptr)){const DWORD error=GetLastError();
        if(error!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Sandbox owned Temp creation failed Win32="+std::to_string(error));}
    const auto tempAttributes=GetFileAttributesW(temporary.c_str());
    if(tempAttributes==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();throw std::runtime_error("Sandbox owned Temp attributes failed Win32="+std::to_string(error));}
    Require((tempAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(tempAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Sandbox owned Temp is not a regular directory");
    // Sorted Unicode double-NUL block, no ambient credentials/private stores/PATH or inherited cwd.
    std::wstring environment=L"LOCALAPPDATA="+folder;environment+=L'\0';
    environment+=L"SystemRoot="+std::wstring(windows);environment+=L'\0';
    environment+=L"TEMP="+temporary;environment+=L'\0';environment+=L"TMP="+temporary;environment+=L'\0';
    environment+=L"WINDIR="+std::wstring(windows);environment+=L'\0';environment+=L'\0';
    Require(environment.size()<=32767,"Sandbox explicit environment exceeds bound");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=list;PROCESS_INFORMATION information{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),directory.c_str(),&startup.StartupInfo,&information)){
        const DWORD error=GetLastError();throw std::runtime_error("Sandbox hidden launch failed Win32="+std::to_string(error));}
    // Caller owns the process before any post-creation check can throw.
    child.process=Handle(information.hProcess);child.control=std::move(parentInput);child.result=std::move(parentOutput);child.pid=information.dwProcessId;
    Handle thread(information.hThread);FILETIME exit{},kernel{},user{};
    if(!GetProcessTimes(child.process.value,&child.created,&exit,&kernel,&user)||!CheckToken(child.process.value,profile.Text())||(signal&&!CheckSignalToken(child.process.value,profile.Text()))){
        child.control.Reset();DWORD code=0;Require(child.Wait(code),"Sandbox token refusal child still requires normal shutdown");
        throw std::runtime_error("Sandbox parent token verification failed; no GO was sent");}
}
// Public runtime staging runs only during explicit hosting admission. Guards outlive the child.
class Runtime {
    struct Descriptor {
        PSECURITY_DESCRIPTOR value=nullptr;
        explicit Descriptor(const std::wstring& text){Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&value,nullptr)!=FALSE,"Signal runtime descriptor failed");}
        ~Descriptor(){if(value)LocalFree(value);}
    };
    std::wstring path_,user_;
    std::vector<Handle> directories_;
    Handle executable_;
    static std::wstring Extended(const std::wstring& path){return path.starts_with(L"\\\\?\\")?path:path.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;}
    static std::wstring Hash(const std::vector<BYTE>& bytes){BYTE hash[32]{};DWORD size=sizeof(hash);Require(!bytes.empty()&&bytes.size()<=32*1024*1024&&
        CryptHashCertificate2(L"SHA256",0,nullptr,bytes.data(),static_cast<DWORD>(bytes.size()),hash,&size)&&size==32,"Signal runtime hash failed");
        const wchar_t digits[]=L"0123456789abcdef";std::wstring result;for(auto b:hash){result+=digits[b>>4];result+=digits[b&15];}return result;}
    static std::vector<BYTE> Read(HANDLE file){BY_HANDLE_FILE_INFORMATION info{};Require(GetFileInformationByHandle(file,&info)&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1&&!info.nFileSizeHigh&&
        info.nFileSizeLow&&info.nFileSizeLow<=32*1024*1024,"Signal runtime file identity or size invalid");
        LARGE_INTEGER zero{};Require(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE,"Signal runtime rewind failed");std::vector<BYTE> bytes(info.nFileSizeLow);DWORD read=0;
        Require(ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size(),"Signal runtime read failed");return bytes;}
    std::wstring Template(const Profile& profile,bool directory)const{const auto flags=directory?L"OICI":L"";
        return L"O:"+user_+L"D:P(D;"+flags+L";0x000d0156;;;"+profile.Text()+L")(A;"+flags+L";RC;;;OW)(A;"+flags+L";FA;;;"+user_+
            L")(A;"+flags+L";FA;;;SY)(A;"+flags+L";FRFX;;;"+profile.Text()+L")";}
    void Validate(HANDLE file,const Descriptor& expected,bool directory){BY_HANDLE_FILE_INFORMATION info{};Require(GetFileInformationByHandle(file,&info)&&
        !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory&&
        (directory||info.nNumberOfLinks==1),"Signal runtime path is not regular");PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR raw=nullptr;
        const auto error=GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&raw);
        if(error!=ERROR_SUCCESS)throw std::runtime_error("Signal runtime security read failed Win32="+std::to_string(error));
        struct Free{PSECURITY_DESCRIPTOR value;~Free(){LocalFree(value);}} free{raw};PSID wantedOwner=nullptr;BOOL ignored=FALSE,present=FALSE;PACL wanted=nullptr;
        SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;Require(GetSecurityDescriptorOwner(expected.value,&wantedOwner,&ignored)&&
            GetSecurityDescriptorDacl(expected.value,&present,&wanted,&ignored)&&present&&wanted&&GetSecurityDescriptorControl(raw,&control,&revision)&&
            (control&SE_DACL_PROTECTED)&&owner&&wantedOwner&&EqualSid(owner,wantedOwner)&&acl&&IsValidAcl(acl)&&acl->AclSize==wanted->AclSize&&
            std::memcmp(acl,wanted,acl->AclSize)==0,"Signal runtime owner or protected ACL differs");}
    void Directory(const std::wstring& path,const Descriptor& expected){SECURITY_ATTRIBUTES security{sizeof(security),expected.value,FALSE};
        if(!CreateDirectoryW(Extended(path).c_str(),&security)){const DWORD error=GetLastError();if(error!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Signal runtime directory create failed Win32="+std::to_string(error));}
        Handle handle(CreateFileW(Extended(path).c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!handle)throw std::runtime_error("Signal runtime directory open failed Win32="+std::to_string(GetLastError()));
        Validate(handle.value,expected,true);directories_.push_back(std::move(handle));}
public:
    Runtime(Profile& profile,const std::wstring& source){profile.VerifyProduct();
        Handle anchor(CreateFileW(Extended(profile.Directory()).c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!anchor)throw std::runtime_error("Signal runtime profile anchor failed Win32="+std::to_string(GetLastError()));
        BY_HANDLE_FILE_INFORMATION anchorInfo{};Require(GetFileInformationByHandle(anchor.value,&anchorInfo)&&(anchorInfo.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&
            !(anchorInfo.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Signal runtime profile anchor is not regular");profile.VerifyProduct();directories_.reserve(3);directories_.push_back(std::move(anchor));
        Require(!source.empty()&&source.size()<32768,"Signal runtime source path invalid");
        const auto leaf=source.find_last_of(L"\\/");Require(leaf!=std::wstring::npos&&_wcsicmp(source.substr(leaf+1).c_str(),L"TCardSignal.exe")==0,"Signal runtime source is not the optional artifact");
        wchar_t absolute[32768]{};const auto length=GetFullPathNameW(source.c_str(),32768,absolute,nullptr);Require(length&&length<32768&&_wcsicmp(source.c_str(),absolute)==0,"Signal runtime source must be canonical absolute");
        Handle input(CreateFileW(Extended(source).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!input)throw std::runtime_error("Signal runtime source open failed Win32="+std::to_string(GetLastError()));const auto bytes=Read(input.value);const auto digest=Hash(bytes);
        Require(bytes.size()>=2&&bytes[0]=='M'&&bytes[1]=='Z',"Signal runtime artifact header invalid");HANDLE tokenRaw=nullptr;Require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tokenRaw)!=FALSE,"Signal runtime user token failed");
        Handle token(tokenRaw);DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);Require(size>=sizeof(TOKEN_USER)&&size<=65536,"Signal runtime user SID bound invalid");std::vector<BYTE> user(size);
        Require(GetTokenInformation(token.value,TokenUser,user.data(),size,&size)!=FALSE,"Signal runtime user SID unavailable");user_=SidText(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        const auto root=profile.Directory()+L"\\Runtime",directory=root+L"\\"+digest;Descriptor dirSD(Template(profile,true)),fileSD(Template(profile,false));
        Directory(root,dirSD);Directory(directory,dirSD);path_=directory+L"\\TCardSignal.exe";SECURITY_ATTRIBUTES security{sizeof(security),fileSD.value,FALSE};
        executable_=Handle(CreateFileW(Extended(path_).c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,&security,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(executable_){Validate(executable_.value,fileSD,false);DWORD written=0;Require(WriteFile(executable_.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&
                written==bytes.size()&&FlushFileBuffers(executable_.value),"Signal runtime durable copy failed");}
        else{const DWORD error=GetLastError();if(error!=ERROR_FILE_EXISTS)throw std::runtime_error("Signal runtime copy create failed Win32="+std::to_string(error));
            executable_=Handle(CreateFileW(Extended(path_).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!executable_)throw std::runtime_error("Signal runtime existing copy open failed Win32="+std::to_string(GetLastError()));Validate(executable_.value,fileSD,false);}
        Require(Hash(Read(executable_.value))==digest,"Signal runtime durable copy hash differs");
        // No writable handle remains when Windows opens an executable image section.
        executable_.Reset();executable_=Handle(CreateFileW(Extended(path_).c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!executable_)throw std::runtime_error("Signal runtime verified image open failed Win32="+std::to_string(GetLastError()));Validate(executable_.value,fileSD,false);
        Require(Hash(Read(executable_.value))==digest,"Signal runtime verified image changed");
        // Source/plugin ACLs are unchanged; there is no old-version or partial-copy deletion.
    }
    Runtime(const Runtime&)=delete;
    const std::wstring& Path()const{return path_;}
};
inline Runtime PrepareRuntime(Profile& profile,const std::wstring& source){return Runtime(profile,source);}
// Returns only after the fixed production node token/READY gate. The owner checks its protected roots before GO.
inline void LaunchSignal(Child& child,const Runtime& runtime,Profile& profile,const std::wstring& state,std::stop_token stop={})
{
    Require(!stop.stop_requested(),"Signal launch cancelled");profile.VerifyProduct();
    Launch(child,runtime.Path(),profile,{state},ProbeMode::Signal);Packet packet{};bool ready=false;const auto until=GetTickCount64()+waitMilliseconds;
    while(!stop.stop_requested()&&GetTickCount64()<until&&!child.Exited()){if(ReadPacket(child.result.value,packet,50)){ready=true;break;}}
    if(!ready||stop.stop_requested())child.control.Reset();
    Require(!stop.stop_requested()&&ready&&packet.kind==readyPacket&&packet.code==0&&packet.pid==child.pid&&
        CheckSignalToken(child.process.value,profile.Text()),"Signal production READY failed");
}
} // namespace tcard_signal_sandbox
