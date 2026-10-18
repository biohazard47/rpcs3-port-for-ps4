#pragma once

// PKG Store: browse a pkgi-format list (pkgi_games.txt) and download its PS3 packages in the background into
// /data/rpcs3/pkgs, where the XMB lists them and installs them with ps4_install_packages.
//
// Pure logic, standard library and POSIX only, so it builds for the host tests (test/pkgstore_test.cpp) and for the
// console. Nothing here throws (the frontend is built without exceptions). The network is injected as a 'fetcher'
// (util/ps4_pkgstore_http.hpp provides the curl one), and so are the file operations.
//
// The list's RAP / "Download .RAP file" columns are never read: parse_list looks columns up by name and has no code for
// them. Licenses are the player's own (/data/rpcs3/raps).

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace utils::ps4::pkgstore
{
// ------------------------------------------------------------------------------------------------ the list
enum class kind : std::uint8_t { game, dlc, demo, psx, psp, minis, theme, avatar };

struct item
{
    std::string tid, region, name, url, content_id; // region upper case (US EU JP ASIA INT ...); url "" = no direct link
    std::uint64_t size = 0;                         // bytes, 0 = unknown
    kind type = kind::game;
    bool downloadable() const { return !url.empty(); }
};

namespace detail
{
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }
inline char upper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 32) : c; }
inline std::string lowered(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}
inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline bool starts_with_nocase(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != lower(prefix[i])) return false;
    return true;
}
inline int compare_nocase(std::string_view a, std::string_view b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (const int d = int(static_cast<unsigned char>(lower(a[i]))) - int(static_cast<unsigned char>(lower(b[i])))) return d;
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
} // namespace detail

// The kind comes from the trailing "(PSX)", "(DLC)"... of the name, as pkgi's own content types do.
inline kind kind_of(std::string_view name)
{
    name = detail::trim(name);
    if (name.empty() || name.back() != ')') return kind::game;
    const std::size_t open = name.rfind('(');
    if (open == std::string_view::npos) return kind::game;
    const std::string tag = detail::lowered(name.substr(open + 1, name.size() - open - 2));
    static constexpr struct { std::string_view key; kind k; } table[] = {{"psx", kind::psx}, {"psp", kind::psp}, {"minis", kind::minis},
        {"dlc", kind::dlc}, {"theme", kind::theme}, {"demo", kind::demo}, {"avatar", kind::avatar}};
    for (const auto& row : table)
        if (tag.find(row.key) != std::string::npos) return row.k;
    return kind::game;
}

// Tab separated, first line = header. Columns are found by name, in any order; missing ones stay empty.
inline std::vector<item> parse_list(std::string_view text)
{
    std::vector<item> out;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    constexpr std::size_t none = std::string_view::npos;
    std::size_t c_tid = none, c_region = none, c_name = none, c_url = none, c_content = none, c_size = none;
    bool have_header = false;
    std::vector<std::string_view> f;
    while (!text.empty())
    {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == none ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        f.clear();
        for (std::size_t at = 0;;)
        {
            const std::size_t tab = line.find('\t', at);
            f.push_back(line.substr(at, tab == none ? none : tab - at));
            if (tab == none) break;
            at = tab + 1;
        }
        if (!have_header)
        {
            have_header = true;
            for (std::size_t i = 0; i < f.size(); ++i)
            {
                const std::string h = detail::lowered(detail::trim(f[i]));
                if (h == "title id") c_tid = i;
                else if (h == "region") c_region = i;
                else if (h == "name") c_name = i;
                else if (h == "pkg direct link") c_url = i;
                else if (h == "content id") c_content = i;
                else if (h == "file size") c_size = i;
            }
            if (c_name == none) return {};
            continue;
        }
        const auto at = [&](std::size_t column) { return column < f.size() ? detail::trim(f[column]) : std::string_view{}; };
        item it;
        it.name = std::string(at(c_name));
        if (it.name.empty()) continue;
        it.tid = std::string(at(c_tid));
        it.region = std::string(at(c_region));
        for (char& c : it.region) c = detail::upper(c);
        it.content_id = std::string(at(c_content));
        const std::string_view url = at(c_url);
        if (detail::starts_with_nocase(url, "http://") || detail::starts_with_nocase(url, "https://")) it.url = std::string(url);
        const std::string_view size = at(c_size);
        std::uint64_t value = 0;
        if (!size.empty() && std::from_chars(size.data(), size.data() + size.size(), value).ec == std::errc{}) it.size = value;
        it.type = kind_of(it.name);
        out.push_back(std::move(it));
    }
    return out;
}

