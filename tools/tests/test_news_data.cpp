// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "apps/news/news_client.hpp"

int main(int argc, char** argv) {
    std::vector<news::Article> articles;
    assert(news::NewsClient::Parse(
        R"({"status":"success","data":[{"Title":"新闻标题","HotValue":"21276514"},{"Title":"新闻标题","HotValue":9},{"Title":"第二条新闻","HotValue":10000},{"Title":"bad","HotValue":"oops"}]})",
        articles));
    assert(articles.size() == 2 && articles[0].heat == 21276514 && articles[1].heat == 10000);
    auto first = articles[0].key;
    assert(!news::NewsClient::Parse("<html>blocked</html>", articles) && articles[0].key == first);
    assert(!news::NewsClient::Parse(R"({"status":"failed","data":[]})", articles));
    assert(!news::NewsClient::Parse(R"({"data":[{"Title":"bad","HotValue":"999999999999999999"}]})", articles));
    assert(!news::NewsClient::Parse(R"({"data":[{"Title":"fraction","HotValue":1.5}]})", articles));
    assert(!news::NewsClient::Parse(R"({"data":[{"Title":"negative","HotValue":-3}]})", articles));
    assert(news::NewsClient::Parse(R"({"data":[{"Title":"escaped\nheadline","HotValue":"0"}]})", articles));
    assert(std::string_view(articles[0].title) == "escaped headline" && articles[0].heat == 0);
    assert(news::HeatText(21276514).c_str() == std::string_view("2127.7万"));
    assert(news::HeatText(100000000).c_str() == std::string_view("1.0亿"));
    assert(news::HeatText(99).c_str() == std::string_view("99"));
    assert(news::NewsClient::Parse(R"({"data":[]})", articles) && articles.empty());
    std::string body = R"({"data":[)";
    for (unsigned i = 0; i < 60; ++i) {
        if (i) body += ',';
        body += "{\"Title\":\"item " + std::to_string(i) + "\",\"HotValue\":1}";
    }
    body += "]}";
    assert(news::NewsClient::Parse(body, articles) && articles.size() == news::MaxArticles);
    assert(!micropixel::ui::ValidUtf8(std::string("\xc0\x80", 2)));
    assert(!micropixel::ui::ValidUtf8(std::string("\xed\xa0\x80", 3)));
    auto measure = [](std::string_view s) {
        unsigned width = 0;
        for (uint8_t c : s)
            if ((c & 0xc0) != 0x80) width += c < 128 ? 1 : 2;
        return width;
    };
    auto lines = micropixel::ui::WrapText<2>("中文标题ABCDE再来一段", 8, measure);
    assert(lines[0] == "中文标题" && lines[1].ends_with("…"));
    for (const auto& line : lines) assert(micropixel::ui::ValidUtf8(line) && measure(line) <= 8);
    auto full = micropixel::ui::WrapText<8>("中文标题ABCDE再来一段", 8, measure);
    std::string joined;
    for (const auto& line : full) joined += line;
    assert(joined == "中文标题ABCDE再来一段");
    auto newline = micropixel::ui::WrapText<3>("a\nb", 8, measure);
    assert(newline[0] == "a" && newline[1] == "b");
    if (argc == 2) {
        std::ifstream file(argv[1]);
        std::string live((std::istreambuf_iterator<char>(file)), {});
        assert(news::NewsClient::Parse(live, articles) && articles.size() == 50);
        std::puts("PASS captured live Toutiao response: 50 titles and heat values");
    }
    std::puts(
        "PASS bounded news parsing, deduplication, cache preservation, heat formatting, UTF-8 and measured wrapping");
}
