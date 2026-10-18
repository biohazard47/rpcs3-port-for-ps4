// Host test: g++ -std=c++20 -fno-exceptions -Wall -Wextra -pthread -I.. pkgstore_test.cpp -o t && ./t [pkgi_games.txt]
#include "util/ps4_pkgstore.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace utils::ps4::pkgstore;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; } } while (0)

static void wait_idle(manager& m) { for (int i = 0; i < 400 && m.any_active(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

int main(int argc, char** argv)
{
    // ---- parser
    const std::string db = "\xEF\xBB\xBFTitle ID\tRegion\tName\tPKG direct link\tRAP\tContent ID\tLast Modification Date\tDownload .RAP file\tFile Size\tSHA256\r\n"
        "NPUA1\tus\tAlpha Game\thttp://h.example/a.pkg\tNOT REQUIRED\tCID1\t\t\t1000\t\r\n"
        "NPEA2\tEU\tBeta Pack (DLC)\thttps://h.example:8443/b.pkg\tSECRETRAPKEY0123\tCID2\t\t\t2000\t\r\n"
        "NPJA3\tJP\tGamma (PSX)\tMISSING\tMISSING\tCID3\t\t\t\t\r\n"
        "\r\n"
        "NPJA4\tJP\t\thttp://x/y.pkg\t\t\t\t\t5\t\r\n"      // no name: skipped
        "SHORT\tUS\tShort Row\r\n";                          // fewer fields: tolerated
    const std::vector<item> items = parse_list(db);
    CHECK(items.size() == 4);
    CHECK(items[0].region == "US" && items[0].size == 1000 && items[0].downloadable());
    CHECK(items[1].type == kind::dlc && parse_url(items[1].url).port == 8443 && parse_url(items[1].url).secure);
    CHECK(items[2].type == kind::psx && !items[2].downloadable() && items[2].size == 0);
    CHECK(items[3].name == "Short Row" && !items[3].downloadable());
    for (const item& it : items) CHECK((it.name + it.tid + it.url + it.content_id + it.region).find("SECRETRAP") == std::string::npos);
    CHECK(parse_list("garbage without the name column\nx\ty").empty());

    // ---- search
    const catalog cat(items);
    CHECK(cat.find({"alpha"}).size() == 1);
    CHECK(cat.find({"ALPHA game"}).size() == 1 && cat.find({"alpha zzz"}).empty());
    CHECK(cat.find({"npea2"}).size() == 1 && cat.find({"cid3"}).size() == 1);
    { filter f; f.region = "jp"; CHECK(cat.find(f).size() == 1); }
    { filter f; f.type = kind::dlc; CHECK(cat.find(f).size() == 1); }
    { filter f; f.only_downloadable = true; CHECK(cat.find(f).size() == 2); }
    { filter f; f.order = sort_by::size_desc; const auto r = cat.find(f); CHECK(items[r[0]].name == "Beta Pack (DLC)"); }
    { filter f; const auto r = cat.find(f); CHECK(items[r[0]].name == "Alpha Game" && items[r[3]].name == "Short Row"); }

    // ---- urls and names
    CHECK(parse_url("http://zeus.dl.playstation.net/cdn/x.pkg").port == 80);
    CHECK(!parse_url("ftp://a/b").ok && !parse_url("http://u@h/x").ok && !parse_url("http://h:0/x").ok && !parse_url("http:///x").ok && !parse_url("http://[::1]/x").ok);
    CHECK(parse_url("http://h").path == "/" && parse_url("http://h?q=1").path == "/?q=1");
    CHECK(file_stem({"NPUA30002", "US", "Bejeweled 2: Deluxe/Edition", "", ""}) == "NPUA30002_Bejeweled_2_Deluxe_Edition");
    CHECK(file_stem({"", "", "../../etc/passwd", "", ""}).find('/') == std::string::npos);
    CHECK(file_stem({"X", "", std::string(300, 'a'), "", ""}).size() <= 100 && file_stem({"", "", "", "", ""}) == "PKG");

    // ---- downloads with a fake network and the real file system under /tmp
    const std::string root = "/tmp/pkgstore_test/";
    std::system(("rm -rf " + root).c_str());
    config cfg; cfg.pkg_dir = root + "pkgs/"; cfg.part_dir = root + "part/"; cfg.reserve_bytes = 0;
    const std::string payload(5000, 'x');
    std::atomic<int> calls{0}; std::atomic<std::uint64_t> last_resume{0};
    std::atomic<int> finished{0};
    enum class mode { good, offline, http404, longer, slow } how = mode::good;
    const fetcher fake = [&](const fetch_request& rq) -> fetch_result
    {
        ++calls; last_resume = rq.resume_from;
        fetch_result r;
        if (how == mode::offline) { r.status = fetch_result::code::offline; r.detail = "DNS"; return r; }
        if (how == mode::http404) { r.status = fetch_result::code::http_error; r.http = 404; return r; }
        std::FILE* f = std::fopen(rq.part_path.c_str(), rq.resume_from ? "ab" : "wb");
        std::uint64_t at = rq.resume_from;
        const std::uint64_t end = how == mode::longer ? payload.size() + 77 : payload.size();
        while (at < end)
        {
            if (rq.cancelled()) { std::fclose(f); r.status = fetch_result::code::cancelled; return r; }
            const std::uint64_t n = std::min<std::uint64_t>(500, end - at);
            std::fwrite(payload.data(), 1, std::min<std::uint64_t>(n, payload.size()), f);
            if (end > payload.size() && n > payload.size()) std::fwrite("y", 1, n - payload.size(), f);
            at += n;
            rq.progress(at, end);
            if (how == mode::slow) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::fclose(f);
        r.status = fetch_result::code::ok; r.expected_total = end; return r;
    };
    item good{"NPUA1", "US", "Alpha Game", "http://h.example/a.pkg", "CID1", 5000};
    std::int64_t fake_time = 0;
    manager m(cfg, fake, posix_file_ops(), [&] { return fake_time += 400; }, [&](const job_view&) { ++finished; });

    fail_reason why = fail_reason::none;
    CHECK(m.enqueue(items[2], &why) == 0 && why == fail_reason::no_link);
    const std::uint32_t id1 = m.enqueue(good);
    CHECK(id1 != 0); wait_idle(m);
    auto snap = m.snapshot();
    CHECK(snap.size() == 1 && snap[0].st == state::done && snap[0].done == 5000 && finished == 1);
    CHECK(posix_file_ops().file_size(cfg.pkg_dir + "NPUA1_Alpha_Game.pkg") == 5000 && posix_file_ops().file_size(cfg.part_dir + "NPUA1_Alpha_Game.part") < 0);
    CHECK(m.enqueue(good, &why) == 0 && why == fail_reason::already_there);   // already installed in pkgs/

    // resume: half a part file is left, the fetcher is asked to continue at 1234
    m.clear_finished(); std::remove((cfg.pkg_dir + "NPUA1_Alpha_Game.pkg").c_str());
    { std::FILE* f = std::fopen((cfg.part_dir + "NPUA1_Alpha_Game.part").c_str(), "wb"); std::fwrite(payload.data(), 1, 1234, f); std::fclose(f); }
    m.enqueue(good); wait_idle(m);
    CHECK(last_resume == 1234 && m.snapshot()[0].st == state::done && posix_file_ops().file_size(cfg.pkg_dir + "NPUA1_Alpha_Game.pkg") == 5000);

    // failures keep or drop the part as designed
    item other{"NPUA9", "US", "Other", "http://h.example/o.pkg", "", 5000};
    how = mode::offline; m.enqueue(other); wait_idle(m);
    CHECK(m.snapshot()[0].why == fail_reason::offline && m.snapshot()[0].st == state::failed);
    how = mode::http404; m.enqueue(other); wait_idle(m);
    CHECK(m.snapshot()[0].why == fail_reason::http && m.snapshot()[0].detail.find("404") != std::string::npos);
    how = mode::longer; m.enqueue(other); wait_idle(m);   // server says 5077 and sends 5077, the list says 5000: the server wins
    CHECK(m.snapshot()[0].st == state::done);
    std::remove((cfg.pkg_dir + "NPUA9_Other.pkg").c_str());
    item wrong{"NPUA8", "US", "Wrong Size", "http://h.example/w.pkg", "", 4000};
    how = mode::good;
    const fetcher no_total = [&](const fetch_request& rq) { auto r = fake(rq); r.expected_total = 0; return r; };
    manager m2(cfg, no_total, posix_file_ops(), nullptr, nullptr);
    m2.enqueue(wrong); wait_idle(m2);   // no Content-Length: the list's 4000 is checked and 5000 arrived
    CHECK(m2.snapshot()[0].why == fail_reason::size_mismatch && m2.snapshot()[0].detail.find("got 5000, expected 4000") != std::string::npos);
    CHECK(posix_file_ops().file_size(cfg.part_dir + "NPUA8_Wrong_Size.part") < 0);   // longer than expected: dropped

    // free space
    file_ops tight = posix_file_ops(); tight.free_bytes = [](const std::string&) -> std::int64_t { return 100; };
    manager m3(cfg, fake, tight, nullptr, nullptr);
    item big{"NPUA7", "US", "Big", "http://h.example/big.pkg", "", 5000};
    m3.enqueue(big); wait_idle(m3);
    CHECK(m3.snapshot()[0].why == fail_reason::no_space);

    // a failed write is reported as a full disk
    const fetcher full = [](const fetch_request&) { fetch_result r; r.status = fetch_result::code::failed; r.disk_error = true; return r; };
    manager m5(cfg, full, posix_file_ops(), nullptr, nullptr);
    m5.enqueue(big); wait_idle(m5);
    CHECK(m5.snapshot()[0].why == fail_reason::no_space);

    // cancel a running download and a queued one; speed is reported
    how = mode::slow;
    item s1{"NPUS1", "US", "Slow One", "http://h.example/s1.pkg", "", 5000}, s2{"NPUS2", "US", "Slow Two", "http://h.example/s2.pkg", "", 5000};
    manager m4(cfg, fake, posix_file_ops(), [&] { return fake_time += 400; }, nullptr);
    const auto a = m4.enqueue(s1), b = m4.enqueue(s2);
    CHECK(m4.enqueue(s1) == a);                       // same link: same job
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    bool saw_speed = false; for (auto& v : m4.snapshot()) if (v.id == a && v.st == state::downloading && v.speed > 0) saw_speed = true;
    CHECK(saw_speed);
    m4.cancel(b); m4.cancel(a); wait_idle(m4);
    for (auto& v : m4.snapshot()) CHECK(v.st == state::cancelled);
    CHECK(posix_file_ops().file_size(cfg.part_dir + "NPUS1_Slow_One.part") < 0);   // delete_on_cancel
    m4.clear_finished(); CHECK(m4.snapshot().empty() && !m4.any_active());

    // ---- the real list, when given
    if (argc > 1)
    {
        std::ifstream in(argv[1], std::ios::binary); std::stringstream ss; ss << in.rdbuf();
        const catalog real(parse_list(ss.str()));
        std::size_t with_link = 0; for (const item& it : real.items()) with_link += it.downloadable();
        std::cout << "real list: " << real.items().size() << " items, " << with_link << " with a link\n";
        CHECK(real.items().size() == 7909 && with_link == 5514);
        const auto r = real.find({"bejeweled"}); CHECK(r.size() >= 3 && real.items()[r[0]].name.starts_with("Bejeweled"));
    }
    std::cout << (failures ? "FAILED\n" : "ALL OK\n");
    return failures != 0;
}
