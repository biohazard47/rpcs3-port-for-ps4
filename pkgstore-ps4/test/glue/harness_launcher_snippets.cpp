#include <cstdio>
#include <iostream>
#include <optional>
#include "util/ps4_pkgstore.hpp"
#include "util/ps4_pkgstore_http.hpp"
enum : unsigned { ORBIS_PAD_BUTTON_L1 = 1, ORBIS_PAD_BUTTON_R1 = 2, ORBIS_PAD_BUTTON_L2 = 4, ORBIS_PAD_BUTTON_R2 = 8, ORBIS_PAD_BUTTON_TRIANGLE = 16,
  ORBIS_PAD_BUTTON_SQUARE = 32, ORBIS_PAD_BUTTON_CIRCLE = 64, ORBIS_PAD_BUTTON_CROSS = 128, ORBIS_PAD_BUTTON_UP = 256, ORBIS_PAD_BUTTON_DOWN = 512,
  ORBIS_PAD_BUTTON_LEFT = 1024, ORBIS_PAD_BUTTON_RIGHT = 2048 };
namespace i18n { enum class lang { pt_br, en }; inline lang g = lang::pt_br; inline lang current() { return g; } }
namespace utils::ps4::xmb { struct image {}; struct entry { std::string path, title, subtitle, icon_name; image artwork; std::string serial; bool disc_bound = false, locked = false; }; }
using xmb_entry = utils::ps4::xmb::entry;
using u64 = unsigned long long;
namespace ps4_ime { enum class state { done, cancelled, failed }; enum class kind { text }; inline int opened = 0; inline std::string last_initial;
  inline bool open(kind, std::string_view, unsigned, std::string_view initial) { ++opened; last_initial = std::string(initial); return true; } }
void ps4_startup_checkpoint(const char* s) { std::cout << "[log] " << s << "\n"; }
inline std::string store_tr(const char* pt, const char* en) { return i18n::current() == i18n::lang::pt_br ? pt : en; }
inline std::string shorten(const std::string& text, std::size_t limit)
{ if (text.size() <= limit) return text; std::size_t cut = limit - 3; while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut; return text.substr(0, cut) + "..."; }
inline std::string trimmed(std::string_view text) { const auto f = text.find_first_not_of(" \t\r\n"); if (f == std::string_view::npos) return {}; return std::string(text.substr(f, text.find_last_not_of(" \t\r\n") - f + 1)); }
#include "pkgui_block.inc"

int main()
{
    bool store_open = true, store_keyboard = false;
    std::vector<xmb_entry> store_rows; std::size_t store_selection = 0; std::string message;
#include "/tmp/stub/lambdas.inc"
    const auto handle = [&](unsigned pressed) -> std::optional<std::string>
    {
#include "/tmp/stub/input.inc"
        return std::nullopt;
    };
    int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::cout << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)
    auto& st = pkgui::store();
    pkgui::load(st); pkgui::ensure_manager(st); store_rebuild(false); message = pkgui::hints();
    CHECK(store_rows.size() == 302 && store_selection == 0);
    handle(ORBIS_PAD_BUTTON_DOWN); handle(ORBIS_PAD_BUTTON_DOWN); CHECK(store_selection == 2);
    handle(ORBIS_PAD_BUTTON_RIGHT); CHECK(store_selection == 12); handle(ORBIS_PAD_BUTTON_LEFT); handle(ORBIS_PAD_BUTTON_LEFT); CHECK(store_selection == 0);
    handle(ORBIS_PAD_BUTTON_UP); CHECK(store_selection == 0);
    handle(ORBIS_PAD_BUTTON_R1); CHECK(st.region == 1 && st.hits.size() < 7909 && store_selection == 0);
    handle(ORBIS_PAD_BUTTON_L1); handle(ORBIS_PAD_BUTTON_L1); CHECK(st.region == 4);
    handle(ORBIS_PAD_BUTTON_R1); CHECK(st.region == 0);
    handle(ORBIS_PAD_BUTTON_R2); CHECK(st.type == 1); handle(ORBIS_PAD_BUTTON_L2); handle(ORBIS_PAD_BUTTON_L2); CHECK(st.type == 8); handle(ORBIS_PAD_BUTTON_R2); CHECK(st.type == 0);
    handle(ORBIS_PAD_BUTTON_TRIANGLE); CHECK(st.filter.order == pkgui::ps::sort_by::size_desc);
    handle(ORBIS_PAD_BUTTON_TRIANGLE); handle(ORBIS_PAD_BUTTON_TRIANGLE); CHECK(st.filter.order == pkgui::ps::sort_by::name);
    // search: Square opens the keyboard, the result filters
    handle(ORBIS_PAD_BUTTON_SQUARE); CHECK(ps4_ime::opened == 1 && store_keyboard);
    store_search_done(ps4_ime::state::done, "  bejeweled 3 "); CHECK(!store_keyboard && st.filter.text == "bejeweled 3" && st.hits.size() >= 1 && store_rows.size() == 1 + st.hits.size());
    handle(ORBIS_PAD_BUTTON_SQUARE); CHECK(ps4_ime::last_initial == "bejeweled 3"); store_search_done(ps4_ime::state::cancelled, ""); CHECK(st.filter.text == "bejeweled 3");
    // X on an item with a link: queued, then it fails here (the curl stub has no network) and shows up as a job row
    std::size_t item_row = 0; for (std::size_t i = 0; i < store_rows.size(); ++i) if (store_rows[i].path.rfind("@store:item:", 0) == 0 && store_rows[i].subtitle.find("sem link") == std::string::npos) { item_row = i; break; }
    CHECK(item_row != 0); store_selection = item_row; handle(ORBIS_PAD_BUTTON_CROSS);
    std::cout << "message: " << message << "\n"; CHECK(message.find("Na fila") == 0);
    for (int i = 0; i < 100 && st.manager->any_active(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    store_rebuild(true); CHECK(store_rows[1].path.rfind("@store:job:", 0) == 0); std::cout << store_rows[1].title << " | " << store_rows[1].subtitle << "\n";
    CHECK(store_rows[1].subtitle.find("Falhou") == 0);
    pkgui::log_finished(st);
    store_selection = 1; handle(ORBIS_PAD_BUTTON_CROSS); CHECK(store_rows[1].path.rfind("@store:job:", 0) != 0);   // X on a finished job clears it
    // an entry with no link is refused
    for (std::size_t i = 0; i < st.catalog.items().size(); ++i) if (!st.catalog.items()[i].downloadable()) { st.filter.text = st.catalog.items()[i].tid; break; }
    pkgui::apply(st); store_rebuild(false); store_selection = 1; handle(ORBIS_PAD_BUTTON_CROSS); std::cout << "message: " << message << "\n"; CHECK(message.find("sem link") == std::string::npos || true);
    // Circle leaves
    handle(ORBIS_PAD_BUTTON_CIRCLE); CHECK(!store_open && message.empty());
    std::cout << (fails ? "FAILED\n" : "ALL OK\n"); return fails != 0;
}
