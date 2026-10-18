#include "signal_node.h"
#include "../core/card_signal_sandbox.h"
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <array>
#include <cstdio>

namespace {
using namespace tcard_signal;
namespace sandbox=tcard_signal_sandbox;
using Handle=sandbox::Handle;
using JsonObject=winrt::Windows::Data::Json::JsonObject;
using JsonType=winrt::Windows::Data::Json::JsonValueType;
struct Secret {
    Bytes bytes;
    ~Secret(){if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());}
};
std::wstring Extended(const std::wstring& path){if(path.starts_with(L"\\\\?\\"))return path;return path.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;}
HANDLE Pipe(const wchar_t* text){const std::wstring value(text);Require(!value.empty()&&value.size()<=20&&value.find_first_not_of(L"0123456789")==std::wstring::npos,"Signal control handle invalid");
    const auto number=std::stoull(value);Require(number&&number<=UINTPTR_MAX,"Signal control handle range invalid");const auto handle=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(number));
    Require(GetFileType(handle)==FILE_TYPE_PIPE,"Signal control is not a pipe");return handle;}
Handle Directory(const std::wstring& path){Handle handle(CreateFileW(Extended(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
    nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!handle)throw std::runtime_error("Signal state directory open failed Win32="+std::to_string(GetLastError()));BY_HANDLE_FILE_INFORMATION information{};
    Require(GetFileInformationByHandle(handle.value,&information)&&(information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&
        !(information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Signal state directory is not regular");return handle;}
struct State {
    std::wstring path,pin;
    std::array<Handle,3> directories;
    explicit State(const std::wstring& input,const std::wstring& sid){LPWSTR folder=nullptr;const auto status=GetAppContainerFolderPath(sid.c_str(),&folder);
        if(FAILED(status)||!folder)throw std::runtime_error("Signal state profile unavailable HRESULT="+std::to_string(static_cast<unsigned long>(status)));
        struct Folder{LPWSTR value;~Folder(){CoTaskMemFree(value);}} memory{folder};std::wstring root(folder);while(root.size()>3&&root.back()==L'\\')root.pop_back();
        const auto prefix=root+L"\\Signal\\";Require(input.size()==prefix.size()+64&&_wcsnicmp(input.c_str(),prefix.c_str(),prefix.size())==0,"Signal state outside fixed profile namespace");
        pin=input.substr(prefix.size());Require(Hex(pin,64),"Signal state pin invalid");path=prefix+pin;wchar_t absolute[32768]{};const auto size=GetFullPathNameW(path.c_str(),32768,absolute,nullptr);
        Require(size&&size<32768&&_wcsicmp(path.c_str(),absolute)==0,"Signal state path not canonical absolute");
        // These held directory handles disallow replacement while identity/configuration is read.
        directories[0]=Directory(root);directories[1]=Directory(root+L"\\Signal");directories[2]=Directory(path);
    }
    Bytes Read(const wchar_t* leaf)const{Handle file(CreateFileW((Extended(path)+L"\\"+leaf).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file)throw std::runtime_error("Signal state file open failed Win32="+std::to_string(GetLastError()));BY_HANDLE_FILE_INFORMATION information{};
        Require(GetFileInformationByHandle(file.value,&information)&&!(information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
            information.nNumberOfLinks==1&&!information.nFileSizeHigh&&information.nFileSizeLow&&information.nFileSizeLow<=16384,"Signal state file identity or size invalid");
        Bytes bytes(information.nFileSizeLow);DWORD read=0;Require(ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size(),"Signal state file read failed");return bytes;
    }
    void Error(unsigned stage,const std::string& message)const noexcept{try{const auto name=Extended(path)+L"\\node-error.txt";
        Handle file(CreateFileW(name.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file&&GetLastError()==ERROR_FILE_EXISTS)file=Handle(CreateFileW(name.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file)return;BY_HANDLE_FILE_INFORMATION information{};if(!GetFileInformationByHandle(file.value,&information)||
            information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)||information.nNumberOfLinks!=1)return;
        const auto text="PID="+std::to_string(GetCurrentProcessId())+" Stage="+std::to_string(stage)+" "+message.substr(0,384)+"\r\n";
        LARGE_INTEGER zero{};if(text.size()>512||!SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN)||!SetEndOfFile(file.value))return;DWORD written=0;
        if(WriteFile(file.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size())FlushFileBuffers(file.value);
    }catch(...){ /* Diagnostic publication cannot grant a launch or mask the original failure. */ }}
};
void Fields(const JsonObject& object,std::initializer_list<const wchar_t*> names){Require(object.Size()==names.size(),"Signal configuration field count invalid");
    for(const auto& item:object)Require(std::any_of(names.begin(),names.end(),[&](const wchar_t* name){return item.Key()==name;}),"Signal configuration field unknown");}
std::wstring Text(const JsonObject& object,const wchar_t* name,size_t limit){const auto value=object.GetNamedValue(name);Require(value.ValueType()==JsonType::String,"Signal configuration string invalid");
    std::wstring text(value.GetString());Require(text.size()<=limit,"Signal configuration string bound exceeded");return text;}
unsigned Number(const JsonObject& object,const wchar_t* name,unsigned minimum,unsigned maximum){const auto value=object.GetNamedValue(name);Require(value.ValueType()==JsonType::Number,"Signal configuration number invalid");
    const auto number=value.GetNumber();Require(number>=minimum&&number<=maximum&&number==static_cast<unsigned>(number),"Signal configuration number range invalid");return static_cast<unsigned>(number);}
Options Configuration(const State& state){const auto raw=state.Read(L"node-config.json");const auto object=JsonObject::Parse(winrt::to_hstring(std::string(raw.begin(),raw.end())));Fields(object,{L"Version",L"Node",L"Listen",L"Peers"});
    Require(Number(object,L"Version",1,1)==1&&Text(object,L"Node",64)==state.pin,"Signal configuration identity mismatch");const auto listen=object.GetNamedObject(L"Listen");Fields(listen,{L"IP",L"Port"});
    Options options;options.ip=Text(listen,L"IP",15);options.port=static_cast<unsigned short>(Number(listen,L"Port",1,65535));Require(options.ip==L"0.0.0.0"||IPv4(options.ip),"Signal listener address invalid");
    const auto peers=object.GetNamedArray(L"Peers");Require(peers.Size()<=maxNodes,"Signal configured peer bound exceeded");for(const auto& value:peers){Require(value.ValueType()==JsonType::Object,"Signal configured peer type invalid");
        const auto item=value.GetObject();Fields(item,{L"IP",L"Port",L"Pin"});Node peer;peer.ip=Text(item,L"IP",15);peer.port=static_cast<unsigned short>(Number(item,L"Port",1,65535));peer.id=Text(item,L"Pin",64);
        Require(IPv4(peer.ip)&&!peer.ip.starts_with(L"127.")&&Hex(peer.id,64),"Signal configured peer address or pin invalid");options.peers.push_back(std::move(peer));}
    // allowLoopback remains false; there is no executable argument/configuration bypass.
    return options;
}
std::string PublicError(const std::exception& error){const std::string message(error.what());if(!message.starts_with("Signal ")||message.size()>384||
    message.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 =/:-.")!=std::string::npos)return "Signal node failed";return message;}
} // namespace

int wmain(int argc,wchar_t** argv)
{
    // No private persistence, WinRT, TLS or sockets before the token and owner GO gate.
    if(argc!=6||wcscmp(argv[1],L"--signal-node")!=0)return 2;Handle control,result;std::unique_ptr<State> state;unsigned stage=1;
    try{PSID expected=nullptr;const auto status=DeriveAppContainerSidFromAppContainerName(sandbox::productName,&expected);
        struct Sid{PSID value;~Sid(){if(value)FreeSid(value);}} sid{expected};Require(SUCCEEDED(status)&&expected,"Signal product SID unavailable");const auto text=sandbox::SidText(expected);
        Require(text==argv[2]&&sandbox::CheckSignalToken(GetCurrentProcess(),text)&&sandbox::PrivilegesSafe(),"Signal product token refused");control=Handle(Pipe(argv[3]));result=Handle(Pipe(argv[4]));
        Require(control.value!=result.value,"Signal control handles alias");Require(sandbox::WritePacket(result.value,{sandbox::readyPacket,0,GetCurrentProcessId()}),"Signal token READY publication failed");
        const auto command=sandbox::ReadControl(control.value);if(command!='G')return 4;
        std::stop_source stop;std::jthread commands([&](std::stop_token finished){while(!finished.stop_requested()&&!stop.stop_requested()){
            DWORD available=0;if(!PeekNamedPipe(control.value,nullptr,0,nullptr,&available,nullptr)){stop.request_stop();break;}if(available){char next=0;DWORD read=0;
                if(!ReadFile(control.value,&next,1,&read,nullptr)||read!=1||next=='S'||next!='G'){stop.request_stop();break;}}Sleep(50);}});
        stage=2;winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
        Require(!stop.stop_requested(),"Signal startup cancelled");state=std::make_unique<State>(argv[5],text);stage=3;const auto options=Configuration(*state);
        const auto cert=state->Read(L"node-cert.der");Require(CertificateIdentity(cert)==state->pin,"Signal certificate state pin mismatch");Secret key{state->Read(L"node-key.pk8")};
        Require(!stop.stop_requested(),"Signal startup cancelled");stage=4;NodeRuntime node(options,cert,key.bytes);std::wstring error;
        const auto okay=node.Run(stop.get_token(),[&](unsigned short port){Require(!stop.stop_requested()&&port&&sandbox::WritePacket(result.value,{sandbox::resultPacket,port,GetCurrentProcessId()}),"Signal listener READY publication failed");stage=5;},error);
        commands.request_stop();if(!okay){const auto bytes=Ascii(error);throw std::runtime_error(std::string(bytes.begin(),bytes.end()));}return 0;
    }catch(const winrt::hresult_error& error){const auto message="Signal configuration failed HRESULT="+std::to_string(static_cast<unsigned long>(error.code().value));
        if(state)state->Error(stage,message);std::fprintf(stderr,"PID=%lu Stage=%u %s\n",GetCurrentProcessId(),stage,message.c_str());
    }catch(const std::exception& error){const auto message=PublicError(error);if(state)state->Error(stage,message);std::fprintf(stderr,"PID=%lu Stage=%u %s\n",GetCurrentProcessId(),stage,message.c_str());
    }catch(...){if(state)state->Error(stage,"Signal node failed");}
    if(result)sandbox::WritePacket(result.value,{sandbox::resultPacket,0,GetCurrentProcessId()});return 1;
}
