#pragma once
#include "card_native_root.h"
#include "card_key_activation.h"
#include "card_signal_acl.h"
#include <stop_token>
#include <set>
#include <array>
#include <cstring>

// Explicit, quiesced admission only. Ordinary writers maintain the bridge ACL template.
// The caller initializes its WinRT apartment and stops all secret-bearing child handles.
namespace tcard_signal_protection {
using namespace tcard_sync;
// Conservative admission ceiling across all three trees; larger stores remain offline/unsupported.
constexpr size_t maxFiles=10000,maxVisited=2*maxFiles,maxDepth=32,maxRecord=65536;
inline void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline void Api(bool value,const char* message)
{
    if(!value){const auto error=GetLastError();throw std::runtime_error(std::string(message)+" Win32="+std::to_string(error));}
}
inline void Cancel(std::stop_token stop){Require(!stop.stop_requested(),"Signal protection cancelled");}
inline bool Exists(const std::wstring& path)
{
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return true;const auto error=GetLastError();
    Require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"Signal protection path unavailable");return false;
}
inline std::wstring Canonical(const std::wstring& path)
{
    Require(!path.empty(),"Signal protection root missing");const auto plain=sync_path_plain(path);
    Require(plain.size()>3&&plain.size()<32700,"Signal protection root invalid");
    // Validate every existing ancestor, not just the final directory.
    auto part=plain;for(unsigned depth=0;depth<64;++depth){tcard_asset::File directory(tcard_asset::open_dir(tcard_native::Extended(part),false));
        Require(bool(directory),"Signal protection root alias or reparse point");const auto at=part.find_last_of(L'\\');if(at==std::wstring::npos||at<3)break;part.resize(at);
        Require(depth<63,"Signal protection ancestor bound exceeded");}
    return plain;
}
inline std::wstring User()
{
    HANDLE raw=nullptr;Require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)!=FALSE,"Signal protection user unavailable");
    tcard_asset::File token(raw);DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);Require(size&&size<65536,"Signal protection user size invalid");
    std::vector<BYTE> bytes(size);Require(GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size)!=FALSE,"Signal protection user unreadable");
    const auto value=tcard_signal_acl::SidText(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);Require(!value.empty(),"Signal protection user invalid");return value;
}
inline std::wstring FileId(HANDLE file,bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};Require(GetFileInformationByHandle(file,&info)!=FALSE&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
        bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory&&(directory||info.nNumberOfLinks==1),"Signal protection object unsafe");
    return std::to_wstring(info.dwVolumeSerialNumber)+L":"+std::to_wstring(info.nFileIndexHigh)+L":"+std::to_wstring(info.nFileIndexLow);
}
inline std::wstring Hex(const std::vector<BYTE>& bytes)
{
    constexpr wchar_t digits[]=L"0123456789abcdef";std::wstring text;text.reserve(bytes.size()*2);
    for(const auto byte:bytes){text.push_back(digits[byte>>4]);text.push_back(digits[byte&15]);}return text;
}
inline std::vector<BYTE> Unhex(const std::wstring& text)
{
    Require(!text.empty()&&text.size()<=maxRecord&&!(text.size()%2),"Signal protection descriptor size invalid");std::vector<BYTE> bytes;
    const auto digit=[](wchar_t c)->unsigned{if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;throw std::runtime_error("Signal protection descriptor encoding invalid");};
    for(size_t i=0;i<text.size();i+=2)bytes.push_back(static_cast<BYTE>((digit(text[i])<<4)|digit(text[i+1])));
    Require(IsValidSecurityDescriptor(bytes.data())!=FALSE,"Signal protection descriptor invalid");return bytes;
}
inline bool EqualSd(const std::vector<BYTE>& left,const std::vector<BYTE>& right)
{
    if(left.empty()||right.empty())return false;const auto equalSid=[](PSID a,PSID b){return a&&b&&EqualSid(a,b);};
    PSID a=nullptr,b=nullptr;BOOL x=FALSE,y=FALSE;
    if(!GetSecurityDescriptorOwner(const_cast<BYTE*>(left.data()),&a,&x)||!GetSecurityDescriptorOwner(const_cast<BYTE*>(right.data()),&b,&y)||!equalSid(a,b))return false;
    if(!GetSecurityDescriptorGroup(const_cast<BYTE*>(left.data()),&a,&x)||!GetSecurityDescriptorGroup(const_cast<BYTE*>(right.data()),&b,&y)||!equalSid(a,b))return false;
    PACL aa=nullptr,bb=nullptr;BOOL ap=FALSE,bp=FALSE;
    if(!GetSecurityDescriptorDacl(const_cast<BYTE*>(left.data()),&ap,&aa,&x)||!GetSecurityDescriptorDacl(const_cast<BYTE*>(right.data()),&bp,&bb,&y)||!ap||!bp||!aa||!bb||aa->AclSize!=bb->AclSize||memcmp(aa,bb,aa->AclSize))return false;
    SECURITY_DESCRIPTOR_CONTROL ac{},bc{};DWORD revision=0;
    return GetSecurityDescriptorControl(const_cast<BYTE*>(left.data()),&ac,&revision)&&GetSecurityDescriptorControl(const_cast<BYTE*>(right.data()),&bc,&revision)&&
        (ac&SE_DACL_PROTECTED)==(bc&SE_DACL_PROTECTED);
}
inline bool Supported(const std::vector<BYTE>& bytes,const std::wstring& user,bool& installed,bool directory=false)
{
    installed=false;if(bytes.empty()||!IsValidSecurityDescriptor(const_cast<BYTE*>(bytes.data())))return false;
    PSID owner=nullptr;BOOL def=FALSE;PACL acl=nullptr;BOOL present=FALSE;
    if(!GetSecurityDescriptorOwner(const_cast<BYTE*>(bytes.data()),&owner,&def)||tcard_signal_acl::SidText(owner)!=user||
        !GetSecurityDescriptorDacl(const_cast<BYTE*>(bytes.data()),&present,&acl,&def)||!present||!acl)return false;
    bool system=false,current=false,denied=false,rights=false,allowed=false;const auto& package=tcard_signal_acl::Identity();
    for(DWORD i=0;i<acl->AceCount;++i){void* entry=nullptr;if(!GetAce(acl,i,&entry))return false;const auto head=static_cast<ACE_HEADER*>(entry);
        if(head->AceFlags&~(OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE|INHERITED_ACE))return false;
        if(head->AceType!=ACCESS_ALLOWED_ACE_TYPE&&head->AceType!=ACCESS_DENIED_ACE_TYPE)return false;
        const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(entry);const auto sid=tcard_signal_acl::SidText(reinterpret_cast<PSID>(&ace->SidStart));
        if(head->AceType==ACCESS_DENIED_ACE_TYPE){if(allowed||sid!=package||ace->Mask!=FILE_ALL_ACCESS||denied||(directory&&(head->AceFlags&3)!=3))return false;denied=true;}
        else if(sid==L"S-1-3-4"){allowed=true;if(ace->Mask!=READ_CONTROL||rights||(directory&&(head->AceFlags&3)!=3))return false;rights=true;}
        else if(sid==L"S-1-5-18"){allowed=true;if(ace->Mask!=FILE_ALL_ACCESS||(directory&&(head->AceFlags&3)!=3))return false;system=true;}
        else if(sid==user){allowed=true;if(ace->Mask!=FILE_ALL_ACCESS||(directory&&(head->AceFlags&3)!=3))return false;current=true;}else return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;if(!GetSecurityDescriptorControl(const_cast<BYTE*>(bytes.data()),&control,&revision))return false;
    if(denied!=rights)return false;installed=denied&&rights;return system&&current;
}
inline bool Protected(const std::vector<BYTE>& bytes)
{
    SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;return !bytes.empty()&&GetSecurityDescriptorControl(const_cast<BYTE*>(bytes.data()),&control,&revision)&&(control&SE_DACL_PROTECTED);
}
inline std::vector<BYTE> Target(const std::vector<BYTE>& old,const std::wstring& user,bool directory)
{
    bool installed=false;Require(Supported(old,user,installed,directory),"Signal protection custom or mixed ACL unsupported");if(installed&&Protected(old))return old;
    PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    if(installed)Require(GetSecurityDescriptorDacl(const_cast<BYTE*>(old.data()),&present,&acl,&defaulted)&&present&&acl,"Signal existing policy unreadable");
    else Require(tcard_signal_acl::Merge(old,acl),"Signal protection ACL merge failed");
    struct Free{PACL value;bool owned;~Free(){if(owned)LocalFree(value);}} free{acl,!installed};
    // Windows makes inherited ACEs explicit when installing a protected DACL. Originals stay untouched.
    std::vector<BYTE> explicitBytes(acl->AclSize);memcpy(explicitBytes.data(),acl,acl->AclSize);
    const auto explicitAcl=reinterpret_cast<PACL>(explicitBytes.data());
    const auto normalized=INHERITED_ACE|(directory?0:OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE);
    for(DWORD i=0;i<explicitAcl->AceCount;++i){void* entry=nullptr;Require(GetAce(explicitAcl,i,&entry)!=FALSE,"Signal target ACE unavailable");
        static_cast<ACE_HEADER*>(entry)->AceFlags&=static_cast<BYTE>(~normalized);}
    PSID owner=nullptr,group=nullptr;BOOL def=FALSE;Require(GetSecurityDescriptorOwner(const_cast<BYTE*>(old.data()),&owner,&def)&&GetSecurityDescriptorGroup(const_cast<BYTE*>(old.data()),&group,&def),"Signal protection SD identity invalid");
    SECURITY_DESCRIPTOR absolute{};Require(InitializeSecurityDescriptor(&absolute,SECURITY_DESCRIPTOR_REVISION)&&SetSecurityDescriptorOwner(&absolute,owner,FALSE)&&
        SetSecurityDescriptorGroup(&absolute,group,FALSE)&&SetSecurityDescriptorDacl(&absolute,TRUE,explicitAcl,FALSE)&&SetSecurityDescriptorControl(&absolute,SE_DACL_PROTECTED,SE_DACL_PROTECTED),"Signal protection target SD invalid");
    DWORD size=0;MakeSelfRelativeSD(&absolute,nullptr,&size);Require(size&&size<=32768,"Signal protection target SD too large");std::vector<BYTE> result(size);
    Require(MakeSelfRelativeSD(&absolute,result.data(),&size)!=FALSE,"Signal protection target serialization failed");result.resize(size);return result;
}
inline bool Guid(const std::wstring& text)
{
    GUID value{};return text.size()==36&&SUCCEEDED(CLSIDFromString((L"{"+text+L"}").c_str(),&value));
}
inline bool Note(const std::wstring& text){return tcard_asset::leaf(text)&&text.size()<=200&&text[0]!=L'_'&&text[0]!=L'.'&&sync_stem(text)==text;}
inline bool HashFile(const std::wstring& text,const wchar_t* suffix){const std::wstring end=suffix;return text.size()==64+end.size()&&text.ends_with(end)&&sync_hex(text.substr(0,64));}
inline bool OneOf(const std::wstring& text,std::initializer_list<const wchar_t*> names){return std::any_of(names.begin(),names.end(),[&](const wchar_t* name){return text==name;});}
enum class Kind{Store,Profile,Groups,Generations,Generation,Stage,Retained,Note,TrashNote,Images,Journal,JournalNote,Trash,Mail,Hashes,Records,Objects,Object,Pools,Pool,References,Reference,Control,Delivery,KeyCatalog,KeyTransaction,KeyEngine,KeyCopies,KeyPublications,KeyMirror};
inline bool Child(Kind parent,const std::wstring& name,bool dir,Kind& next)
{
    next=parent;if(!tcard_asset::leaf(name))return false;
    if(parent==Kind::Control){if(dir&&name==L"_key-transactions"){next=Kind::KeyCatalog;return true;}
        return !dir&&(name==L"key-activation.dpapi"||(name.ends_with(L".request")&&Guid(name.substr(0,name.size()-8)))||(name.ends_with(L".response")&&Guid(name.substr(0,name.size()-9))));}
    // Exact activation/engine producer grammar; retained mirrors use the existing private tree schema.
    if(parent==Kind::KeyCatalog){next=Kind::KeyTransaction;return dir&&Guid(name);}
    if(parent==Kind::KeyTransaction){next=Kind::KeyEngine;return dir?name==L"engine":name==L"activation.dpapi";}
    if(parent==Kind::KeyEngine){if(!dir)return name==L"transition.dpapi";
        if(name==L"publications"){next=Kind::KeyPublications;return true;}next=Kind::KeyCopies;return name==L"root-0"||name==L"root-1";}
    if(parent==Kind::KeyCopies){next=Kind::KeyMirror;return dir&&(name==L"candidate"||name==L"recovery");}
    if(parent==Kind::KeyPublications)return !dir&&HashFile(name,L".dpapi");
    // Delivery metadata is private; accept only the actual journal producer grammar.
    if(parent==Kind::Delivery)return !dir&&(name==L"node.json"||(name.size()==47&&name.starts_with(L"peers-")&&name.ends_with(L".json")&&Guid(name.substr(6,36))));
    if(parent==Kind::Groups){next=Kind::Store;return dir&&Guid(name);}
    if(parent==Kind::Generations){next=Kind::Generation;return dir?sync_hex(name):name==L"active.json";}
    if(parent==Kind::Generation){next=Kind::Store;return dir&&name==L"data";}
    if(parent==Kind::Retained){next=Kind::Stage;return dir&&name.size()==38&&name.front()==L'{'&&name.back()==L'}'&&Guid(name.substr(1,36));}
    if(parent==Kind::Stage){next=Kind::Store;return dir?name==L"data":OneOf(name,{L"target.json",L"native.json",L"native-ready.json",L"activation.json"});}
    if(parent==Kind::Images)return !dir&&(HashFile(name,L".png")||HashFile(name,L".jpg")||HashFile(name,L".gif")||HashFile(name,L".bmp"));
    if(parent==Kind::Note||parent==Kind::TrashNote){next=Kind::Images;return dir?name==L"images":name==L"card.json"||tcard_asset::relative(name,false);}
    if(parent==Kind::Trash){next=Kind::TrashNote;return dir&&name.size()==38&&name.front()==L'{'&&name.back()==L'}'&&Guid(name.substr(1,36));}
    if(parent==Kind::Journal){next=Kind::JournalNote;return dir?Note(name):OneOf(name,{L"actor.json",L"credentials.json",L"space.json",L"peers.json",L"node.json",L"pending.json"});}
    if(parent==Kind::JournalNote){next=Kind::Images;return dir?name==L"images":HashFile(name,L".json")||tcard_asset::relative(name,false)||name==L"pending.json";}
    if(parent==Kind::Hashes)return !dir&&HashFile(name,L".json");
    if(parent==Kind::Records)return !dir&&(HashFile(name,L".json")||OneOf(name,{L"state.json",L"required.json"}));
    if(parent==Kind::References){next=Kind::Reference;return dir&&sync_hex(name);}
    if(parent==Kind::Reference){next=Kind::Objects;return dir?name==L"_objects":name==L"reference.json";}
    if(parent==Kind::Objects){next=Kind::Object;return dir?sync_hex(name):name==L"budget.json";}
    if(parent==Kind::Object){if(dir)return false;if(OneOf(name,{L"manifest.json",L"reference.json"}))return true;
        if(!name.ends_with(L".part"))return false;const auto index=name.substr(0,name.size()-5);return !index.empty()&&index.size()<=2&&index.find_first_not_of(L"0123456789")==std::wstring::npos&&std::stoul(index)<64&&std::to_wstring(std::stoul(index))==index;}
    if(parent==Kind::Pools){next=Kind::Pool;return dir&&sync_hex(name);}
    if(parent==Kind::Pool){next=Kind::Objects;return dir?OneOf(name,{L"_objects",L"_sealed"}):name==L"binding.json";}
    // Source witness files are encrypted, process/nonce-bound producers in the existing private mailbox.
    if(parent==Kind::Mail){if(!dir)return OneOf(name,{L"incoming.json",L"outgoing.json",L"incoming-commit.json",L"outgoing-commit.json",L"incoming-custody.json",L"applied.json",L"status.json",L"check-request.json",L"check-result.json",L"source-query.json",L"source-request.json",L"source-owner.json",L"source-result.json"});
        if(OneOf(name,{L"incoming-objects",L"outgoing-objects"})){next=Kind::Store;return true;}}
    if(parent==Kind::Profile){if(!dir&&OneOf(name,{L"active.json",L"sync-profile.json",L"defaults.json",L"signal-node-cache.json",L"peer-cache.json",L"peer-hints.json",L"connection-log.json"}))return true;
        if(dir&&name==L"groups"){next=Kind::Groups;return true;}if(dir&&name==L"replica"){next=Kind::Store;return true;}}
    if(parent!=Kind::Store&&parent!=Kind::Profile&&parent!=Kind::Mail)return false;
    // A replacement data-scope marker belongs only to a concrete native store.
    if(!dir)return (parent==Kind::Store&&name==L".tcard-data.scope")||OneOf(name,{L".tcard-vault.key",L".tcard-vault.required",L".tcard-native.required"})||(name.ends_with(L".deleted")&&Note(name.substr(0,name.size()-8)));
    if(name==L"_sync")next=Kind::Journal;else if(name==L"_trash")next=Kind::Trash;else if(name==L"_native-generations")next=Kind::Generations;
    else if(name==L"_bootstrap-stage")next=Kind::Stage;else if(name==L"_bootstrap-retained")next=Kind::Retained;else if(name==L"_bootstrap")next=Kind::Hashes;
    else if(OneOf(name,{L"_bootstrap-source",L"_bootstrap-receive"}))next=Kind::Records;else if(name==L"ui")next=Kind::Mail;
    else if(OneOf(name,{L"_transfer",L"_objects",L"_sealed"}))next=Kind::Objects;else if(name==L"_object-transfers")next=Kind::References;
    else if(name==L"_dependency-custody")next=Kind::Pools;else if(parent==Kind::Store&&name==L"_delivery")next=Kind::Delivery;
    else if(parent==Kind::Store&&Note(name))next=Kind::Note;else return false;return true;
}
// Existing native metadata schema, without loading note/history bodies or hashing retained assets.
inline void NoteMetadata(const std::wstring& path,bool trash)
{
    JsonObject value;Require(sync_read(path,L"card.json",value)&&value.GetNamedNumber(L"StorageVersion",0)==1,"Signal native note schema unsupported");
    const auto id=sync_string(value,L"Id"),stem=sync_stem(id),source=sync_string(value,L"SourceFile");
    Require(!id.empty()&&id.size()<=200&&Note(stem)&&tcard_asset::relative(source,false),"Signal native note identity invalid");
    if(!trash)Require(stem==path.substr(path.find_last_of(L'\\')+1),"Signal native note directory identity differs");
    if(value.HasKey(L"History")){const auto history=value.GetNamedArray(L"History");Require(history.Size()<=maxFiles,"Signal native history metadata exceeded");
        for(const auto& item:history)Require(tcard_asset::relative(sync_string(item.GetObject(),L"SourceFile"),false),"Signal native history source invalid");}
}
struct Object{std::wstring path,relative,id,leaf;unsigned role=0,depth=0;bool directory=false;};
struct Layout{std::array<std::wstring,3> roots;std::wstring metadata,user,binding;};
inline Layout Roots(const std::wstring& native,const std::wstring& profile)
{
    Require(!tcard_signal_acl::Identity().empty(),"Signal AppContainer API unavailable");Layout value;
    value.roots={Canonical(tcard_native::Owner(native)),Canonical(profile),Canonical(profile+L"-control")};value.user=User();
    Require(!tcard_activation::Pending(profile),"Signal admission requires completed key activation");
    Require(value.roots[0]==sync_path_plain(tcard_native::Base(native)),"Signal native root must be its stable owner");
    for(size_t i=0;i<3;++i)for(size_t j=i+1;j<3;++j)Require(value.roots[i]!=value.roots[j]&&!value.roots[i].starts_with(value.roots[j]+L"\\")&&!value.roots[j].starts_with(value.roots[i]+L"\\"),"Signal protection roots overlap");
    for(size_t i=0;i<3;++i){auto parent=value.roots[i];for(unsigned depth=0;depth<64;++depth){const auto at=parent.find_last_of(L'\\');if(at==std::wstring::npos||at<3)break;parent.resize(at);
        for(const auto* marker:{L".tcard-vault.key",L".tcard-vault.required",L".tcard-vault.pending",L".tcard-vault.stage",L".tcard-vault.transition"})Require(!Exists(tcard_native::Extended(parent)+L"\\"+marker),"Signal secret key ancestor outside supported root");}
        for(const auto* marker:{L".tcard-vault.pending",L".tcard-vault.stage",L".tcard-vault.transition"})Require(!Exists(tcard_native::Extended(value.roots[i])+L"\\"+marker),"Signal encryption migration pending");
        Require(i==2||tcard_storage::Required(value.roots[i])&&tcard_storage::Ready(value.roots[i]),"Signal encrypted private root unavailable");
        if(i==2)Require(!Exists(tcard_native::Extended(value.roots[i])+L"\\.tcard-vault.key")&&!Exists(tcard_native::Extended(value.roots[i])+L"\\.tcard-vault.required"),"Signal control metadata cannot enter a vault");
        tcard_asset::File handle(tcard_signal_acl::Open(tcard_native::Extended(value.roots[i])));Require(bool(handle),"Signal protection root unreadable");
        value.binding+=std::to_wstring(i)+L":"+value.roots[i]+L":"+FileId(handle.value,true)+L"\n";}
    JsonObject marker;Require(sync_read(value.roots[1],L"sync-profile.json",marker)&&marker.GetNamedNumber(L"Version",0)==1&&sync_string(marker,L"Role")==L"TCardSync.Replica","Signal worker profile identity invalid");
    // Validate the existing active-group schema independently of arbitrary directory names.
    tcard_bridge::Storage(value.roots[1]);value.binding=tcard_bridge::Hash(tcard_signal_acl::Identity()+L"\n"+value.binding);
    value.metadata=tcard_native::Extended(value.roots[2])+L"\\_signal-protection";return value;
}
inline void Trace(unsigned role,const std::wstring& relative,std::wstring& context)
{
    auto bounded=relative.substr(0,256);if(!bounded.empty()&&bounded.back()>=0xd800&&bounded.back()<=0xdbff)bounded.pop_back();
    for(auto& c:bounded)if(c<32)c=L'?';context=L" role="+std::to_wstring(role)+L" relative="+bounded;
}
// Resolve each retained mirror from the authenticated engine journal, not ambiguous directory names.
inline Kind signal_resolve_mirror(const Layout& layout,const std::wstring& path)
{
    const auto copy=path.substr(0,path.find_last_of(L'\\')),engine=copy.substr(0,copy.find_last_of(L'\\'));
    const auto slot=copy.substr(copy.find_last_of(L'\\')+1);Require(slot==L"root-0"||slot==L"root-1","Signal activation mirror slot invalid");
    std::wstring sealed,plain;Require(tcard_bridge::Read(engine,L"transition.dpapi",sealed)&&tcard_bridge::Seal(sealed,plain,true),"Signal activation mirror journal unavailable");
    const auto record=JsonObject::Parse(plain);SecureZeroMemory(plain.data(),plain.size()*sizeof(wchar_t));
    Require(record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Path")==sync_path_plain(engine)&&sync_string(record,L"Mode")==L"rotate","Signal activation mirror journal identity differs");
    const auto roots=record.GetNamedArray(L"Roots");Require(roots.Size()==2,"Signal activation mirror root count invalid");
    const auto first=sync_string(roots.GetObjectAt(0),L"Path"),second=sync_string(roots.GetObjectAt(1),L"Path");
    Require(first<second&&((first==layout.roots[0]&&second==layout.roots[1])||(first==layout.roots[1]&&second==layout.roots[0])),"Signal activation mirror owner differs");
    const auto source=slot==L"root-0"?first:second;return source==layout.roots[0]?Kind::Store:Kind::Profile;
}
inline void Walk(const Layout& layout,unsigned role,const std::wstring& relative,Kind kind,unsigned depth,
    std::vector<Object>& objects,size_t& files,std::stop_token stop,std::wstring& context)
{
    Trace(role,relative,context);
    Cancel(stop);Require(depth<=maxDepth&&objects.size()<maxVisited,"Signal private inventory bound exceeded");
    const auto path=tcard_native::Extended(layout.roots[role])+(relative.empty()?L"":L"\\"+relative);
    tcard_asset::File handle(tcard_signal_acl::Open(path));Require(bool(handle),"Signal private directory unavailable");std::vector<BYTE> sd;bool installed=false;
    Require(tcard_signal_acl::Read(handle.value,sd)&&Supported(sd,layout.user,installed,true),"Signal private directory ACL unsupported");
    if(kind==Kind::KeyMirror){Require(role==2,"Signal activation mirror role invalid");kind=signal_resolve_mirror(layout,path);}
    if(kind==Kind::Note||kind==Kind::TrashNote)NoteMetadata(path,kind==Kind::TrashNote);
    objects.push_back({path,relative,FileId(handle.value,true),tcard_bridge::Hash(std::to_wstring(role)+L":"+relative)+L".sealed",role,depth,true});
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((path+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){Require(GetLastError()==ERROR_FILE_NOT_FOUND,"Signal private enumeration failed");return;}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;Cancel(stop);
        if(role==2&&relative.empty()&&name==L"_signal-protection")continue;
        const auto rel=relative.empty()?name:relative+L"\\"+name;Trace(role,rel,context);
        Require(!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Signal private reparse point refused");const bool directory=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;Kind child{};
        Require(Child(kind,name,directory,child),"Signal private namespace unsupported");
        if(directory)Walk(layout,role,rel,child,depth+1,objects,files,stop,context);
        else{Require(++files<=maxFiles&&objects.size()<maxVisited,"Signal private inventory bound exceeded");tcard_asset::File file(tcard_signal_acl::Open(path+L"\\"+name));
            Require(bool(file)&&tcard_signal_acl::Read(file.value,sd)&&Supported(sd,layout.user,installed),"Signal private file ACL unsupported");
            objects.push_back({path+L"\\"+name,rel,FileId(file.value,false),tcard_bridge::Hash(std::to_wstring(role)+L":"+rel)+L".sealed",role,depth+1,false});}
    }while(FindNextFileW(find,&entry));Require(GetLastError()==ERROR_NO_MORE_FILES,"Signal private enumeration interrupted");
}
// Metadata is DPAPI-sealed but deliberately not vault-packed: key counters and payloads are unchanged.
// A torn/corrupt .pending publication is retained and blocks admission; no unverifiable erasure/recovery is attempted.
inline bool RawRead(const std::wstring& path,std::wstring& plain)
{
    if(!Exists(path))return false;tcard_asset::File file(tcard_signal_acl::Open(path,GENERIC_READ|READ_CONTROL|FILE_READ_ATTRIBUTES));Require(bool(file),"Signal ACL backup unavailable");
    FileId(file.value,false);LARGE_INTEGER size{};Require(GetFileSizeEx(file.value,&size)&&size.QuadPart>0&&size.QuadPart<=maxRecord,"Signal ACL backup size invalid");
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));DWORD got=0;Require(ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr)&&got==bytes.size(),"Signal ACL backup read failed");
    const auto text=tcard_asset::decode(bytes);Require(tcard_asset::encode(text)==bytes&&tcard_bridge::Seal(text,plain,true),"Signal ACL backup authentication failed");return true;
}
inline void Publish(const Layout& layout,const std::wstring& leaf,const std::wstring& plain)
{
    const auto path=layout.metadata+L"\\"+leaf,pending=path+L".pending";std::wstring old,part;
    if(RawRead(path,old)&&old==plain){if(RawRead(pending,part)){Require(part==plain,"Signal ACL publication intent differs");Require(DeleteFileW(pending.c_str())!=FALSE,"Signal ACL intent cleanup failed");}return;}
    if(RawRead(pending,part))Require(part==plain,"Signal ACL publication intent differs");
    else{std::wstring sealed;Require(tcard_bridge::Seal(plain,sealed)&&sealed.size()<=maxRecord,"Signal ACL backup DPAPI size exceeded");const auto bytes=tcard_asset::encode(sealed);
        tcard_asset::File file(CreateFileW(pending.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        Api(bool(file),"Signal ACL backup creation failed");FileId(file.value,false);DWORD written=0;Require(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file.value),"Signal ACL backup flush failed");}
    Require(RawRead(pending,part)&&part==plain,"Signal ACL backup readback failed");
    Api(MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Signal ACL backup publication failed");
    Require(RawRead(path,old)&&old==plain,"Signal ACL published backup differs");
}
inline JsonObject Summary(const Layout& layout,const std::wstring& phase,const std::wstring& digest,size_t count)
{
    JsonObject value;value.Insert(L"Version",JsonValue::CreateNumberValue(1));value.Insert(L"SID",JsonValue::CreateStringValue(tcard_signal_acl::Identity()));
    value.Insert(L"Binding",JsonValue::CreateStringValue(layout.binding));value.Insert(L"Phase",JsonValue::CreateStringValue(phase));
    value.Insert(L"Digest",JsonValue::CreateStringValue(digest));value.Insert(L"Count",JsonValue::CreateNumberValue(static_cast<double>(count)));return value;
}
inline JsonObject ReadSummary(const Layout& layout)
{
    std::wstring text;Require(RawRead(layout.metadata+L"\\admission.sealed",text),"Signal protection admission missing");const auto value=JsonObject::Parse(text);
    Require(value.Size()==6&&value.GetNamedNumber(L"Version",0)==1&&sync_string(value,L"SID")==tcard_signal_acl::Identity()&&sync_string(value,L"Binding")==layout.binding,"Signal protection admission identity differs");
    const auto count=value.GetNamedNumber(L"Count",-1);Require(std::isfinite(count)&&count>=0&&count<=maxVisited&&count==std::floor(count),"Signal protection admission count invalid");return value;
}
inline std::set<std::wstring> Metadata(const Layout& layout)
{
    std::set<std::wstring> leaves;
    if(!Exists(layout.metadata))Require(tcard_bridge::Protect(layout.metadata),"Signal ACL backup directory protection failed");
    tcard_asset::File directory(tcard_signal_acl::Open(layout.metadata));std::vector<BYTE> bytes;bool installed=false;
    Require(bool(directory)&&tcard_signal_acl::Read(directory.value,bytes)&&Supported(bytes,layout.user,installed,true)&&installed&&Protected(bytes),"Signal ACL backup directory policy invalid");
    // Unknown files are retained, never adopted, erased or interpreted as publication intent.
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((layout.metadata+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){Require(GetLastError()==ERROR_FILE_NOT_FOUND,"Signal ACL backup enumeration failed");return leaves;}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};size_t count=0;
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;Require(++count<=2*(maxVisited+1)&&!(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Signal ACL backup namespace invalid");
        const auto leaf=name.ends_with(L".pending")?name.substr(0,name.size()-8):name;Require(leaf==L"admission.sealed"||HashFile(leaf,L".sealed"),"Signal ACL backup contains unknown file");leaves.insert(leaf);
    }while(FindNextFileW(find,&entry));Require(GetLastError()==ERROR_NO_MORE_FILES,"Signal ACL backup enumeration interrupted");return leaves;
}
inline std::wstring Record(const Layout& layout,const Object& object,const std::vector<BYTE>& old,const std::vector<BYTE>& target)
{
    JsonObject value;value.Insert(L"Version",JsonValue::CreateNumberValue(1));value.Insert(L"Binding",JsonValue::CreateStringValue(layout.binding));
    value.Insert(L"Role",JsonValue::CreateNumberValue(object.role));value.Insert(L"Path",JsonValue::CreateStringValue(object.relative));value.Insert(L"Identity",JsonValue::CreateStringValue(object.id));
    value.Insert(L"Directory",JsonValue::CreateBooleanValue(object.directory));value.Insert(L"Old",JsonValue::CreateStringValue(Hex(old)));value.Insert(L"New",JsonValue::CreateStringValue(Hex(target)));return std::wstring(value.Stringify());
}
inline void Backup(const Layout& layout,const Object& object,const std::vector<BYTE>& current,std::vector<BYTE>& old,std::vector<BYTE>& target,bool allowCreate)
{
    std::wstring text;if(!RawRead(layout.metadata+L"\\"+object.leaf,text)){
        Require(allowCreate,"Signal original ACL backup missing");old=current;target=Target(old,layout.user,object.directory);text=Record(layout,object,old,target);Publish(layout,object.leaf,text);return;}
    const auto value=JsonObject::Parse(text);Require(value.Size()==8&&value.GetNamedNumber(L"Version",0)==1&&sync_string(value,L"Binding")==layout.binding&&value.GetNamedNumber(L"Role",-1)==object.role&&
        sync_string(value,L"Path")==object.relative&&sync_string(value,L"Identity")==object.id&&value.GetNamedBoolean(L"Directory") ==object.directory,"Signal original ACL backup identity differs");
    old=Unhex(sync_string(value,L"Old"));target=Unhex(sync_string(value,L"New"));Require(EqualSd(Target(old,layout.user,object.directory),target)&&(EqualSd(current,old)||EqualSd(current,target)),"Signal private ACL drift or backup mismatch");
    std::wstring pending;if(RawRead(layout.metadata+L"\\"+object.leaf+L".pending",pending))Require(pending==text,"Signal original ACL pending record differs");
}
// Completed admission checks stable roots only. Explicit re-Prepare does not audit external descendant ACL drift.
inline bool CheckLocked(const Layout& layout)
{
    const auto admission=ReadSummary(layout);Require(sync_string(admission,L"Phase")==L"complete"&&sync_hex(sync_string(admission,L"Digest")),"Signal private ACL conversion incomplete");
    for(const auto& root:layout.roots){tcard_asset::File handle(tcard_signal_acl::Open(tcard_native::Extended(root)));std::vector<BYTE> bytes;bool installed=false;
        Require(bool(handle)&&tcard_signal_acl::Read(handle.value,bytes)&&Supported(bytes,layout.user,installed,true)&&installed&&Protected(bytes),"Signal private root protection drift");}
    tcard_asset::File metadata(tcard_signal_acl::Open(layout.metadata));std::vector<BYTE> bytes;bool installed=false;
    Require(bool(metadata)&&tcard_signal_acl::Read(metadata.value,bytes)&&Supported(bytes,layout.user,installed,true)&&installed&&Protected(bytes),"Signal ACL admission protection drift");return true;
}
inline bool Prepare(const std::wstring& nativeRoot,const std::wstring& profileRoot,std::stop_token stop,std::wstring& error)
{
    error.clear();std::wstring context;try{Result result;const bool okay=WithSignalProtection(nativeRoot,profileRoot,[&]{
        Cancel(stop);const auto layout=Roots(nativeRoot,profileRoot);std::wstring prior;
        JsonObject admission;const bool present=RawRead(layout.metadata+L"\\admission.sealed",prior);
        if(present){admission=ReadSummary(layout);if(sync_string(admission,L"Phase")==L"complete")return CheckLocked(layout);
            Require(OneOf(sync_string(admission,L"Phase"),{L"capture",L"prepared"}),"Signal ACL admission phase invalid");}
        const auto retained=Metadata(layout);
        if(!present)Publish(layout,L"admission.sealed",std::wstring(Summary(layout,L"capture",L"",0).Stringify()));
        std::vector<Object> objects;size_t files=0;for(unsigned role=0;role<3;++role)Walk(layout,role,L"",role==0?Kind::Store:role==1?Kind::Profile:Kind::Control,0,objects,files,stop,context);
        std::set<std::wstring> expected{L"admission.sealed"};for(const auto& object:objects)expected.insert(object.leaf);
        for(const auto& leaf:retained)Require(expected.contains(leaf),"Signal orphan ACL backup retained; admission refused");
        std::sort(objects.begin(),objects.end(),[](const Object& a,const Object& b){return a.leaf<b.leaf;});std::wstring captured;
        for(const auto& object:objects)captured+=object.leaf+L":"+object.id+L"\n";const auto digest=tcard_bridge::Hash(captured);
        const bool prepared=present&&sync_string(admission,L"Phase")==L"prepared";
        if(prepared)Require(sync_string(admission,L"Digest")==digest&&admission.GetNamedNumber(L"Count")==objects.size(),"Signal prepared inventory changed");
        // First pass: every original SD is durable and read back before ANY target ACL changes.
        for(const auto& object:objects){Trace(object.role,object.relative,context);Cancel(stop);tcard_asset::File file(tcard_signal_acl::Open(object.path));std::vector<BYTE> current,old,target;
            Require(bool(file)&&FileId(file.value,object.directory)==object.id&&tcard_signal_acl::Read(file.value,current),"Signal captured object changed");Backup(layout,object,current,old,target,!prepared);}
        if(!prepared)Publish(layout,L"admission.sealed",std::wstring(Summary(layout,L"prepared",digest,objects.size()).Stringify()));
        std::stable_sort(objects.begin(),objects.end(),[](const Object& a,const Object& b){return a.depth>b.depth;});
        // Second pass: protected descendants first, so ancestor propagation cannot invalidate their originals.
        for(const auto& object:objects){Trace(object.role,object.relative,context);Cancel(stop);tcard_asset::File file(tcard_signal_acl::Open(object.path,READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES));std::vector<BYTE> current,old,target;
            Require(bool(file)&&FileId(file.value,object.directory)==object.id&&tcard_signal_acl::Read(file.value,current),"Signal captured object changed");Backup(layout,object,current,old,target,false);
            if(!EqualSd(current,target)){PACL acl=nullptr;BOOL exists=FALSE,def=FALSE;Require(GetSecurityDescriptorDacl(target.data(),&exists,&acl,&def)&&exists&&acl,"Signal target DACL missing");
                const auto status=SetSecurityInfo(file.value,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);
                if(status!=ERROR_SUCCESS)throw std::runtime_error("Signal private ACL application failed Win32="+std::to_string(status));}
            Require(tcard_signal_acl::Read(file.value,current)&&EqualSd(current,target),"Signal private ACL readback differs");}
        Cancel(stop);Publish(layout,L"admission.sealed",std::wstring(Summary(layout,L"complete",digest,objects.size()).Stringify()));return CheckLocked(layout);
    },result);if(!okay)error=(result.error.empty()?L"Signal private protection refused":result.error)+context;return okay;
    }catch(const std::exception& e){error.assign(e.what(),e.what()+strlen(e.what()));error+=context;return false;}catch(...){error=L"Signal private protection failed";return false;}
}
inline bool Check(const std::wstring& nativeRoot,const std::wstring& profileRoot,std::wstring& error)
{
    error.clear();try{Result result;const bool okay=WithSignalProtection(nativeRoot,profileRoot,[&]{return CheckLocked(Roots(nativeRoot,profileRoot));},result);
        if(!okay)error=result.error.empty()?L"Signal private protection refused":result.error;return okay;
    }catch(const std::exception& e){error.assign(e.what(),e.what()+strlen(e.what()));return false;}catch(...){error=L"Signal private protection check failed";return false;}
}
} // namespace tcard_signal_protection
