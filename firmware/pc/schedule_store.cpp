// SPDX-License-Identifier: Apache-2.0
#include "schedule_store.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
namespace micropixel::pc {
namespace {
struct Statement {
    sqlite3_stmt* p{};
    Statement(sqlite3* db, const char* sql, std::string_view app) {
        if (db && sqlite3_prepare_v2(db, sql, -1, &p, nullptr) == SQLITE_OK)
            sqlite3_bind_text(p, 1, app.data(), app.size(), SQLITE_TRANSIENT);
    }
    ~Statement() {
        if (p) sqlite3_finalize(p);
    }
};
}  // namespace
ScheduleStore::ScheduleStore() {
    const char* configured = std::getenv("MICROPIXEL_PC_DATA_DIR");
    std::filesystem::path directory = configured ? configured : "build/pc-data";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || sqlite3_open((directory / "schedules.sqlite3").c_str(), &db_) != SQLITE_OK) return;
    int schema = -1;
    {
        Statement version(db_, "PRAGMA user_version", {});
        if (version.p && sqlite3_step(version.p) == SQLITE_ROW) schema = sqlite3_column_int(version.p, 0);
    }
    if (schema < 0 || schema > 1) {
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    if (sqlite3_exec(db_,
                     "PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS exports(app TEXT,caller TEXT,types "
                     "INTEGER,PRIMARY KEY(app,caller)); CREATE TABLE IF NOT EXISTS jobs(app TEXT,id INTEGER,flags "
                     "INTEGER,period INTEGER,budget INTEGER,due INTEGER,PRIMARY KEY(app,id)); CREATE TABLE IF NOT "
                     "EXISTS stopped(app TEXT PRIMARY KEY,value INTEGER NOT NULL); PRAGMA user_version=1;",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}
ScheduleStore::~ScheduleStore() {
    if (db_) sqlite3_close(db_);
}
bool ScheduleStore::SaveJob(std::string_view app, uint32_t id, const micropixel_job_plan_t* plan) {
    Statement s(db_, plan ? "INSERT OR REPLACE INTO jobs VALUES(?,?,?,?,?,?)" : "DELETE FROM jobs WHERE app=? AND id=?",
                app);
    if (!s.p) return false;
    sqlite3_bind_int(s.p, 2, id);
    if (plan) {
        sqlite3_bind_int(s.p, 3, plan->flags);
        sqlite3_bind_int(s.p, 4, plan->interval_seconds);
        sqlite3_bind_int(s.p, 5, plan->deadline_ms);
        sqlite3_bind_int64(s.p, 6, plan->next_utc);
    }
    return sqlite3_step(s.p) == SQLITE_DONE;
}
void ScheduleStore::LoadJobs(std::string_view app, uint32_t index, host::JobScheduler& jobs) {
    Statement s(db_, "SELECT id,flags,period,budget,due FROM jobs WHERE app=? LIMIT 2", app);
    if (!s.p) return;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        host::JobScheduler::Record r{};
        r.app = index;
        r.plan = {sizeof(r.plan),
                  static_cast<uint16_t>(sqlite3_column_int(s.p, 1)),
                  static_cast<uint32_t>(sqlite3_column_int(s.p, 0)),
                  static_cast<uint32_t>(sqlite3_column_int(s.p, 2)),
                  static_cast<uint32_t>(sqlite3_column_int(s.p, 3)),
                  static_cast<uint64_t>(sqlite3_column_int64(s.p, 4))};
        (void)jobs.Restore(r);
    }
}
bool ScheduleStore::SetStopped(std::string_view app, bool value) {
    Statement s(db_, "INSERT OR REPLACE INTO stopped VALUES(?,?)", app);
    if (!s.p) return false;
    sqlite3_bind_int(s.p, 2, value);
    return sqlite3_step(s.p) == SQLITE_DONE;
}
bool ScheduleStore::Stopped(std::string_view app) {
    Statement s(db_, "SELECT value FROM stopped WHERE app=?", app);
    return s.p && sqlite3_step(s.p) == SQLITE_ROW && sqlite3_column_int(s.p, 0);
}
bool ScheduleStore::Delete(std::string_view app) {
    if (!db_ || sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    bool ok = true;
    for (const char* sql :
         {"DELETE FROM jobs WHERE app=?", "DELETE FROM stopped WHERE app=?", "DELETE FROM exports WHERE app=?"}) {
        Statement statement(db_, sql, app);
        ok = ok && statement.p && sqlite3_step(statement.p) == SQLITE_DONE;
    }
    if (!ok || sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}
bool ScheduleStore::SaveWake(std::string_view app, std::string_view caller, uint32_t types) {
    Statement count(db_, "SELECT count(*) FROM exports WHERE app=? AND caller<>?", app);
    if (!count.p) return false;
    sqlite3_bind_text(count.p, 2, caller.data(), caller.size(), SQLITE_TRANSIENT);
    if (sqlite3_step(count.p) != SQLITE_ROW || sqlite3_column_int(count.p, 0) >= 4) return false;
    Statement s(db_, "INSERT OR REPLACE INTO exports VALUES(?,?,?)", app);
    if (!s.p) return false;
    sqlite3_bind_text(s.p, 2, caller.data(), caller.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.p, 3, types);
    return sqlite3_step(s.p) == SQLITE_DONE;
}
void ScheduleStore::LoadWake(std::string_view app, uint32_t index, host::AppHub& hub) {
    Statement s(db_, "SELECT caller,types FROM exports WHERE app=? LIMIT 4", app);
    if (!s.p) return;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        auto* caller = reinterpret_cast<const char*>(sqlite3_column_text(s.p, 0));
        if (caller) (void)hub.AllowWake(index, caller, sqlite3_column_int64(s.p, 1));
    }
}
}  // namespace micropixel::pc
