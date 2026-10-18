#include "task_store.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsqlite/winsqlite3.h>

namespace tcalendar {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out.data(), n);
    return out;
}

bool ExecSql(sqlite3* db, const char* sql, std::wstring& out_error) {
    out_error.clear();
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err && *err) {
            out_error = Utf8ToWide(err);
        } else if (db) {
            out_error = Utf8ToWide(sqlite3_errmsg(db));
        }
    }
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

bool EnsureColumnExists(sqlite3* db, const char* table, const char* column,
                        const char* column_def, std::wstring& out_error) {
    out_error.clear();
    if (!db || !table || !column || !column_def) return false;

    std::string pragma_sql = "PRAGMA table_info(";
    pragma_sql += table;
    pragma_sql += ")";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, pragma_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        out_error = Utf8ToWide(sqlite3_errmsg(db));
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (col_name && std::strcmp(col_name, column) == 0) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);

    if (found) return true;

    std::string alter_sql = "ALTER TABLE ";
    alter_sql += table;
    alter_sql += " ADD COLUMN ";
    alter_sql += column;
    alter_sql += " ";
    alter_sql += column_def;
    return ExecSql(db, alter_sql.c_str(), out_error);
}

unsigned long long ParseTaskSeq(const std::wstring& id) {
    if (id.rfind(L"t-", 0) != 0) return 0;
    unsigned long long value = 0;
    for (size_t i = 2; i < id.size(); ++i) {
        const wchar_t c = id[i];
        if (c < L'0' || c > L'9') return 0;
        value = value * 10ULL + static_cast<unsigned long long>(c - L'0');
    }
    return value;
}

} // namespace

bool TaskStore::Initialize(const std::wstring& db_path) {
    if (db_) return true;

    last_error_.clear();
    db_path_ = db_path;
    if (db_path_.empty()) {
        SetLastError(L"storage_db_path is empty");
        return false;
    }

    try {
        const std::filesystem::path p(db_path_);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
    } catch (...) {
        SetLastError(L"failed to create DB parent directory");
        return false;
    }

    const std::string db_utf8 = WideToUtf8(db_path_);
    if (db_utf8.empty()) {
        SetLastError(L"failed to convert DB path to UTF-8");
        return false;
    }

    if (sqlite3_open_v2(db_utf8.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::wstring sqlite_error;
        if (db_) {
            sqlite_error = Utf8ToWide(sqlite3_errmsg(db_));
            sqlite3_close(db_);
            db_ = nullptr;
        }
        SetLastError(L"sqlite open failed" + (sqlite_error.empty() ? std::wstring() : (L": " + sqlite_error)));
        return false;
    }

    if (!EnsureSchema() || !LoadAllTasks()) {
        Shutdown();
        return false;
    }

    return true;
}

void TaskStore::Shutdown() {
    by_id_.clear();
    seq_ = 0;
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool TaskStore::EnsureSchema() {
    if (!db_) {
        SetLastError(L"EnsureSchema called without open DB");
        return false;
    }

    std::wstring schema_error;
    if (!ExecSql(db_,
            "CREATE TABLE IF NOT EXISTS tasks("
            "id TEXT PRIMARY KEY,"
            "date TEXT NOT NULL,"
            "title TEXT NOT NULL,"
            "detail TEXT NOT NULL DEFAULT '',"
            "start_time TEXT NOT NULL DEFAULT '',"
            "end_time TEXT NOT NULL DEFAULT '',"
            "done INTEGER NOT NULL,"
            "alert_enabled INTEGER NOT NULL DEFAULT 0,"
            "updated_at_utc TEXT NOT NULL"
            ");",
            schema_error)) {
        SetLastError(L"failed to create/verify schema" +
                     (schema_error.empty() ? std::wstring() : (L": " + schema_error)));
        return false;
    }

    if (!EnsureColumnExists(db_, "tasks", "detail", "TEXT NOT NULL DEFAULT ''", schema_error) ||
        !EnsureColumnExists(db_, "tasks", "start_time", "TEXT NOT NULL DEFAULT ''", schema_error) ||
        !EnsureColumnExists(db_, "tasks", "end_time", "TEXT NOT NULL DEFAULT ''", schema_error) ||
        !EnsureColumnExists(db_, "tasks", "alert_enabled", "INTEGER NOT NULL DEFAULT 0", schema_error)) {
        SetLastError(L"failed to migrate schema" +
                     (schema_error.empty() ? std::wstring() : (L": " + schema_error)));
        return false;
    }

    return true;
}

bool TaskStore::LoadAllTasks() {
    if (!db_) return false;

    by_id_.clear();
    seq_ = 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, date, title, detail, start_time, end_time, done, alert_enabled, updated_at_utc FROM tasks";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetLastError(L"failed to prepare task load query" +
                     std::wstring(L": ") + Utf8ToWide(sqlite3_errmsg(db_)));
        return false;
    }

    bool ok = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TaskItem t{};
        t.id = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        t.date = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        t.title = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
        t.detail = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
        t.start_time = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        t.end_time = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        t.done = sqlite3_column_int(stmt, 6) != 0;
        t.alert_enabled = sqlite3_column_int(stmt, 7) != 0;
        t.updated_at_utc = Utf8ToWide(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));

        if (t.id.empty() || t.date.empty() || t.title.empty()) {
            SetLastError(L"loaded task row has empty required fields");
            ok = false;
            break;
        }

        seq_ = (std::max)(seq_, ParseTaskSeq(t.id));
        by_id_[t.id] = t;
    }

    sqlite3_finalize(stmt);
    if (!ok && last_error_.empty()) {
        SetLastError(L"failed while loading task rows");
    }
    return ok;
}