enum class sort_by : std::uint8_t { name, size_desc, title_id };

struct filter
{
    std::string text;                 // every word must be in the name, the title ID or the content ID
    std::string region;               // "" = all, else the list's code (US EU JP ASIA)
    std::optional<kind> type;
    sort_by order = sort_by::name;
    bool only_downloadable = false;
};

class catalog
{
    std::vector<item> m_items;
    std::vector<std::string> m_hay;
public:
    catalog() = default;
    explicit catalog(std::vector<item> items) : m_items(std::move(items))
    {
        m_hay.reserve(m_items.size());
        for (const item& it : m_items) m_hay.push_back(detail::lowered(it.name + ' ' + it.tid + ' ' + it.content_id));
    }
    const std::vector<item>& items() const { return m_items; }

    // Indexes into items(), in the order asked.
    std::vector<std::uint32_t> find(const filter& f) const
    {
        std::vector<std::string> words;
        const std::string text = detail::lowered(f.text);
        for (std::size_t i = 0; i < text.size();)
        {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            const std::size_t start = i;
            while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
            if (i > start) words.push_back(text.substr(start, i - start));
        }
        std::string region = f.region;
        for (char& c : region) c = detail::upper(c);
        std::vector<std::uint32_t> out;
        for (std::uint32_t i = 0; i < m_items.size(); ++i)
        {
            const item& it = m_items[i];
            if (f.only_downloadable && !it.downloadable()) continue;
            if (!region.empty() && it.region != region) continue;
            if (f.type && it.type != *f.type) continue;
            bool all = true;
            for (const std::string& w : words)
                if (m_hay[i].find(w) == std::string::npos) { all = false; break; }
            if (all) out.push_back(i);
        }
        const auto by_name = [&](std::uint32_t a, std::uint32_t b) { return detail::compare_nocase(m_items[a].name, m_items[b].name) < 0; };
        switch (f.order)
        {
        case sort_by::size_desc:
            std::stable_sort(out.begin(), out.end(), [&](std::uint32_t a, std::uint32_t b)
                { return m_items[a].size != m_items[b].size ? m_items[a].size > m_items[b].size : by_name(a, b); });
            break;
        case sort_by::title_id:
            std::stable_sort(out.begin(), out.end(), [&](std::uint32_t a, std::uint32_t b)
                { return m_items[a].tid != m_items[b].tid ? m_items[a].tid < m_items[b].tid : by_name(a, b); });
            break;
        default: std::stable_sort(out.begin(), out.end(), by_name); break;
        }
        return out;
    }
};

// ------------------------------------------------------------------------------------------------ URLs and names
struct url_parts
{
    bool ok = false, secure = false;
    std::string host, path;
    std::uint16_t port = 0;
};

// http(s)://host[:port]/path (no IPv6 literals, no user info).
inline url_parts parse_url(std::string_view url)
{
    url_parts u;
    std::size_t at = 0;
    if (detail::starts_with_nocase(url, "https://")) { u.secure = true; at = 8; }
    else if (detail::starts_with_nocase(url, "http://")) at = 7;
    else return u;
    const std::size_t end = url.find_first_of("/?#", at);
    std::string_view authority = url.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    u.path = end == std::string_view::npos ? "/" : std::string(url.substr(end));
    if (u.path[0] != '/') u.path.insert(u.path.begin(), '/');
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('[') != std::string_view::npos) return u;
    unsigned port = u.secure ? 443 : 80;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos)
    {
        const std::string_view digits = authority.substr(colon + 1);
        unsigned parsed = 0;
        if (digits.empty() || std::from_chars(digits.data(), digits.data() + digits.size(), parsed).ec != std::errc{} || parsed == 0 || parsed > 65535) return u;
        port = parsed;
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) return u;
    u.host = std::string(authority);
    u.port = static_cast<std::uint16_t>(port);
    u.ok = true;
    return u;
}

