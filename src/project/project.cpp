#include "iris/project/project.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/subtitle/ass_script.hpp"
#include "iris/subtitle/ir_bridge.hpp"
#include "iris/text/cleanup.hpp"
#include "iris/text/karaoke_apply.hpp"
#include "iris/text/rules.hpp"
#include "iris/view/ass_builder.hpp"

namespace iris::project {

namespace fs = std::filesystem;

namespace {

std::int64_t modified_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::int64_t size_of(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(s);
}

std::string path_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string escape_html(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

void tally(Report& r) {
    r.files = static_cast<int>(r.entries.size());
    r.ok = r.needs_karaoke = r.errors = r.skipped = r.failed = 0;
    for (const auto& e : r.entries) {
        if (e.status == "ok") ++r.ok;
        else if (e.status == "needs-karaoke") ++r.needs_karaoke;
        else if (e.status == "skipped") ++r.skipped;
        else if (e.status == "failed") ++r.failed;
        else ++r.errors;
    }
}

} // namespace

nlohmann::json Report::to_json() const {
    nlohmann::json j;
    j["root"] = root;
    if (!error.empty()) j["error"] = error;
    j["files"] = files;
    j["ok"] = ok;
    j["needsKaraoke"] = needs_karaoke;
    j["errors"] = errors;
    j["skipped"] = skipped;
    j["failed"] = failed;
    j["entries"] = nlohmann::json::array();
    for (const auto& e : entries) {
        j["entries"].push_back({{"input", e.input},
                                {"output", e.output},
                                {"size", e.size},
                                {"modified", e.modified},
                                {"events", e.events},
                                {"karaoke", e.karaoke},
                                {"pending", e.pending},
                                {"errors", e.errors},
                                {"warnings", e.warnings},
                                {"status", e.status},
                                {"message", e.message}});
    }
    return j;
}

Report Report::from_json(const nlohmann::json& j) {
    Report r;
    r.root = j.value("root", std::string());
    r.error = j.value("error", std::string());
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& e : j["entries"]) {
            Entry entry;
            entry.input = e.value("input", std::string());
            entry.output = e.value("output", std::string());
            entry.size = e.value("size", std::int64_t{0});
            entry.modified = e.value("modified", std::int64_t{0});
            entry.events = e.value("events", 0);
            entry.karaoke = e.value("karaoke", 0);
            entry.pending = e.value("pending", 0);
            entry.errors = e.value("errors", 0);
            entry.warnings = e.value("warnings", 0);
            entry.status = e.value("status", std::string());
            entry.message = e.value("message", std::string());
            r.entries.push_back(std::move(entry));
        }
    }
    tally(r);
    return r;
}

std::vector<std::string> collect_files(const std::vector<std::string>& inputs, bool recursive) {
    std::set<std::string> files;
    std::error_code ec;
    for (const auto& input : inputs) {
        const auto p = iris::infra::utf8_path(input);
        if (fs::is_directory(p, ec)) {
            if (recursive) {
                for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied,
                                                         ec),
                     end;
                     it != end; it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec)) continue;
                    const std::string ext = it->path().extension().string();
                    if (ext == ".ass" || ext == ".ssa") files.insert(path_utf8(it->path()));
                }
            } else {
                for (fs::directory_iterator it(p, ec), end; it != end; it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec)) continue;
                    const std::string ext = it->path().extension().string();
                    if (ext == ".ass" || ext == ".ssa") files.insert(path_utf8(it->path()));
                }
            }
        } else if (fs::is_regular_file(p, ec)) {
            files.insert(input);
        }
    }
    return {files.begin(), files.end()};
}

Report scan(const std::vector<std::string>& inputs, const ScanOptions& options) {
    Report report;
    if (!inputs.empty()) report.root = inputs.front();
    auto lint_opts = options.lint;

    for (const auto& path : collect_files(inputs, options.recursive)) {
        Entry entry;
        entry.input = path;
        entry.size = size_of(iris::infra::utf8_path(path));
        entry.modified = modified_of(iris::infra::utf8_path(path));

        quality::LintFile file;
        std::string error;
        if (!quality::load_ass_file(path, file, error)) {
            entry.status = "failed";
            entry.message = error;
            report.entries.push_back(std::move(entry));
            continue;
        }
        const auto r = quality::lint_ass(file, lint_opts, {});
        entry.events = r.dialogues;
        entry.karaoke = r.karaoke_lines;
        entry.pending = r.lyric_pending;
        entry.errors = r.errors;
        entry.warnings = r.warnings;
        entry.status = r.errors > 0 ? "error" : (r.lyric_pending > 0 ? "needs-karaoke" : "ok");
        report.entries.push_back(std::move(entry));
    }
    tally(report);
    return report;
}

