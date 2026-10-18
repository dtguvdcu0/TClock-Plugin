#pragma once
#include "../../core/card_sync_cipher.h"

// Removable read-only native journal adapter. Caller is the UI owner or private worker.
namespace tcard_compat {
bool Capture(const std::wstring& source,std::vector<tcard_cipher::Object>& objects,std::string& error);
// Holds the native writer mutex through final snapshot capture and v2 activation.
bool AdvanceJournal(const std::wstring& root,const std::wstring& source,size_t budget,bool activate,tcard_cipher::Status& status);
}
