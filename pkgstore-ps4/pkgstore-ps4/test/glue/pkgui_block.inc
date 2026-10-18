// 0.40: PKG Store. A pkgi-style list (/data/rpcs3/pkgi_games.txt) whose packages download in the background into
// /data/rpcs3/pkgs, where the games column lists them and X installs them. The logic is in util/ps4_pkgstore.hpp, the
// curl fetcher in util/ps4_pkgstore_http.hpp; this block is the XMB side: rows, filters and the texts.
namespace pkgui
{
namespace ps = utils::ps4::pkgstore;

constexpr const char* list_path = "/data/rpcs3/pkgi_games.txt";
constexpr std::size_t max_rows = 300; // hits shown at once; the search narrows the rest

struct store_state
{
    ps::catalog catalog;
    std::unique_ptr<ps::manager> manager;
    ps::filter filter;
    std::vector<std::uint32_t> hits;
    bool loaded = false;
    std::string error;
    int region = 0, type = 0;                // positions in the cycles below
    std::atomic<bool> landed{false};         // set by the worker when a file reaches /data/rpcs3/pkgs
    std::vector<std::uint32_t> logged;       // finished jobs already written to startup.log
};

// Never destroyed: a download goes on while a game runs, and its callback keeps pointing here.
inline store_state& store()
{
    static store_state* const state = new store_state;
    return *state;
}

inline const char* const region_codes[] = {"", "US", "EU", "JP", "ASIA"};
inline constexpr int region_count = 5;
inline constexpr int type_count = 9; // all, then the eight kinds in the order of ps::kind

inline std::string region_label(int i)
{
    switch (i)
    {
    case 1: return store_tr("EUA", "USA");
    case 2: return store_tr("Europa", "Europe");
    case 3: return store_tr("Japão", "Japan");
    case 4: return store_tr("Ásia", "Asia");
    default: return store_tr("Todas", "All");
    }
}

inline std::string type_label(int i)
{
    switch (i)
    {
    case 1: return store_tr("Jogos", "Games");
    case 2: return "DLC";
    case 3: return store_tr("Demos", "Demos");
    case 4: return "PSX";
    case 5: return "PSP";
    case 6: return "Minis";
    case 7: return store_tr("Temas", "Themes");
    case 8: return store_tr("Avatares", "Avatars");
    default: return store_tr("Todos", "All");
    }
}

inline std::string order_label(ps::sort_by order)
{
    switch (order)
    {
    case ps::sort_by::size_desc: return store_tr("Tamanho", "Size");
    case ps::sort_by::title_id: return "Title ID";
    default: return store_tr("Nome", "Name");
    }
}

inline std::string kind_label(ps::kind k)
{
    switch (k)
    {
    case ps::kind::dlc: return "DLC";
    case ps::kind::demo: return "Demo";
    case ps::kind::psx: return "PSX";
    case ps::kind::psp: return "PSP";
    case ps::kind::minis: return "Minis";
    case ps::kind::theme: return store_tr("Tema", "Theme");
    case ps::kind::avatar: return store_tr("Avatar", "Avatar");
    default: return store_tr("Jogo", "Game");
    }
}

inline std::string size_label(std::uint64_t bytes)
{
    if (!bytes) return "?";
    const char* unit[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int u = 0;
    while (value >= 1024.0 && u < 3) { value /= 1024.0; ++u; }
    char text[32];
    std::snprintf(text, sizeof(text), u ? "%.1f %s" : "%.0f %s", value, unit[u]);
    return text;
}

inline std::string hints()
{
    return store_tr("X: baixar  Quadrado: pesquisar  L1/R1: região  L2/R2: tipo  Triângulo: ordem  Círculo: voltar",
        "X: download  Square: search  L1/R1: region  L2/R2: type  Triangle: sort  Circle: back");
}

inline std::string fail_label(const ps::job_view& v)
{
    switch (v.why)
    {
    case ps::fail_reason::offline: return store_tr("sem rede ou DNS", "no network or DNS");
    case ps::fail_reason::http: return shorten(v.detail, 40);
    case ps::fail_reason::no_space: return store_tr("sem espaço em /data", "not enough space on /data");
    case ps::fail_reason::size_mismatch: return store_tr("tamanho não confere", "size does not match");
    case ps::fail_reason::disk: return store_tr("erro de disco", "disk error");
    default: return shorten(v.detail.empty() ? std::string("?") : v.detail, 50);
    }
}

inline std::string job_label(const ps::job_view& v)
{
    switch (v.st)
    {
    case ps::state::queued: return store_tr("Na fila (X cancela)", "Queued (X cancels)");
    case ps::state::downloading:
    {
        const unsigned pct = v.total ? static_cast<unsigned>(std::min<std::uint64_t>(100, v.done * 100 / v.total)) : 0;
        std::string text = store_tr("Baixando ", "Downloading ") + std::to_string(pct) + "%  " + size_label(v.done) + " / " + size_label(v.total);
        if (v.speed > 0) text += "  " + size_label(static_cast<std::uint64_t>(v.speed)) + "/s";
        return text + store_tr("  (X cancela)", "  (X cancels)");
    }
    case ps::state::verifying: return store_tr("Verificando...", "Checking...");
    case ps::state::done: return store_tr("Concluído: instale pela coluna Jogos", "Done: install it from the Games column");
    case ps::state::cancelled: return store_tr("Cancelado (X limpa)", "Cancelled (X clears)");
    default: return store_tr("Falhou: ", "Failed: ") + fail_label(v) + store_tr(" (X limpa)", " (X clears)");
    }
}

inline void apply(store_state& s)
{
    s.filter.region = region_codes[s.region];
    s.filter.type = s.type == 0 ? std::nullopt : std::optional<ps::kind>(static_cast<ps::kind>(s.type - 1));
    s.hits = s.catalog.find(s.filter);
}

inline void load(store_state& s)
{
    s.loaded = false;
    s.error.clear();
    s.hits.clear();
    const fs::file file(list_path);
    if (!file)
    {
        s.error = store_tr("Falta /data/rpcs3/pkgi_games.txt (copie a lista por FTP e aperte X)", "Missing /data/rpcs3/pkgi_games.txt (copy the list by FTP, then press X)");
        return;
    }
    if (file.size() > (96ull << 20))
    {
        s.error = store_tr("pkgi_games.txt grande demais (limite 96 MiB)", "pkgi_games.txt is too big (limit 96 MiB)");
        return;
    }
    s.catalog = ps::catalog(ps::parse_list(file.to_string()));
    if (s.catalog.items().empty())
    {
        s.error = store_tr("A lista está vazia ou não tem o cabeçalho do pkgi (X tenta de novo)", "The list is empty or lacks the pkgi header (X retries)");
        return;
    }
    s.loaded = true;
    apply(s);
}

inline void ensure_manager(store_state& s)
{
    if (s.manager) return;
    ps::file_ops files = ps::posix_file_ops();
    files.free_bytes = [](const std::string&) -> std::int64_t
    {
        fs::device_stat stat{};
        return fs::statfs("/data/", stat) ? static_cast<std::int64_t>(stat.avail_free) : -1;
    };
    store_state* const self = &s;
    s.manager = std::make_unique<ps::manager>(ps::config{}, ps::http_fetch, std::move(files), nullptr,
        [self](const ps::job_view&) { self->landed = true; }); // worker thread: only a flag
}

// Header row, the running and finished downloads (newest first, at most 4), then up to max_rows hits.
inline std::vector<xmb_entry> rows(store_state& s)
{
    std::vector<xmb_entry> out;
    if (!s.loaded)
    {
        out.push_back({"@store:error", s.error.empty() ? store_tr("Lista não carregada", "List not loaded") : s.error, "", "tex_psn", {}});
        return out;
    }
    const std::string query = s.filter.text.empty() ? std::string("-") : "\"" + shorten(s.filter.text, 24) + "\"";
    out.push_back({"@store:info", "PKG Store: " + std::to_string(s.hits.size()) + store_tr(" resultados", " results"),
        store_tr("Busca ", "Search ") + query + " | " + store_tr("Região ", "Region ") + region_label(s.region) + " | " +
            store_tr("Tipo ", "Type ") + type_label(s.type) + " | " + store_tr("Ordem ", "Order ") + order_label(s.filter.order),
        "tex_psn", {}});
    if (s.manager)
    {
        std::size_t shown = 0;
        for (const ps::job_view& job : s.manager->snapshot())
        {
            if (shown++ == 4) break;
            out.push_back({"@store:job:" + std::to_string(job.id), job.name, job_label(job), "tex_psn", {}});
        }
    }
    const auto& items = s.catalog.items();
    const std::size_t count = std::min(s.hits.size(), max_rows);
    for (std::size_t i = 0; i < count; ++i)
    {
        const ps::item& it = items[s.hits[i]];
        std::string line = it.tid + " | " + it.region + " | " + size_label(it.size);
        if (it.type != ps::kind::game) line += " | " + kind_label(it.type);
        if (!it.downloadable()) line += store_tr(" | sem link", " | no link");
        out.push_back({"@store:item:" + std::to_string(s.hits[i]), it.name, line, "tex_psn", {}});
    }
    if (s.hits.size() > count)
        out.push_back({"@store:info", store_tr("Mostrando ", "Showing ") + std::to_string(count) + store_tr(" de ", " of ") + std::to_string(s.hits.size()),
            store_tr("Refine a busca (Quadrado) para ver o resto", "Narrow the search (Square) to see the rest"), "tex_psn", {}});
    return out;
}

// Writes the finished downloads to /data/rpcs3/startup.log once each (UI thread), with the technical detail.
inline void log_finished(store_state& s)
{
    if (!s.manager) return;
    for (const ps::job_view& v : s.manager->snapshot())
    {
        if (v.active() || std::find(s.logged.begin(), s.logged.end(), v.id) != s.logged.end()) continue;
        s.logged.push_back(v.id);
        const char* what = v.st == ps::state::done ? "done" : v.st == ps::state::cancelled ? "cancelled" : "FAILED";
        ps4_startup_checkpoint(("pkgstore: " + std::string(what) + ": " + v.tid + " " + v.name + " (" + std::to_string(v.done) + " bytes) " + v.detail).c_str());
    }
}
} // namespace pkgui

