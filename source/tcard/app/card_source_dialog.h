#pragma once

// This modal extends the existing key/join flow; only its caller activates on the UI thread.
bool card_source_equal(const std::wstring& a,const std::wstring& b)
{
    auto normalize=[](const std::wstring& input){std::wstring out;bool space=false;for(auto c:input){if(iswspace(c)){space=!out.empty();continue;}if(space){out+=L' ';space=false;}if(c>=L'A'&&c<=L'Z')c=static_cast<wchar_t>(c+L'a'-L'A');out+=c;}return out;};
    auto left=normalize(a),right=normalize(b);const bool same=left==right;tcard_source_job::Wipe first{left},second{right};return same;
}
bool card_source_policy(const tcard_source_job::Intent& intent,const tcard_bridge::Settings& settings)
{return intent.allow==settings.whitelist&&intent.deny==settings.blacklist&&intent.remote==settings.remoteAdmission;}
struct CardSourceDialog {
    enum class Phase {Idle,Acquiring,Building,Ready,Paused,Failed};
    std::wstring directory,native,profile,words,job;std::mutex mutex;
    std::atomic<Phase> phase{Phase::Idle};std::atomic<bool> done{true};bool closing=false,blocked=false;
    std::jthread task;
    ~CardSourceDialog(){task.request_stop();if(task.joinable())task.join();SecureZeroMemory(words.data(),words.size()*sizeof(wchar_t));}
    std::wstring Job(){std::lock_guard lock(mutex);return job;}
    bool Reuse(const std::wstring& endpoint,const tcard_bridge::Settings& settings)
    {
        const auto token=Job();if(token.empty())return false;tcard_source_job::Intent intent;tcard_source_job::ReadIntent(profile,token,intent);tcard_crypto::Keys old;
        return card_source_equal(words,intent.words)&&intent.endpoint==endpoint&&card_source_policy(intent,settings)&&tcard_storage::LoadKey(native,old)&&tcard_storage::Matches(profile,old)&&tcard_ack::Identity(old)==intent.previous;
    }
    void Start(const std::wstring& endpoint,const tcard_bridge::Settings& settings,bool reuse)
    {
        if(!done.load())return;if(task.joinable())task.join();done=false;phase=Phase::Acquiring;
        try{task=std::jthread([this,endpoint,settings,reuse](std::stop_token stop){
            try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                {tcard_bridge::Client client;client.SetDirectory(directory);client.SetStorage(native);
                    struct JoinChild{tcard_bridge::Client& value;~JoinChild(){while(!value.StopAcquisition())std::this_thread::sleep_for(std::chrono::milliseconds(50));}} join{client};
                    auto token=reuse?Job():L"";
                    if(stop.stop_requested())throw std::runtime_error("Acquisition paused");
                    const bool started=reuse?client.ResumeAcquisition(token):client.BeginAcquisition(words,endpoint,settings,token);
                    {std::lock_guard lock(mutex);job=token;}tcard_source_job::Require(started,"Cannot start protected acquisition");
                    for(;;){
                        if(stop.stop_requested())throw std::runtime_error("Acquisition paused");tcard_source_job::Snapshot status;
                        if(client.ReadAcquisition(status)){
                            if(status.ready)break;tcard_source_job::Require(status.phase==L"acquiring","Acquisition stopped before verified originals");
                        }else tcard_source_job::Require(client.AcquisitionRunning(),"Acquisition child exited without bound status");
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    tcard_source_job::Require(client.StopAcquisition(),"Acquisition normal stop pending");phase=Phase::Building;tcard_replacement::Candidate candidate;
                    tcard_source_job::Require(tcard_source_job::PrepareCandidate(profile,token,candidate,[&](const std::wstring&,size_t,size_t){if(stop.stop_requested())throw std::runtime_error("Reconstruction paused");}),"Source-only reconstruction incomplete");
                    if(stop.stop_requested())throw std::runtime_error("Reconstruction paused");phase=Phase::Ready;
                }
            }catch(...){phase=stop.stop_requested()?Phase::Paused:Phase::Failed;}
            done=true;
        });}catch(...){phase=Phase::Failed;done=true;throw;}
    }
};
std::wstring card_source_endpoint(HWND hwnd,const tcard_bridge::Settings& settings)
{
    std::vector<tcard_net::Peer> peers;std::vector<tcard_net::Rule> allow,deny;const auto text=window_text(GetDlgItem(hwnd,1821));
    if(!tcard_net::ParsePeers(text,settings.peerPort?settings.peerPort:settings.listenPort,peers)||peers.size()!=1||!tcard_net::ParseRules(settings.whitelist,allow)||!tcard_net::ParseRules(settings.blacklist,deny)||!tcard_net::IsAllowed(peers[0].address,allow,deny))return L"";
    return peers[0].ip+L":"+std::to_wstring(peers[0].port);
}
void card_source_update(HWND hwnd,CardSourceDialog& state)
{
    using Phase=CardSourceDialog::Phase;const auto phase=state.phase.load();const bool done=state.done.load();
    const wchar_t* key=L"sync.source_idle";const wchar_t* fallback=L"Choose the running source PC's listening endpoint. Local data stays in its current group until you confirm replacement.";
    if(state.closing){key=L"sync.source_stopping";fallback=L"Pausing safely. The protected download is kept for resume.";}
    else if(phase==Phase::Acquiring){key=L"sync.source_acquiring";fallback=L"Downloading saved notes, history and images from the specified source...";}
    else if(phase==Phase::Building){key=L"sync.source_building";fallback=L"Verifying originals and preparing the replacement...";}
    else if(phase==Phase::Ready){key=L"sync.source_ready";fallback=L"Verified. Replacement selects the source contents and keeps your old notes/history in encrypted recovery.";}
    else if(phase==Phase::Failed||state.blocked){key=L"sync.source_failed";fallback=L"Download could not finish. Check the source and IP rules. Existing data and the protected download are kept.";}
    SetDlgItemTextW(hwnd,1823,tcard_text(key,fallback));EnableWindow(GetDlgItem(hwnd,1821),done&&!state.closing&&!state.blocked&&phase!=Phase::Ready);
    bool reuse=false;std::wstring endpoint;try{const auto settings=tcard_bridge::ReadSettings(preferences_path());endpoint=card_source_endpoint(hwnd,settings);if(done&&!state.blocked&&!endpoint.empty())reuse=state.Reuse(endpoint,settings);}catch(...){state.blocked=true;}
    SetDlgItemTextW(hwnd,1824,reuse?tcard_text(L"sync.source_resume",L"Resume download"):tcard_text(L"sync.source_start",L"Start new download"));
    EnableWindow(GetDlgItem(hwnd,1824),done&&!state.closing&&!state.blocked&&!endpoint.empty()&&phase!=Phase::Ready);
    EnableWindow(GetDlgItem(hwnd,IDOK),done&&!state.closing&&phase==Phase::Ready);EnableWindow(GetDlgItem(hwnd,IDCANCEL),!state.closing);
}
INT_PTR CALLBACK card_source_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(const auto painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    auto* state=reinterpret_cast<CardSourceDialog*>(GetWindowLongPtrW(hwnd,DWLP_USER));
    if(message==WM_INITDIALOG){
        state=reinterpret_cast<CardSourceDialog*>(lp);SetWindowLongPtrW(hwnd,DWLP_USER,reinterpret_cast<LONG_PTR>(state));if(!state){EndDialog(hwnd,IDCANCEL);return TRUE;}
        SetWindowTextW(hwnd,tcard_text(L"sync.join_replace",L"Replace from source"));const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        auto control=[&](const wchar_t* cls,const wchar_t* value,DWORD style,int id,int x,int y,int width,int height){RECT rect{x,y,x+width,y+height};MapDialogRect(hwnd,&rect);
            HWND child=CreateWindowExW(wcscmp(cls,L"COMBOBOX")==0?WS_EX_CLIENTEDGE:0,cls,value,WS_CHILD|WS_VISIBLE|style,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(child,WM_SETFONT,font,TRUE);};
        control(L"STATIC",tcard_text(L"sync.source_endpoint",L"Source PC (IPv4:port)"),0,1820,8,8,344,12);
        control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL,1821,8,24,344,80);SendDlgItemMessageW(hwnd,1821,CB_LIMITTEXT,32,0);
        control(L"STATIC",tcard_text(L"sync.source_help",L"Keep TCard and synchronization running on the source PC. Your saved local data is encrypted and kept."),0,1822,8,44,344,20);
        control(L"STATIC",L"",0,1823,8,68,344,20);
        control(L"BUTTON",L"",WS_TABSTOP|BS_PUSHBUTTON,1824,8,92,104,16);
        control(L"BUTTON",tcard_text(L"sync.source_confirm",L"Replace and join"),WS_TABSTOP|BS_PUSHBUTTON,IDOK,128,92,104,16);
        control(L"BUTTON",tcard_text(L"button.cancel",L"Cancel"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDCANCEL,248,92,104,16);
        const auto settings=tcard_bridge::ReadSettings(preferences_path());std::vector<tcard_net::Peer> peers;tcard_net::ParsePeers(tcard_bridge::EditPeers(settings),settings.listenPort,peers);
        for(const auto& peer:peers){const auto text=peer.ip+L":"+std::to_wstring(peer.port);SendDlgItemMessageW(hwnd,1821,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
        if(!peers.empty())SetDlgItemTextW(hwnd,1821,(peers[0].ip+L":"+std::to_wstring(peers[0].port)).c_str());
        try{const auto token=tcard_source_job::Pending(state->profile);if(!token.empty()){tcard_source_job::Intent intent;tcard_source_job::ReadIntent(state->profile,token,intent);
            if(card_source_equal(state->words,intent.words)){state->job=token;SetDlgItemTextW(hwnd,1821,intent.endpoint.c_str());}}}catch(...){state->blocked=true;}
        tcard_ui::prepare_settings(hwnd);SetTimer(hwnd,1,100,nullptr);card_source_update(hwnd,*state);SetFocus(GetDlgItem(hwnd,IDCANCEL));return FALSE;
    }
    if(!state)return FALSE;
    if(message==WM_TIMER&&wp==1){card_source_update(hwnd,*state);if(state->closing&&state->done.load())EndDialog(hwnd,IDCANCEL);return TRUE;}
    if(message==WM_CLOSE){SendMessageW(hwnd,WM_COMMAND,IDCANCEL,0);return TRUE;}
    if(message==WM_COMMAND){const int id=LOWORD(wp);
        if(id==IDCANCEL){if(state->done.load())EndDialog(hwnd,IDCANCEL);else{state->closing=true;state->task.request_stop();card_source_update(hwnd,*state);}return TRUE;}
        if(id==IDOK){if(state->done.load()&&state->phase.load()==CardSourceDialog::Phase::Ready&&!state->closing)EndDialog(hwnd,IDOK);return TRUE;}
        if(id==1824&&state->done.load()&&!state->closing&&!state->blocked&&state->phase.load()!=CardSourceDialog::Phase::Ready){try{
            const auto settings=tcard_bridge::ReadSettings(preferences_path());const auto endpoint=card_source_endpoint(hwnd,settings);if(!endpoint.empty())state->Start(endpoint,settings,state->Reuse(endpoint,settings));
            }catch(...){state->blocked=true;}card_source_update(hwnd,*state);return TRUE;}
        if(id==1821){card_source_update(hwnd,*state);return TRUE;}
    }
    if(message==WM_DESTROY)KillTimer(hwnd,1);return FALSE;
}
bool card_choose_source(HWND owner,const std::wstring& words,std::wstring& job)
{
    CardSourceDialog state;state.directory=g_sync_client.Directory();state.native=g_file_path;state.profile=g_sync_client.Profile();state.words=words;
    struct Active{Active(){g_source_dialog=true;}~Active(){g_source_dialog=false;}} active;
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cx=360;dialog.cy=116;
    std::vector<WORD> data(sizeof(dialog)/sizeof(WORD));memcpy(data.data(),&dialog,sizeof(dialog));data.insert(data.end(),{0,0,0,9});const wchar_t face[]=L"Segoe UI";data.insert(data.end(),face,face+ARRAYSIZE(face));
    const auto result=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),owner,card_source_proc,reinterpret_cast<LPARAM>(&state));
    if(result!=IDOK)return false;job=state.Job();return !job.empty();
}