Report run(const std::vector<std::string>& inputs, const RunOptions& options,
           const std::string& manifest_path) {
    Report report;
    if (!inputs.empty()) report.root = inputs.front();

    text::RuleSet rules;
    text::Glossary glossary;
    bool has_rules = false;
    if (!options.rules_path.empty()) {
        std::string error;
        if (!text::load_rules(options.rules_path, rules, error)) {
            report.error = error;
            return report;
        }
        has_rules = true;
    }
    if (!options.glossary_path.empty()) {
        std::string error;
        if (!text::load_glossary(options.glossary_path, glossary, error)) {
            report.error = error;
            return report;
        }
        has_rules = true;
    }

    // 读旧台账（增量判断）。
    std::map<std::string, Entry> previous;
    if (!manifest_path.empty()) {
        std::ifstream f(iris::infra::utf8_path(manifest_path), std::ios::binary);
        if (f) {
            nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            if (!j.is_discarded()) {
                for (auto& e : Report::from_json(j).entries) previous[e.input] = e;
            }
        }
    }

    const fs::path out_dir = iris::infra::utf8_path(options.out_dir);
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    auto lint_opts = options.lint;
    lint_opts.check_fonts = false; // 批处理不检查本机字体

    for (const auto& path : collect_files(inputs, true)) {
        Entry entry;
        entry.input = path;
        entry.size = size_of(iris::infra::utf8_path(path));
        entry.modified = modified_of(iris::infra::utf8_path(path));

        const fs::path in_path = iris::infra::utf8_path(path);
        const std::string stem = path_utf8(in_path.stem());
        const fs::path out_path = out_dir / iris::infra::utf8_path(stem + ".ass");
        entry.output = path_utf8(out_path);

        const auto it = previous.find(path);
        if (!options.force && it != previous.end() && it->second.size == entry.size &&
            it->second.modified == entry.modified && fs::exists(out_path, ec)) {
            entry = it->second;
            entry.status = "skipped";
            ++report.skipped;
            report.entries.push_back(std::move(entry));
            continue;
        }

        subtitle::AssScript script;
        std::string error;
        if (!subtitle::load_ass_script(path, script, error)) {
            entry.status = "failed";
            entry.message = error;
            ++report.failed;
            report.entries.push_back(std::move(entry));
            continue;
        }

        model::IrDocument doc;
        doc.generator.tool = "iris";
        doc.generator.version = "0.1.0";
        doc.generator.command = "project";
        doc.generator.input_file = path;
        doc.config.input_file_path = path;
        doc.config.command_name = "project";
        doc.state.tracks.push_back(subtitle::track_from_script(script, ""));
        subtitle::apply_presentation(script, doc.state);

        if (has_rules) text::apply_rules_to_document(doc, rules, glossary, 0);
        text::correct_document(doc);

        text::KaraokeApplyOptions karaoke_opts;
        karaoke_opts.fill_even = options.fill_even;
        text::KaraokeApplyStats karaoke_stats;
        text::apply_karaoke(doc, karaoke_opts, karaoke_stats);

        view::AssBuildOptions build_opts;
        build_opts.template_path = options.template_path;
        build_opts.crlf = true;
        view::AssBuildResult built;
        if (!view::build_ass(doc, build_opts, built, error)) {
            entry.status = "failed";
            entry.message = error;
            ++report.failed;
            report.entries.push_back(std::move(entry));
            continue;
        }

        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            entry.status = "failed";
            entry.message = "无法写入 " + entry.output;
            ++report.failed;
            report.entries.push_back(std::move(entry));
            continue;
        }
        out << built.text;
        out.close();

        if (options.keep_ir) {
            try {
                doc.save(path_utf8(out_dir / iris::infra::utf8_path(stem + ".iris.json")));
            } catch (const std::exception&) {
                // IR 写失败不影响成品
            }
        }

        // 对产物做一次体检。
        quality::LintFile linted;
        if (quality::load_ass_file(entry.output, linted, error)) {
            const auto r = quality::lint_ass(linted, lint_opts, {});
            entry.events = r.dialogues;
            entry.karaoke = r.karaoke_lines;
            entry.pending = r.lyric_pending;
            entry.errors = r.errors;
            entry.warnings = r.warnings;
        } else {
            entry.message = error;
        }
        entry.status = entry.errors > 0 ? "error"
                       : entry.pending > 0 ? "needs-karaoke"
                                           : "ok";
        if (!built.warnings.empty() && entry.message.empty())
            entry.message = built.warnings.front();

        if (entry.status == "ok") ++report.ok;
        else if (entry.status == "needs-karaoke") ++report.needs_karaoke;
        else ++report.errors;
        report.entries.push_back(std::move(entry));
    }

    report.files = static_cast<int>(report.entries.size());
    if (!manifest_path.empty()) {
        std::ofstream f(iris::infra::utf8_path(manifest_path), std::ios::binary);
        if (f) f << report.to_json().dump(2) << '\n';
    }
    return report;
}

