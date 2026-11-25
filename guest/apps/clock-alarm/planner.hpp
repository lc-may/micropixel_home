// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "sdk/calendar.hpp"
#include "sdk/micropixel.hpp"
namespace clock_app {
using namespace micropixel;
enum Page : unsigned { Alarm, Calendar, Notes, Editor, AlarmEditor };
struct Model {
    unsigned hour{8}, minute{}, page{Alarm}, month{1};
    unsigned editor_parent{Notes};
    int year{2026};
    bool daily{true}, enabled{}, opened{};
    int32_t offset{480};
    uint32_t revision{}, done_mask{};
    calendar::Date selected{}, draft_date{};
    uint32_t draft_minute{};
    int edit_slot{-1};
    std::string draft, title_status;
    std::array<std::optional<AlarmPlan>, 6> notes;
    void Read(const Application& app) {
        auto now = app.alarms().Now().value();
        offset = now.offset_minutes;
        for (unsigned i = 0; i < notes.size(); ++i) {
            auto note = app.alarms().Get(i + 3);
            if (note) {
                if (!notes[i] || notes[i]->enabled != note->enabled || notes[i]->once_utc != note->once_utc) ++revision;
                notes[i] = *note;
            } else if (note.error().code() == ErrorCode::kNotFound) {
                if (notes[i]) ++revision;
                notes[i].reset();
            }
        }
        done_mask = app.storage().GetU32Or("notes.done", 0);
        if (!opened) {
            opened = true;
            selected = calendar::LocalDate(now.utc, offset);
            year = selected.year;
            month = selected.month;
            auto saved = app.alarms().Get(1);
            if (saved) {
                enabled = saved->enabled;
                daily = saved->weekly;
                auto m = saved->weekly ? saved->minute_of_day
                                       : uint32_t(calendar::LocalSeconds(saved->once_utc, offset) % 86400) / 60;
                hour = m / 60;
                minute = m % 60;
            }
        }
    }
    bool Done(unsigned i) const { return notes[i] && !notes[i]->enabled && (done_mask & (1U << i)); }
    bool New(const Application& app) {
        edit_slot = -1;
        for (unsigned i = 0; i < notes.size(); ++i)
            if (!notes[i]) {
                edit_slot = int(i);
                break;
            }
        if (edit_slot < 0) {
            title_status = "事项已满，请先删除一条";
            return false;
        }
        draft.clear();
        draft_date = selected;
        draft_minute = 9 * 60;
        auto now = app.alarms().Now().value();
        const auto today = calendar::LocalDate(now.utc, offset);
        if (draft_date == today) {
            draft_minute = (uint32_t(calendar::LocalSeconds(now.utc, offset) % 86400) / 60 + 19) / 5 * 5;
            if (draft_minute >= 1440) {
                draft_date = calendar::FromDays(calendar::Days(today) + 1);
                draft_minute = 0;
            }
        }
        editor_parent = page;
        page = Editor;
        title_status = "输入事项名称，再选择提醒时间";
        ++revision;
        return true;
    }
    void Edit(unsigned i) {
        if (i >= notes.size() || !notes[i]) return;
        edit_slot = int(i);
        draft = notes[i]->label.c_str();
        draft_date = calendar::LocalDate(notes[i]->once_utc, offset);
        draft_minute = uint32_t(calendar::LocalSeconds(notes[i]->once_utc, offset) % 86400) / 60;
        editor_parent = page;
        page = Editor;
        title_status.clear();
        ++revision;
    }
    bool Save(const Application& app) {
        auto now = app.alarms().Now().value();
        auto utc = calendar::Utc(draft_date, draft_minute, offset);
        if (edit_slot < 0 || edit_slot >= 6 || draft.empty() || draft.size() > 63) {
            title_status = "请输入简短的事项名称";
            return false;
        }
        for (unsigned char c : draft)
            if (c < 32 || c == 127) {
                title_status = "事项名称不能包含控制字符";
                return false;
            }
        if (!now.valid || utc <= static_cast<int64_t>(now.utc) || utc > 4102444800LL) {
            title_status = "请选择未来的提醒时间";
            return false;
        }
        const auto old_mask = done_mask, new_mask = done_mask & ~(1U << edit_slot);
        if (!app.storage().SetU32("notes.done", new_mask)) {
            title_status = "无法保存事项，请重试";
            return false;
        }
        AlarmPlan plan;
        plan.id = uint32_t(edit_slot) + 3;
        plan.once_utc = uint64_t(utc);
        plan.label.Append(draft.c_str());
        auto saved = app.alarms().Register(plan);
        if (!saved) {
            (void)app.storage().SetU32("notes.done", old_mask);
            title_status = "提醒保存失败，请重试";
            return false;
        }
        done_mask = new_mask;
        notes[edit_slot] = plan;
        page = Notes;
        title_status = "事项已保存，到点提醒";
        ++revision;
        app.log().Info("NOTE saved");
        return true;
    }
    bool Toggle(const Application& app, unsigned i) {
        if (i >= notes.size() || !notes[i]) return false;
        auto plan = *notes[i];
        bool done = !Done(i);
        auto now = app.alarms().Now().value();
        plan.enabled = !done && plan.once_utc > now.utc;
        const auto next = done ? done_mask | (1U << i) : done_mask & ~(1U << i);
        if (!app.storage().SetU32("notes.done", next)) {
            title_status = "完成状态保存失败";
            return false;
        }
        if (!app.alarms().Register(plan)) {
            (void)app.storage().SetU32("notes.done", done_mask);
            title_status = "提醒更新失败";
            return false;
        }
        done_mask = next;
        notes[i] = plan;
        ++revision;
        app.log().Info(done ? "NOTE completed" : "NOTE reopened");
        return true;
    }
    bool Delete(const Application& app, unsigned i) {
        if (i >= notes.size() || !notes[i]) return false;
        if (!app.alarms().Remove(i + 3)) {
            title_status = "删除失败，请重试";
            return false;
        }
        notes[i].reset();
        done_mask &= ~(1U << i);
        (void)app.storage().SetU32("notes.done", done_mask);
        ++revision;
        app.log().Info("NOTE deleted");
        return true;
    }
    AlarmPlan Plan(const WallTime& now) const {
        AlarmPlan plan;
        plan.weekly = daily;
        plan.label.Append("闹钟提醒");
        if (daily) {
            plan.weekdays = 127;
            plan.minute_of_day = uint16_t(hour * 60 + minute);
        } else {
            auto target = calendar::Utc(calendar::LocalDate(now.utc, offset), hour * 60 + minute, offset);
            if (target <= static_cast<int64_t>(now.utc)) target += 86400;
            plan.once_utc = uint64_t(target);
        }
        return plan;
    }
};
inline FixedString<64> TimeText(uint64_t utc, int32_t offset, bool seconds) {
    auto local = calendar::LocalSeconds(utc, offset) % 86400;
    FixedString<64> out;
    auto h = calendar::FormatTime(uint32_t(local) / 60);
    out.Append(h.c_str());
    if (seconds) {
        out.Append(":");
        if (local % 60 < 10) out.Append("0");
        out.AppendInt(local % 60);
    }
    return out;
}
}  // namespace clock_app
