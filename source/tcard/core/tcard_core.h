#pragma once

#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace tcard {

struct CardRecord {
    std::wstring id;
    std::wstring title;
    std::wstring source;
    std::wstring color = L"#FFF5A8";
    std::wstring fontFamily = L"Segoe UI";
    double fontSize = 14.0;
    bool inheritFontFamily = true;
    bool inheritFontSize = true;
    bool live = true;
    bool markdown = false;
    bool richHtml = false;
    int windowWidthDip = 0;
    int windowHeightDip = 0;
    FILETIME updatedUtc{};
    FILETIME createdUtc{};
};

// A thread-bound update scope. Values are sampled lazily once per scope.
// All renders in the scope use its time; destroy it before the next update.
class RenderBatch {
public:
    explicit RenderBatch(const SYSTEMTIME& localTime);
    ~RenderBatch();
    RenderBatch(const RenderBatch&) = delete;
    RenderBatch& operator=(const RenderBatch&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::wstring Render(const std::wstring& source, const SYSTEMTIME& localTime);
std::wstring RenderMarkdown(const std::wstring& source);
std::wstring RenderRich(const std::wstring& source, const SYSTEMTIME& localTime);
bool ConfigureCustomVariables(const std::wstring& iniPath);
void RefreshCustomVariables();
bool LoadCards(const std::wstring& filePath, std::vector<CardRecord>& cards);
// Unlike legacy LoadCards, a readable empty directory is a successful snapshot.
bool LoadCardSnapshot(const std::wstring& filePath, std::vector<CardRecord>& cards, bool migrate = true);
// Read only requested current bundles; committed deletions are absent from the result.
// Failure preserves the output. This never scans or migrates unrelated notes.
bool LoadCardSubset(const std::wstring& filePath, const std::vector<std::wstring>& ids, std::vector<CardRecord>& cards);
bool SaveCards(const std::wstring& filePath, std::vector<CardRecord>& cards, bool checkpoint = true, unsigned historyLimit = 0);
bool DeleteCard(const std::wstring& filePath, const std::wstring& id);
// Removes only expired local trash copies. Zero uses current UTC; nonzero supports deterministic audits.
// Tombstones, sync history and offline intent are never removed by this operation.
bool PurgeTrash(const std::wstring& filePath, ULONGLONG nowUtcMs = 0);
// Local retained copies only; tokens are opaque and must be revalidated before every action.
// Restoration appends a causal save. Removal retains synchronized deletion records and history.
struct TrashRecord {
    CardRecord card;
    std::wstring token;
    std::wstring directory;
    ULONGLONG deletedUtcMs = 0;
    bool readable = false;
};
bool LoadTrash(const std::wstring& filePath, std::vector<TrashRecord>& records);
// Preserve local history/geometry before applying a remote restore; never create a local sync operation.
// The caller still commits the received current body. Existing Trash copies remain untouched.
bool RetainCardHistory(const std::wstring& filePath, const std::wstring& id, CardRecord& retained);
bool RestoreTrash(const std::wstring& filePath, const std::wstring& token, CardRecord& restored);
bool RemoveTrash(const std::wstring& filePath, const std::wstring& token);
std::wstring CardDirectory(const std::wstring& filePath, const std::wstring& id);

}
