// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "apps/news/article_client.hpp"

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
    std::string long_text;
    for (unsigned i = 0; i < 300; ++i) long_text += "中文ABC";
    size_t offset = 0;
    while (offset < long_text.size()) {
        auto n = micropixel::ui::FittingTextPrefix(std::string_view(long_text).substr(offset), 8, measure);
        assert(n && micropixel::ui::ValidUtf8(std::string_view(long_text).substr(offset, n)));
        assert(measure(std::string_view(long_text).substr(offset, n)) <= 8);
        offset += n;
    }
    const std::string path = "2026/11/17/ARTItest123261117.shtml";
    assert(news::NewsClient::ValidCctvPath(path));
    assert(!news::NewsClient::ValidCctvPath("2026/11/17/../../secret.shtml"));
    assert(news::NewsClient::ParseCctv(
        "news({\"data\":{\"list\":[{\"title\":\"正文新闻\",\"url\":\"https://news.cctv.com/" + path +
            "\"},{\"title\":\"outside\",\"url\":\"https://other.example/" + path + "\"}]}});",
        articles));
    assert(articles.size() == 1 && articles[0].path == path && articles[0].key == news::TitleKey(path));
    assert(!news::NewsClient::ParseCctv("news({\"data\":{\"list\":[]}});alert(1)", articles));
    assert(news::NewsClient::ParseCctv("{\"data\":{\"list\":[]}}", articles) && articles.empty());
    news::Document doc;
    assert(news::ParseDocument(
        R"(<html><meta name="source" content="测试来源"><div id="content_area"><p>首段 &amp; &#x4E2D;&#25991;。</p><script>hidden()</script><style>hide</style><p>第二段<strong>加粗</strong>&ldquo;引号&rdquo;</p></div><footer>不得读到页脚</footer></html>)",
        doc));
    assert(doc.text == "首段 & 中文。\n第二段加粗“引号”" && doc.source == "测试来源");
    assert(news::ParseDocument(
        R"(<script>var contentdate = '<p>正文第一段。</p><p>第二段\u4e2d\u6587 &nbsp; &mdash; &#128512;。</p>';</script><div id="content_area"></div>)",
        doc));
    assert(doc.text.find("正文第一段。\n第二段中文") == 0 && doc.text.find("😀") != doc.text.npos);
    const auto saved_doc = doc.text;
    assert(!news::ParseDocument("<html><nav>not an article</nav></html>", doc) && doc.text == saved_doc);
    assert(news::ParseDocument(
               "<script>var contentdate = '<p>[!--begin:htmlVideoCode--]abc123[!--end:htmlVideoCode--]</p>';</script>",
               doc) &&
           doc.media_only && doc.text.empty());
    assert(news::ParseDocument("<div id=\"content_area\"><p>" + std::string(40000, 'a') + "</p></div>", doc) &&
           doc.truncated && doc.text.size() == news::MaxArticleText);
    if (argc >= 2) {
        std::ifstream file(argv[1]);
        std::string live((std::istreambuf_iterator<char>(file)), {});
        assert(news::NewsClient::Parse(live, articles) && articles.size() == 50);
        std::puts("PASS captured live Toutiao response: 50 titles and heat values");
    }
    if (argc >= 3) {
        std::ifstream file(argv[2]);
        std::string live((std::istreambuf_iterator<char>(file)), {});
        assert(news::NewsClient::ParseCctv(live, articles) && !articles.empty());
        std::puts("PASS captured live CCTV news feed and trusted article paths");
    }
    if (argc >= 4) {
        std::ifstream file(argv[3]);
        std::string live((std::istreambuf_iterator<char>(file)), {});
        assert(news::ParseDocument(live, doc) && doc.text.size() > 100 && !doc.media_only);
        std::puts("PASS captured live CCTV article text extraction");
    }
    std::puts(
        "PASS news sources, JSONP, trusted paths, HTML/quoted data/entity extraction, media/size bounds, UTF-8 "
        "wrapping and cache preservation");
}
