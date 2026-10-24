#pragma once
#include "card_native_commit.h"

namespace tcard_native {
// Candidate bytes are pinned; source identities plus sticky change observation preserve live saves.
class Activation {
    class Pins {
        struct Item {std::unique_ptr<tcard_asset::File> file;BY_HANDLE_FILE_INFORMATION info{};};
        std::map<std::wstring,Item> files_;
        static bool Same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b)
        {
            return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
                a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&CompareFileTime(&a.ftLastWriteTime,&b.ftLastWriteTime)==0;
        }
        void Scan(const std::wstring& root,const std::wstring& relative,bool source,bool capture,size_t& visited,size_t& count,std::stop_token stop)
        {
            if(stop.stop_requested())throw std::runtime_error("Native activation proof interrupted");
            const auto directory=Extended(relative.empty()?root:root+L"\\"+relative);
            tcard_asset::File guard(tcard_asset::open_dir(directory,false));if(!guard)throw std::runtime_error("Unsafe activation directory");
            WIN32_FIND_DATAW entry{};const auto search=FindFirstFileW((directory+L"\\*").c_str(),&entry);
            if(search==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return;throw std::runtime_error("Activation enumeration failed");}
            struct Search{HANDLE value;~Search(){FindClose(value);}} cleanup{search};
            do{if(stop.stop_requested())throw std::runtime_error("Native activation proof interrupted");const std::wstring name=entry.cFileName;
                if(name==L"."||name==L".."||(source&&relative.empty()&&tcard_retained::Excluded(name)))continue;
                const auto path=relative.empty()?name:relative+L"\\"+name;
                if(++visited>2*tcard_retained::maxFiles||!tcard_retained::Path(path)||(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Unsafe activation entry");
                if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){Scan(root,path,source,capture,visited,count,stop);continue;}
                if(++count>tcard_retained::maxFiles)throw std::runtime_error("Activation file budget exceeded");
                auto file=std::make_unique<tcard_asset::File>(CreateFileW((directory+L"\\"+name).c_str(),capture&&!source?GENERIC_READ:FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_DELETE|(!capture||source?FILE_SHARE_WRITE:0),nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                BY_HANDLE_FILE_INFORMATION info{};
                if(!*file||!tcard_asset::regular(file->value,false)||!GetFileInformationByHandle(file->value,&info))throw std::runtime_error("Activation file identity unavailable");
                if(capture){if(source)file.reset();if(!files_.emplace(path,Item{std::move(file),info}).second)throw std::runtime_error("Duplicate activation entry");}
                else{const auto found=files_.find(path);BY_HANDLE_FILE_INFORMATION held{};
                    if(found==files_.end()||(!source&&!GetFileInformationByHandle(found->second.file->value,&held))||
                        (source?!Same(found->second.info,info):!Same(found->second.info,held)||!Same(held,info)))
                        throw std::runtime_error("Native data changed after verification");}
            }while(FindNextFileW(search,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Activation enumeration interrupted");
        }
    public:
        void Capture(const std::wstring& root,bool source,std::stop_token stop){size_t visited=0,count=0;Scan(root,L"",source,true,visited,count,stop);}
        void Current(const std::wstring& root,bool source){size_t visited=0,count=0;Scan(root,L"",source,false,visited,count,{});if(count!=files_.size())throw std::runtime_error("Native activation entries disappeared");}
    } local_,candidate_;
    struct Changes {HANDLE value=INVALID_HANDLE_VALUE;~Changes(){if(value!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(value);}} changes_;
    std::wstring source_,candidatePath_,group_;
    Plan plan_;
    Activation()=default;
public:
    Activation(const Activation&)=delete;
    Activation& operator=(const Activation&)=delete;
    static std::unique_ptr<Activation> Verify(const std::wstring& source,std::stop_token stop={})
    {
        if(stop.stop_requested())throw std::runtime_error("Native activation proof interrupted");
        auto proof=std::unique_ptr<Activation>(new Activation);proof->source_=source;
        tcard_retained::Capture capture;
        {std::lock_guard guard(sync_mutex);SyncLock lock(Owner(source));if(!lock)throw std::runtime_error("Native proof owner busy");
            proof->plan_=ReadPlan(source);if(Selected(source)!=proof->plan_.before)throw std::runtime_error("Native proof generation changed");
            capture=tcard_retained::Read(source);if(capture.digest!=proof->plan_.native)throw std::runtime_error("Native proof source differs");
            proof->group_=tcard_chunks::Group(source);const auto staged=tcard_stage::Data(source),destination=Data(source,proof->plan_.after);
            const bool moved=Exists(destination);if(moved&&Exists(staged))throw std::runtime_error("Ambiguous native proof candidates");
            if(!moved){const auto generation=Control(source)+L"\\"+proof->plan_.after;
                if(!tcard_bridge::Protect(generation))throw std::runtime_error("Cannot protect native proof generation");Check(generation);
                if(!MoveFileExW(staged.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot retain native proof generation");}
            proof->candidatePath_=destination;
        }
        proof->changes_.value=FindFirstChangeNotificationW(Extended(source).c_str(),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_ATTRIBUTES);
        if(proof->changes_.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Native proof change observation unavailable");
        proof->local_.Capture(source,true,stop);proof->candidate_.Capture(proof->candidatePath_,false,stop);
        // Protected Begin/Prepare records bind the complete catalogs and their deterministic inventories.
        if(tcard_retained::Collect(source,true,stop)!=capture.files||Digest(tcard_retained::Collect(proof->candidatePath_,false,stop))!=proof->plan_.catalog)
            throw std::runtime_error("Native activation bytes differ");
        if(stop.stop_requested())throw std::runtime_error("Native activation proof interrupted");
        proof->local_.Current(source,true);proof->candidate_.Current(proof->candidatePath_,false);return proof;
    }
    bool Commit(const std::wstring& source)
    {
        std::unique_lock guard(sync_mutex,std::try_to_lock);SyncLock lock(Owner(source),0);
        if(!guard.owns_lock()||!lock)throw std::runtime_error("Native activation owner busy");
        const auto plan=ReadPlan(source);
        if(source!=source_||Selected(source)!=plan_.before||tcard_chunks::Group(source)!=group_||
            plan.before!=plan_.before||plan.after!=plan_.after||plan.catalog!=plan_.catalog||plan.native!=plan_.native||plan.inventory!=plan_.inventory)
            throw std::runtime_error("Native activation proof context changed");
        if(tcard_retained::Read(source).digest!=plan_.native)throw std::runtime_error("Native activation capture changed");
        const auto staged=tcard_stage::Data(source),destination=Data(source,plan_.after);
        if(!Exists(destination)||Exists(staged))throw std::runtime_error("Native activation candidate moved");
        // A sticky kernel notification also detects in-place saves, even if size or timestamps are restored.
        if(WaitForSingleObject(changes_.value,0)!=WAIT_TIMEOUT)throw std::runtime_error("Native source changed during verification");
        local_.Current(source,true);candidate_.Current(candidatePath_,false);
        Check(destination);JsonObject pointer;pointer.Insert(L"Version",JsonValue::CreateNumberValue(1));pointer.Insert(L"Generation",JsonValue::CreateStringValue(plan_.after));
        if(!sync_write(Control(source),L"active.json",pointer)||Selected(source)!=plan_.after)throw std::runtime_error("Native activation publication blocked");return true;
    }
};
}