// "NPUA30002_Bejeweled_2": only [A-Za-z0-9._-], at most 100 bytes, never empty.
inline std::string file_stem(const item& it)
{
    std::string raw = (it.tid.empty() ? std::string("PKG") : it.tid) + "_" + it.name;
    std::string out;
    bool gap = false;
    for (const char c : raw)
    {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (plain) { if (gap && !out.empty()) out += '_'; out += c; gap = false; }
        else gap = true;
        if (out.size() >= 100) break;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    return out.empty() ? std::string("PKG") : out;
}

// ------------------------------------------------------------------------------------------------ downloads
enum class state : std::uint8_t { queued, downloading, verifying, done, failed, cancelled };
enum class fail_reason : std::uint8_t { none, no_link, already_there, no_thread, offline, http, no_space, size_mismatch, disk, other };

struct config
{
    std::string pkg_dir = "/data/rpcs3/pkgs/";           // what the XMB lists; a finished file is renamed here
    std::string part_dir = "/data/rpcs3/data/pkgstore/"; // unfinished files: outside every folder the library scans
    bool delete_on_cancel = true;
    std::uint64_t reserve_bytes = 256ull << 20;          // kept free besides the download itself
};

struct fetch_request
{
    std::string url, part_path;
    std::uint64_t resume_from = 0;                          // bytes already in part_path
    std::function<bool()> cancelled;                        // poll; true = stop as soon as possible
    std::function<void(std::uint64_t done, std::uint64_t total)> progress; // totals include resume_from; 0 = unknown
};

struct fetch_result
{
    enum class code : std::uint8_t { ok, cancelled, offline, http_error, failed } status = code::failed;
    long http = 0;
    std::uint64_t expected_total = 0; // size the server announced (resume_from included), 0 = unknown
    bool disk_error = false;          // writing the part file failed (a full /data above all)
    std::string detail;               // technical text for the log
};

using fetcher = std::function<fetch_result(const fetch_request&)>;

struct file_ops
{
    std::function<bool(const std::string& dir)> make_dirs;                         // mkdir -p
    std::function<std::int64_t(const std::string& path)> file_size;                // -1 = missing
    std::function<bool(const std::string& from, const std::string& to)> rename;
    std::function<void(const std::string& path)> remove;
    std::function<std::int64_t(const std::string& dir)> free_bytes;                // -1 = unknown
};

inline file_ops posix_file_ops()
{
    file_ops ops;
    ops.make_dirs = [](const std::string& dir)
    {
        for (std::size_t i = 1; i <= dir.size(); ++i)
            if (i == dir.size() || dir[i] == '/')
            {
                const std::string part = dir.substr(0, i);
                struct stat st{};
                if (::stat(part.c_str(), &st) != 0 && ::mkdir(part.c_str(), 0777) != 0 && ::stat(part.c_str(), &st) != 0) return false;
            }
        return true;
    };
    ops.file_size = [](const std::string& path) -> std::int64_t
    {
        struct stat st{};
        return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? std::int64_t(st.st_size) : -1;
    };
    ops.rename = [](const std::string& a, const std::string& b) { return std::rename(a.c_str(), b.c_str()) == 0; };
    ops.remove = [](const std::string& p) { std::remove(p.c_str()); };
    ops.free_bytes = [](const std::string&) -> std::int64_t { return -1; };
    return ops;
}

struct job_view
{
    std::uint32_t id = 0;
    std::string name, tid;
    state st = state::queued;
    std::uint64_t done = 0, total = 0;
    double speed = 0;               // bytes per second
    fail_reason why = fail_reason::none;
    std::string detail;             // technical text (HTTP code, byte counts, curl text)
    bool active() const { return st == state::queued || st == state::downloading || st == state::verifying; }
};

// Runs 'work' on a detached thread; false when none could be created (the console reports the per-process cap as an
// error code where std::thread would end the app).
inline bool run_detached(std::function<void()> work)
{
    auto* owned = new (std::nothrow) std::function<void()>(std::move(work));
    if (!owned) return false;
    pthread_t thread{};
    const auto entry = [](void* arg) -> void*
    {
        const std::unique_ptr<std::function<void()>> held(static_cast<std::function<void()>*>(arg));
        (*held)();
        return nullptr;
    };
    if (::pthread_create(&thread, nullptr, +entry, owned) != 0)
    {
        delete owned;
        return false;
    }
    ::pthread_detach(thread);
    return true;
}

// One worker thread, one download at a time (like pkgi), started on the first enqueue.
class manager
{
    struct job
    {
        std::uint32_t id = 0;
        item it;
        state st = state::queued;
        std::uint64_t done = 0, total = 0;
        double speed = 0;
        fail_reason why = fail_reason::none;
        std::string detail, part, dest;
        std::atomic<bool> cancel{false};
    };
    struct core
    {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::shared_ptr<job>> queue;
        std::vector<std::shared_ptr<job>> jobs;
        std::uint32_t seq = 0;
        bool quit = false, worker = false;
        config cfg;
        file_ops files;
        fetcher fetch;
        std::function<void(const job_view&)> on_finished; // called on the worker thread when a file lands in pkg_dir
        std::function<std::int64_t()> clock_ms;
    };
    std::shared_ptr<core> m_core;

    static job_view view_of(const job& j)
    {
        return {j.id, j.it.name, j.it.tid, j.st, j.done, j.total, j.speed, j.why, j.detail};
    }
    static void finish(core& c, const std::shared_ptr<job>& j, state st, fail_reason why = fail_reason::none, std::string detail = {})
    {
        std::lock_guard lock(c.m);
        j->st = st;
        j->why = why;
        j->detail = std::move(detail);
        j->speed = 0;
    }

    static void run(core& c, const std::shared_ptr<job>& j)
    {
        if (j->cancel) { finish(c, j, state::cancelled); return; }
        if (!c.files.make_dirs(c.cfg.part_dir) || !c.files.make_dirs(c.cfg.pkg_dir)) { finish(c, j, state::failed, fail_reason::disk, "mkdir"); return; }
        const std::int64_t existing = c.files.file_size(j->part);
        const std::uint64_t have = existing > 0 ? std::uint64_t(existing) : 0;
        if (j->it.size && c.files.free_bytes)
        {
            const std::int64_t free_now = c.files.free_bytes(c.cfg.pkg_dir);
            const std::uint64_t need = (j->it.size > have ? j->it.size - have : 0) + c.cfg.reserve_bytes;
            if (free_now >= 0 && std::uint64_t(free_now) < need)
            {
                finish(c, j, state::failed, fail_reason::no_space, "free " + std::to_string(free_now) + ", need " + std::to_string(need));
                return;
            }
        }
        {
            std::lock_guard lock(c.m);
            j->st = state::downloading;
            j->done = have;
            j->total = j->it.size;
        }
        std::int64_t mark_at = c.clock_ms ? c.clock_ms() : 0;
        std::uint64_t mark_done = have;
        fetch_request rq;
        rq.url = j->it.url;
        rq.part_path = j->part;
        rq.resume_from = have;
        rq.cancelled = [j] { return j->cancel.load(); };
        rq.progress = [&c, j, mark_at, mark_done](std::uint64_t done, std::uint64_t total) mutable
        {
            const std::int64_t now = c.clock_ms ? c.clock_ms() : 0;
            std::lock_guard lock(c.m);
            j->done = done;
            if (total) j->total = total;
            if (now - mark_at >= 1000)
            {
                j->speed = double(done - std::min(done, mark_done)) * 1000.0 / double(now - mark_at);
                mark_at = now;
                mark_done = done;
            }
        };
        const fetch_result r = c.fetch(rq);
        using code = fetch_result::code;
        switch (r.status)
        {
        case code::cancelled:
            if (c.cfg.delete_on_cancel) c.files.remove(j->part);
            finish(c, j, state::cancelled);
            return;
        case code::offline: finish(c, j, state::failed, fail_reason::offline, r.detail); return;
        case code::http_error:
            if (r.http == 403 || r.http == 404 || r.http == 410 || r.http == 416) c.files.remove(j->part); // that file will not resume
            finish(c, j, state::failed, fail_reason::http, "HTTP " + std::to_string(r.http) + (r.detail.empty() ? "" : " " + r.detail));
            return;
        case code::failed: finish(c, j, state::failed, r.disk_error ? fail_reason::no_space : fail_reason::other, r.detail); return;
        case code::ok: break;
        }
        {
            std::lock_guard lock(c.m);
            j->st = state::verifying;
        }
        const std::int64_t size = c.files.file_size(j->part);
        const std::uint64_t expected = r.expected_total ? r.expected_total : j->it.size; // the server beats the list
        if (size < 0 || (expected && std::uint64_t(size) != expected))
        {
            const std::string text = "got " + std::to_string(size) + ", expected " + std::to_string(expected);
            if (size > 0 && expected && std::uint64_t(size) > expected) c.files.remove(j->part); // cannot be resumed
            finish(c, j, state::failed, fail_reason::size_mismatch, text);
            return;
        }
        if (!c.files.rename(j->part, j->dest)) { finish(c, j, state::failed, fail_reason::disk, "rename"); return; }
        job_view done_view;
        {
            std::lock_guard lock(c.m);
            j->st = state::done;
            j->done = j->total = std::uint64_t(size);
            j->speed = 0;
            done_view = view_of(*j);
        }
        if (c.on_finished) c.on_finished(done_view);
    }

    static void worker(std::shared_ptr<core> c)
    {
        for (;;)
        {
            std::shared_ptr<job> j;
            {
                std::unique_lock lock(c->m);
                c->cv.wait(lock, [&] { return c->quit || !c->queue.empty(); });
                if (c->quit) return;
                j = c->queue.front();
                c->queue.pop_front();
            }
            run(*c, j);
        }
    }

public:
    manager(config cfg, fetcher fetch, file_ops files = posix_file_ops(), std::function<std::int64_t()> clock_ms = nullptr,
        std::function<void(const job_view&)> on_finished = nullptr)
        : m_core(std::make_shared<core>())
    {
        m_core->cfg = std::move(cfg);
        m_core->fetch = std::move(fetch);
        m_core->files = std::move(files);
        m_core->on_finished = std::move(on_finished);
        m_core->clock_ms = clock_ms ? std::move(clock_ms) : [] {
            return std::int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    }
    ~manager()
    {
        {
            std::lock_guard lock(m_core->m);
            m_core->quit = true;
            for (auto& j : m_core->jobs) j->cancel = true;
        }
        m_core->cv.notify_all();
    }
    manager(const manager&) = delete;
    manager& operator=(const manager&) = delete;

    // The job id, or 0 with 'why' saying so: no direct link, the file is already in pkg_dir, no thread. A download of the
    // same link that is still running is returned as it is.
    std::uint32_t enqueue(const item& it, fail_reason* why = nullptr)
    {
        const auto refuse = [&](fail_reason r) { if (why) *why = r; return std::uint32_t{0}; };
        if (!it.downloadable() || !parse_url(it.url).ok) return refuse(fail_reason::no_link);
        const std::string stem = file_stem(it);
        const std::string dest = m_core->cfg.pkg_dir + stem + ".pkg";
        if (m_core->files.file_size(dest) >= 0) return refuse(fail_reason::already_there);
        std::lock_guard lock(m_core->m);
        for (const auto& j : m_core->jobs)
            if (j->it.url == it.url && (j->st == state::queued || j->st == state::downloading || j->st == state::verifying)) return j->id;
        if (!m_core->worker)
        {
            std::shared_ptr<core> c = m_core;
            if (!run_detached([c] { worker(c); })) return refuse(fail_reason::no_thread);
            m_core->worker = true;
        }
        auto j = std::make_shared<job>();
        j->id = ++m_core->seq;
        j->it = it;
        j->dest = dest;
        j->part = m_core->cfg.part_dir + stem + ".part";
        j->total = it.size;
        m_core->jobs.push_back(j);
        m_core->queue.push_back(j);
        m_core->cv.notify_one();
        return j->id;
    }

    void cancel(std::uint32_t id)
    {
        std::lock_guard lock(m_core->m);
        for (auto& j : m_core->jobs)
            if (j->id == id && (j->st == state::queued || j->st == state::downloading || j->st == state::verifying))
            {
                j->cancel = true;
                if (j->st == state::queued) j->st = state::cancelled; // the worker skips it
            }
    }

    // Newest first.
    std::vector<job_view> snapshot() const
    {
        std::lock_guard lock(m_core->m);
        std::vector<job_view> out;
        for (auto it = m_core->jobs.rbegin(); it != m_core->jobs.rend(); ++it) out.push_back(view_of(**it));
        return out;
    }

    bool any_active() const
    {
        std::lock_guard lock(m_core->m);
        for (const auto& j : m_core->jobs)
            if (j->st == state::queued || j->st == state::downloading || j->st == state::verifying) return true;
        return false;
    }

    void clear_finished()
    {
        std::lock_guard lock(m_core->m);
        std::erase_if(m_core->jobs, [](const std::shared_ptr<job>& j)
            { return j->st == state::done || j->st == state::failed || j->st == state::cancelled; });
    }
};
} // namespace utils::ps4::pkgstore
