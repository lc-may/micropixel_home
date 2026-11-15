// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "web.hpp"
namespace netease {
// Read only the known scalar assignments; never execute server-supplied script.
inline std::string ScriptAssignment(std::string_view script, std::string_view name) {
    size_t offset = 0;
    while ((offset = script.find(name, offset)) != script.npos) {
        auto value = Trim(script.substr(offset + name.size()));
        offset += name.size();
        if (!value.starts_with('=')) continue;
        value = Trim(value.substr(1));
        if (value.empty()) return {};
        if (value.front() == '\'' || value.front() == '"') {
            auto quote = value.front();
            value.remove_prefix(1);
            auto end = value.find(quote);
            return end == value.npos ? std::string{} : std::string(value.substr(0, end));
        }
        return std::string(value.substr(0, value.find_first_of("; \t\r\n")));
    }
    return {};
}
inline std::string WechatPollingOrigin(std::string_view page) {
    auto flag = ScriptAssignment(page, "usenewdomain");
    return flag == "1" || flag == "true" ? "https://lp.open.weixin.qq.com" : "https://long.open.weixin.qq.com";
}
}  // namespace netease