std::wstring TaskStore::NextId() {
    ++seq_;
    std::wstringstream ss;
    ss << L"t-" << seq_;
    return ss.str();
}

std::wstring TaskStore::NowUtcIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    wchar_t buf[32] = {0};
    std::wcsftime(buf, 32, L"%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::wstring(buf);
}

const std::wstring& TaskStore::GetLastError() const {
    return last_error_;
}

void TaskStore::SetLastError(const std::wstring& message) {
    last_error_ = message;
}

void TaskStore::SetTestForceWriteFailure(bool enabled) {
    test_force_write_failure_ = enabled;
}

bool TaskStore::SaveTaskInsert(const TaskItem& task) {
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }

    if (test_force_write_failure_) {
        SetLastError(L"forced storage write failure (test mode)");
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO tasks(id, date, title, detail, start_time, end_time, done, alert_enabled, updated_at_utc) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetLastError(L"failed to prepare insert statement: " + Utf8ToWide(sqlite3_errmsg(db_)));
        return false;
    }

    const std::string id = WideToUtf8(task.id);
    const std::string date = WideToUtf8(task.date);
    const std::string title = WideToUtf8(task.title);
    const std::string detail = WideToUtf8(task.detail);
    const std::string start_time = WideToUtf8(task.start_time);
    const std::string end_time = WideToUtf8(task.end_time);
    const std::string updated = WideToUtf8(task.updated_at_utc);

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, start_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, end_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, task.done ? 1 : 0);
    sqlite3_bind_int(stmt, 8, task.alert_enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 9, updated.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) {
        SetLastError(L"failed to insert task: " + Utf8ToWide(sqlite3_errmsg(db_)));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool TaskStore::CreateTask(const std::wstring& date, const std::wstring& title,
                           const std::wstring& detail, const std::wstring& start_time,
                           const std::wstring& end_time, bool alert_enabled, TaskItem& out_task) {
    if (date.empty() || title.empty()) {
        SetLastError(L"invalid task payload");
        return false;
    }
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }

    TaskItem t{};
    t.id = NextId();
    t.date = date;
    t.title = title;
    t.detail = detail;
    t.start_time = start_time;
    t.end_time = end_time;
    t.done = false;
    t.alert_enabled = alert_enabled;
    t.updated_at_utc = NowUtcIso8601();

    if (!SaveTaskInsert(t)) return false;

    by_id_[t.id] = t;
    out_task = t;
    return true;
}

bool TaskStore::ToggleDone(const std::wstring& id, bool done) {
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }

    auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        SetLastError(L"task not found");
        return false;
    }

    const std::wstring updated_at = NowUtcIso8601();

    if (test_force_write_failure_) {
        SetLastError(L"forced storage write failure (test mode)");
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE tasks SET done=?, updated_at_utc=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetLastError(L"failed to prepare toggle statement: " + Utf8ToWide(sqlite3_errmsg(db_)));
        return false;
    }

    const std::string updated = WideToUtf8(updated_at);
    const std::string id_utf8 = WideToUtf8(id);

    sqlite3_bind_int(stmt, 1, done ? 1 : 0);
    sqlite3_bind_text(stmt, 2, updated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, id_utf8.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) {
        SetLastError(L"failed to update done flag: " + Utf8ToWide(sqlite3_errmsg(db_)));
    }
    sqlite3_finalize(stmt);
    if (!ok) return false;

    it->second.done = done;
    it->second.updated_at_utc = updated_at;
    return true;
}

