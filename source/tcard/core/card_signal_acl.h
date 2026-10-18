#pragma once
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <stdexcept>

// No profile creation, static Userenv import, store scan or write is performed by SID discovery.
namespace tcard_signal_acl {
inline constexpr wchar_t productName[]=L"TCard.Signaling.v1";
inline std::wstring SidText(PSID sid)
{
    LPWSTR value=nullptr;if(!sid||!IsValidSid(sid)||!ConvertSidToStringSidW(sid,&value))return {};
    std::wstring text=value;LocalFree(value);return text;
}
inline const std::wstring& Identity()
{
    static const std::wstring identity=[](){
        wchar_t path[32768]{};const auto size=GetSystemDirectoryW(path,32768);if(!size||size>=32750)return std::wstring{};
        const auto library=LoadLibraryExW((std::wstring(path)+L"\\userenv.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!library)return std::wstring{};
        // ASCII export name is an API boundary, never a filesystem path or command.
        using Derive=HRESULT(WINAPI*)(PCWSTR,PSID*);const auto derive=reinterpret_cast<Derive>(GetProcAddress(library,"DeriveAppContainerSidFromAppContainerName"));
        PSID sid=nullptr;std::wstring result;if(derive&&SUCCEEDED(derive(productName,&sid)))result=SidText(sid);
        if(sid)FreeSid(sid);FreeLibrary(library);return result;
    }();return identity;
}
inline std::wstring Template(const std::wstring& user)
{
    const auto& identity=Identity();const auto policy=identity.empty()?std::wstring{}:
        L"(D;OICI;FA;;;"+identity+L")(A;OICI;RC;;;OW)";
    return L"O:"+user+L"D:P"+policy+L"(A;OICI;FA;;;SY)(A;OICI;FA;;;"+user+L")";
}
inline bool Read(HANDLE file,std::vector<BYTE>& bytes)
{
    constexpr auto fields=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION;
    DWORD size=0;GetKernelObjectSecurity(file,fields,nullptr,0,&size);if(!size||size>65536)return false;bytes.resize(size);
    if(!GetKernelObjectSecurity(file,fields,reinterpret_cast<PSECURITY_DESCRIPTOR>(bytes.data()),size,&size)){bytes.clear();return false;}bytes.resize(size);return true;
}
inline bool PackagePolicy(const std::vector<BYTE>& bytes)
{
    if(bytes.empty())return false;PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    if(!GetSecurityDescriptorDacl(const_cast<BYTE*>(bytes.data()),&present,&acl,&defaulted)||!present||!acl)return false;
    for(DWORD i=0;i<acl->AceCount;++i){void* entry=nullptr;if(!GetAce(acl,i,&entry))return true;const auto header=static_cast<ACE_HEADER*>(entry);
        if(header->AceType==ACCESS_DENIED_ACE_TYPE){const auto denied=static_cast<ACCESS_DENIED_ACE*>(entry);const auto sid=SidText(reinterpret_cast<PSID>(&denied->SidStart));if(sid.starts_with(L"S-1-15-"))return true;}
        else if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)return true; // Unknown ACEs cannot justify legacy overwrite.
    }
    return false;
}
inline bool Owner(HANDLE file,PSID user)
{
    std::vector<BYTE> bytes;if(!Read(file,bytes))return false;PSID owner=nullptr;BOOL defaulted=FALSE;
    return GetSecurityDescriptorOwner(bytes.data(),&owner,&defaulted)&&owner&&EqualSid(owner,user);
}
inline HANDLE Open(const std::wstring& path,DWORD access=READ_CONTROL|FILE_READ_ATTRIBUTES)
{
    const auto file=CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(file==INVALID_HANDLE_VALUE)return file;
    FILE_ATTRIBUTE_TAG_INFO info{};if(!GetFileInformationByHandleEx(file,FileAttributeTagInfo,&info,sizeof(info))||info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){CloseHandle(file);return INVALID_HANDLE_VALUE;}return file;
}
// Missing optional APIs must not erase a previously installed package deny.
inline bool PreserveUnavailable(const std::wstring& directory,PSID user,bool& handled)
{
    handled=false;if(!Identity().empty())return true;
    HANDLE file=Open(directory);const bool exists=file!=INVALID_HANDLE_VALUE;std::vector<BYTE> bytes;
    if(exists){const bool read=Read(file,bytes),owner=Owner(file,user);CloseHandle(file);if(!read||!owner){handled=true;return false;}
        if(PackagePolicy(bytes)){handled=true;return false;}}
    const auto slash=directory.find_last_of(L'\\');if(slash==std::wstring::npos){handled=true;return false;}
    file=Open(directory.substr(0,slash));if(file==INVALID_HANDLE_VALUE){handled=true;return false;}
    const bool read=Read(file,bytes);CloseHandle(file);if(!read){handled=true;return false;}if(!PackagePolicy(bytes))return true;
    // Without the expected SID there is no proof that a new protected child preserves installed policy.
    handled=true;return false;
}
inline bool Merge(const std::vector<BYTE>& old,PACL& merged)
{
    merged=nullptr;const auto& identity=Identity();if(identity.empty()||old.empty())return false;PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    if(!GetSecurityDescriptorDacl(const_cast<BYTE*>(old.data()),&present,&acl,&defaulted)||!present||!acl)return false;
    PSID package=nullptr,ownerRights=nullptr;if(!ConvertStringSidToSidW(identity.c_str(),&package))return false;
    if(!ConvertStringSidToSidW(L"S-1-3-4",&ownerRights)){LocalFree(package);return false;}
    EXPLICIT_ACCESSW rules[2]{};rules[0].grfAccessPermissions=FILE_ALL_ACCESS;rules[0].grfAccessMode=DENY_ACCESS;
    rules[1].grfAccessPermissions=READ_CONTROL;rules[1].grfAccessMode=SET_ACCESS;
    for(unsigned i=0;i<2;++i){rules[i].grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;rules[i].Trustee.TrusteeForm=TRUSTEE_IS_SID;rules[i].Trustee.TrusteeType=TRUSTEE_IS_UNKNOWN;}
    rules[0].Trustee.ptstrName=static_cast<LPWSTR>(package);rules[1].Trustee.ptstrName=static_cast<LPWSTR>(ownerRights);
    const auto result=SetEntriesInAclW(2,rules,acl,&merged);LocalFree(package);LocalFree(ownerRights);return result==ERROR_SUCCESS;
}
inline bool Apply(HANDLE file)
{
    std::vector<BYTE> old;if(!Read(file,old))return false;PACL acl=nullptr;if(!Merge(old,acl))return false;
    const auto result=SetSecurityInfo(file,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);LocalFree(acl);return result==ERROR_SUCCESS;
}
}