std::string to_html(const Report& report, const std::string& title) {
    std::ostringstream os;
    os << "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"utf-8\">\n<title>"
       << escape_html(title) << "</title>\n<style>\n"
          "body{font-family:-apple-system,'Segoe UI',Roboto,'Noto Sans SC',sans-serif;"
          "margin:2rem auto;max-width:1200px;padding:0 1rem;color:#1f2328;background:#fff}\n"
          "h1{font-size:1.4rem;margin:0 0 .25rem}.sub{color:#57606a;margin:0 0 1.2rem}\n"
          ".cards{display:flex;flex-wrap:wrap;gap:.75rem;margin:1rem 0}\n"
          ".card{border:1px solid #d0d7de;border-radius:10px;padding:.6rem .9rem;min-width:110px}\n"
          ".card .k{color:#57606a;font-size:.78rem}.card .v{font-size:1.4rem;font-weight:600}\n"
          "table{border-collapse:collapse;width:100%;font-size:.86rem}\n"
          "th,td{border:1px solid #d0d7de;padding:.3rem .45rem;text-align:left}\n"
          "th{background:#f6f8fa}.mono{font-family:ui-monospace,Consolas,monospace}\n"
          ".ok{color:#1a7f37;font-weight:600}.need{color:#9a6700;font-weight:600}\n"
          ".err{color:#cf222e;font-weight:600}.skip{color:#57606a}\n"
          "</style>\n</head>\n<body>\n";
    os << "<h1>" << escape_html(title) << "</h1>\n";
    os << "<p class=\"sub\">" << escape_html(report.root) << " · " << report.files
       << " 个文件</p>\n";

    auto card = [&](const std::string& k, int v) {
        os << "<div class=\"card\"><div class=\"k\">" << escape_html(k) << "</div><div class=\"v\">"
           << v << "</div></div>\n";
    };
    os << "<div class=\"cards\">\n";
    card("完成", report.ok);
    card("待补歌词", report.needs_karaoke);
    card("有错误", report.errors);
    card("跳过（增量）", report.skipped);
    card("失败", report.failed);
    os << "</div>\n";

    os << "<table><tr><th>文件</th><th>事件</th><th>歌词</th><th>待补</th><th>E</th><th>W</th>"
          "<th>状态</th><th>说明</th></tr>\n";
    for (const auto& e : report.entries) {
        const char* cls = e.status == "ok" ? "ok"
                          : e.status == "needs-karaoke" ? "need"
                          : e.status == "skipped" ? "skip"
                                                  : "err";
        os << "<tr><td class=\"mono\">" << escape_html(path_utf8(iris::infra::utf8_path(e.input).filename()))
           << "</td><td>" << e.events << "</td><td>" << e.karaoke << "</td><td>" << e.pending
           << "</td><td>" << e.errors << "</td><td>" << e.warnings << "</td><td class=\"" << cls
           << "\">" << escape_html(e.status) << "</td><td>" << escape_html(e.message)
           << "</td></tr>\n";
    }
    os << "</table>\n</body>\n</html>\n";
    return os.str();
}

} // namespace iris::project