bool TaskStore::UpdateTask(const std::wstring& id, const std::wstring& title,
                           const std::wstring& detail, const std::wstring& start_time,
                           const std::wstring& end_time, bool alert_enabled) {
    if (title.empty()) {
        SetLastError(L"invalid task payload");
        return false;
    }
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }

    auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        SetLastError(L"task not found");
        return false;
    }

    const std::wstring updated_at = NowUtcIso8601();

    if (test_force_write_failure_) {
        SetLastError(L"forced storage write failure (test mode)");
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE tasks SET title=?, detail=?, start_time=?, end_time=?, alert_enabled=?, updated_at_utc=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetLastError(L"failed to prepare task update statement: " + Utf8ToWide(sqlite3_errmsg(db_)));
        return false;
    }

    const std::string title_utf8 = WideToUtf8(title);
    const std::string detail_utf8 = WideToUtf8(detail);
    const std::string start_time_utf8 = WideToUtf8(start_time);
    const std::string end_time_utf8 = WideToUtf8(end_time);
    const std::string updated = WideToUtf8(updated_at);
    const std::string id_utf8 = WideToUtf8(id);

    sqlite3_bind_text(stmt, 1, title_utf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, detail_utf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, start_time_utf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, end_time_utf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, alert_enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 6, updated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, id_utf8.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) {
        SetLastError(L"failed to update task: " + Utf8ToWide(sqlite3_errmsg(db_)));
    }
    sqlite3_finalize(stmt);
    if (!ok) return false;

    it->second.title = title;
    it->second.detail = detail;
    it->second.start_time = start_time;
    it->second.end_time = end_time;
    it->second.alert_enabled = alert_enabled;
    it->second.updated_at_utc = updated_at;
    return true;
}

bool TaskStore::UpdateTitle(const std::wstring& id, const std::wstring& title) {
    if (title.empty()) {
        SetLastError(L"invalid task payload");
        return false;
    }
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }

    auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        SetLastError(L"task not found");
        return false;
    }

    const std::wstring updated_at = NowUtcIso8601();

    if (test_force_write_failure_) {
        SetLastError(L"forced storage write failure (test mode)");
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE tasks SET title=?, updated_at_utc=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetLastError(L"failed to prepare title update statement: " + Utf8ToWide(sqlite3_errmsg(db_)));
        return false;
    }

    const std::string title_utf8 = WideToUtf8(title);
    const std::string updated = WideToUtf8(updated_at);
    const std::string id_utf8 = WideToUtf8(id);

    sqlite3_bind_text(stmt, 1, title_utf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, updated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, id_utf8.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) {
        SetLastError(L"failed to update title: " + Utf8ToWide(sqlite3_errmsg(db_)));
    }
    sqlite3_finalize(stmt);
    if (!ok) return false;

    it->second.title = title;
    it->second.updated_at_utc = updated_at;
    return true;
}

bool TaskStore::DeleteTask(const std::wstring& id) {
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }

    auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        SetLastError(L"task not found");
        return false;
    }

    if (test_force_write_failure_) {
        SetLastError(L"forced storage write failure (test mode)");
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM tasks WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetLastError(L"failed to prepare delete statement: " + Utf8ToWide(sqlite3_errmsg(db_)));
        return false;
    }

    const std::string id_utf8 = WideToUtf8(id);
    sqlite3_bind_text(stmt, 1, id_utf8.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE) && (sqlite3_changes(db_) > 0);
    if (!ok) {
        SetLastError(L"failed to delete task: " + Utf8ToWide(sqlite3_errmsg(db_)));
    }
    sqlite3_finalize(stmt);
    if (!ok) return false;

    by_id_.erase(it);
    return true;
}

bool TaskStore::ReloadFromDb() {
    if (!db_) {
        SetLastError(L"database not initialized");
        return false;
    }
    return LoadAllTasks();
}

std::vector<TaskItem> TaskStore::GetDayTasks(const std::wstring& date) const {
    std::vector<TaskItem> out;
    for (const auto& kv : by_id_) {
        if (kv.second.date == date) out.push_back(kv.second);
    }
    return out;
}

std::vector<TaskItem> TaskStore::GetTasksInRange(const std::wstring& date_from, const std::wstring& date_to) const {
    std::vector<TaskItem> out;
    if (date_from.empty() || date_to.empty() || date_from > date_to) {
        return out;
    }
    for (const auto& kv : by_id_) {
        const auto& d = kv.second.date;
        if (!d.empty() && d >= date_from && d <= date_to) {
            out.push_back(kv.second);
        }
    }
    return out;
}

} // namespace tcalendar
