// PupBerg installer
// lists the Steam games installed on this PC (or takes any game folder), replaces their steam_api
// libraries with PupBerg (keeping the originals as *.valve_original), writes steam_settings with the
// game's appid + steam_interfaces.txt, and asks for the Steam account to use

#include "../account_picker/account_picker_core.hpp"

#include <cstdint>
#include <regex>
#include <set>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static const char BACKUP_SUFFIX[] = ".valve_original";

struct SteamGame {
    uint32_t appid = 0;
    std::string name{};
    std::string installdir{};
    fs::path dir{};
};

struct Payload {
    fs::path win64{}, win32{}, linux64{}, linux32{};
};

struct SteamApiLib {
    fs::path path{};
    enum class Kind { Win64, Win32, Linux64, Linux32 } kind{};
};

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string read_line()
{
    std::string input{};
    if (!std::getline(std::cin, input)) return "q";
    while (input.size() && (input.back() == '\r' || input.back() == ' ')) input.pop_back();
    return input;
}

static fs::path self_dir()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n) return fs::path(buf).parent_path();
#else
    std::error_code ec{};
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe.parent_path();
#endif
    return fs::current_path();
}

// paths read from Steam's files; under Wine a Linux path has to go through the Z: drive
static fs::path host_path(const std::string &p)
{
#if defined(_WIN32)
    if (p.size() && p[0] == '/') {
        std::string w = "Z:" + p;
        std::replace(w.begin(), w.end(), '/', '\\');
        return fs::u8path(w);
    }
#endif
    return fs::u8path(p);
}

static std::vector<std::string> quoted_tokens(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> tokens{};
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string tok{};
        size_t j = i + 1;
        for (; j < text.size() && text[j] != '"'; ++j) {
            if (text[j] == '\\' && j + 1 < text.size()) ++j;
            tok.push_back(text[j]);
        }
        tokens.push_back(tok);
        i = j;
    }
    return tokens;
}

static std::vector<fs::path> steam_libraries()
{
    std::vector<fs::path> libs{};
    std::set<std::string> seen{};
    auto add = [&](const fs::path &p) {
        std::error_code ec{};
        if (!fs::is_directory(p / "steamapps", ec)) return;
        fs::path c = fs::weakly_canonical(p, ec);
        std::string key = lower((ec ? p : c).u8string());
        if (seen.insert(key).second) libs.push_back(p);
    };

    for (const auto &root : steam_install_candidates()) {
        add(root);
        auto tokens = quoted_tokens(root / "steamapps" / "libraryfolders.vdf");
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (lower(tokens[i]) == "path") add(host_path(tokens[i + 1]));
        }
    }
    return libs;
}

static std::vector<SteamGame> installed_games()
{
    std::vector<SteamGame> games{};
    std::set<uint32_t> seen{};
    for (const auto &lib : steam_libraries()) {
        std::error_code ec{};
        for (const auto &e : fs::directory_iterator(lib / "steamapps", ec)) {
            std::string fname = e.path().filename().u8string();
            if (fname.rfind("appmanifest_", 0) != 0 || e.path().extension() != ".acf") continue;
            auto tokens = quoted_tokens(e.path());
            SteamGame g{};
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                std::string key = lower(tokens[i]);
                if (key == "appid" && !g.appid) {
                    try { g.appid = (uint32_t)std::stoul(tokens[i + 1]); } catch (...) {}
                } else if (key == "name" && g.name.empty()) {
                    g.name = tokens[i + 1];
                } else if (key == "installdir" && g.installdir.empty()) {
                    g.installdir = tokens[i + 1];
                }
            }
            if (!g.appid || g.installdir.empty()) continue;
            // compatibility tools and runtimes aren't games
            std::string n = lower(g.name);
            if (n.find("proton") != std::string::npos || n.find("steam linux runtime") != std::string::npos ||
                n.find("steamworks common redistributables") != std::string::npos) continue;
            g.dir = lib / "steamapps" / "common" / fs::u8path(g.installdir);
            if (!fs::is_directory(g.dir, ec)) continue;
            if (seen.insert(g.appid).second) games.push_back(g);
        }
    }
    std::sort(games.begin(), games.end(), [](const SteamGame &a, const SteamGame &b) { return lower(a.name) < lower(b.name); });
    return games;
}

static Payload find_payload()
{
    Payload p{};
    std::error_code ec{};
    fs::path base = self_dir();
    for (const fs::path &dir : { base, base.parent_path() }) {
        if (p.win64.empty() && fs::exists(dir / "steam_api64.dll", ec)) p.win64 = dir / "steam_api64.dll";
        if (p.win32.empty() && fs::exists(dir / "steam_api.dll", ec)) p.win32 = dir / "steam_api.dll";
        if (p.linux64.empty() && fs::exists(dir / "x64" / "libsteam_api.so", ec)) p.linux64 = dir / "x64" / "libsteam_api.so";
        if (p.linux32.empty() && fs::exists(dir / "x86" / "libsteam_api.so", ec)) p.linux32 = dir / "x86" / "libsteam_api.so";
    }
    return p;
}

static std::vector<SteamApiLib> find_steam_api_libs(const fs::path &game_dir)
{
    std::vector<SteamApiLib> libs{};
    std::error_code ec{};
    auto it = fs::recursive_directory_iterator(game_dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 8) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string name = lower(it->path().filename().u8string());
        SteamApiLib lib{};
        lib.path = it->path();
        if (name == "steam_api64.dll") {
            lib.kind = SteamApiLib::Kind::Win64;
        } else if (name == "steam_api.dll") {
            lib.kind = SteamApiLib::Kind::Win32;
        } else if (name == "libsteam_api.so") {
            std::ifstream f(it->path(), std::ios::binary);
            char head[5] = {};
            f.read(head, sizeof(head));
            lib.kind = head[4] == 1 ? SteamApiLib::Kind::Linux32 : SteamApiLib::Kind::Linux64;
        } else {
            continue;
        }
        libs.push_back(lib);
    }
    return libs;
}

static std::string read_file(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// PupBerg and other gbe builds read configs.user.ini, Valve's library never mentions it
static bool is_emulator(const fs::path &p)
{
    return read_file(p).find("configs.user.ini") != std::string::npos;
}

// same patterns as tools/generate_interfaces
static std::vector<std::string> steam_interfaces(const std::string &contents)
{
    static const char *patterns[] = {
        R"(STEAMAPPS_INTERFACE_VERSION\d+)", R"(SteamApps\d+)", R"(STEAMAPPLIST_INTERFACE_VERSION\d+)",
        R"(STEAMAPPTICKET_INTERFACE_VERSION\d+)", R"(SteamClient\d+)", R"(STEAMCONTROLLER_INTERFACE_VERSION)",
        R"(SteamController\d+)", R"(SteamFriends\d+)", R"(SteamGameServerStats\d+)", R"(SteamGameCoordinator\d+)",
        R"(SteamGameServer\d+)", R"(STEAMHTMLSURFACE_INTERFACE_VERSION_\d+)", R"(STEAMHTTP_INTERFACE_VERSION\d+)",
        R"(SteamInput\d+)", R"(STEAMINVENTORY_INTERFACE_V\d+)", R"(SteamMatchMakingServers\d+)", R"(SteamMatchMaking\d+)",
        R"(SteamMatchGameSearch\d+)", R"(SteamParties\d+)", R"(STEAMMUSIC_INTERFACE_VERSION\d+)",
        R"(STEAMMUSICREMOTE_INTERFACE_VERSION\d+)", R"(SteamNetworkingMessages\d+)", R"(SteamNetworkingSockets\d+)",
        R"(SteamNetworkingUtils\d+)", R"(SteamNetworking\d+)", R"(STEAMPARENTALSETTINGS_INTERFACE_VERSION\d+)",
        R"(STEAMREMOTEPLAY_INTERFACE_VERSION\d+)", R"(STEAMREMOTESTORAGE_INTERFACE_VERSION\d+)",
        R"(STEAMSCREENSHOTS_INTERFACE_VERSION\d+)", R"(STEAMTIMELINE_INTERFACE_V\d+)", R"(STEAMUGC_INTERFACE_VERSION\d+)",
        R"(SteamUser\d+)", R"(STEAMUSERSTATS_INTERFACE_VERSION\d+)", R"(SteamUtils\d+)", R"(STEAMVIDEO_INTERFACE_V\d+)",
        R"(STEAMUNIFIEDMESSAGES_INTERFACE_VERSION\d+)", R"(SteamMasterServerUpdater\d+)",
    };
    std::vector<std::string> out{};
    for (const char *patt : patterns) {
        std::regex re(patt);
        std::vector<std::string> matches{};
        for (auto i = std::sregex_iterator(contents.begin(), contents.end(), re); i != std::sregex_iterator(); ++i) {
            if (std::find(matches.begin(), matches.end(), i->str()) == matches.end()) matches.push_back(i->str());
        }
        // SteamClient017 is the one games ask for when several are listed
        if (std::string(patt) == R"(SteamClient\d+)" && std::find(matches.begin(), matches.end(), "SteamClient017") != matches.end()) {
            matches = { "SteamClient017" };
        }
        out.insert(out.end(), matches.begin(), matches.end());
    }
    return out;
}

static bool copy_over(const fs::path &from, const fs::path &to)
{
    std::error_code ec{};
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) std::cout << "    [X] " << ec.message() << "\n";
    return !ec;
}

static void install_game(const fs::path &game_dir, uint32_t appid, const Payload &payload)
{
    auto libs = find_steam_api_libs(game_dir);
    if (libs.empty()) {
        std::cout << "[X] no steam_api64.dll / steam_api.dll / libsteam_api.so found in " << game_dir.u8string() << "\n";
        return;
    }

    for (const auto &lib : libs) {
        const fs::path *src = nullptr;
        switch (lib.kind) {
        case SteamApiLib::Kind::Win64: src = &payload.win64; break;
        case SteamApiLib::Kind::Win32: src = &payload.win32; break;
        case SteamApiLib::Kind::Linux64: src = &payload.linux64; break;
        case SteamApiLib::Kind::Linux32: src = &payload.linux32; break;
        }
        std::cout << "\n  " << lib.path.u8string() << "\n";
        if (!src || src->empty()) {
            std::cout << "    [X] this package has no PupBerg build for " << lib.path.filename().u8string() << ", skipped\n";
            continue;
        }

        std::error_code ec{};
        fs::path backup = lib.path;
        backup += BACKUP_SUFFIX;
        fs::path settings = lib.path.parent_path() / "steam_settings";
        fs::create_directories(settings, ec);

        bool have_original = fs::exists(backup, ec) && !is_emulator(backup);
        if (!fs::exists(backup, ec)) {
            if (is_emulator(lib.path)) {
                // installed by hand or by another emu, there's no Valve library left to keep
                std::cout << "    already an emulator build, replacing it (no Valve original to keep)\n";
            } else {
                // first install: keep Valve's library and remember which interfaces it has
                fs::copy_file(lib.path, backup, ec);
                if (ec) {
                    std::cout << "    [X] couldn't back up the original: " << ec.message() << "\n";
                    continue;
                }
                have_original = true;
                std::cout << "    original saved as " << backup.filename().u8string() << "\n";
            }
        }

        if (have_original && !fs::exists(settings / "steam_interfaces.txt", ec)) {
            auto ifaces = steam_interfaces(read_file(backup));
            if (ifaces.size()) {
                std::ofstream f(settings / "steam_interfaces.txt", std::ios::binary | std::ios::trunc);
                for (const auto &i : ifaces) f << i << "\n";
                std::cout << "    steam_interfaces.txt: " << ifaces.size() << " interfaces\n";
            }
        }

        if (!copy_over(*src, lib.path)) continue;
        std::cout << "    PupBerg installed\n";

        if (appid) {
            std::ofstream f(settings / "steam_appid.txt", std::ios::binary | std::ios::trunc);
            f << appid << "\n";
        }
        if (!fs::exists(settings / "configs.overlay.ini", ec)) {
            std::ofstream f(settings / "configs.overlay.ini", std::ios::binary | std::ios::trunc);
            f << "[overlay::general]\nenable_experimental_overlay=1\n";
        }
        std::cout << "    steam_settings ready" << (appid ? " (appid " + std::to_string(appid) + ")" : "") << "\n";
    }
}

static void uninstall_game(const fs::path &game_dir)
{
    auto libs = find_steam_api_libs(game_dir);
    int restored = 0;
    for (const auto &lib : libs) {
        std::error_code ec{};
        fs::path backup = lib.path;
        backup += BACKUP_SUFFIX;
        if (!fs::exists(backup, ec)) continue;
        if (copy_over(backup, lib.path)) {
            fs::remove(backup, ec);
            std::cout << "  restored " << lib.path.u8string() << "\n";
            ++restored;
        }
    }
    if (!restored) std::cout << "  nothing to restore: no original library was saved here (verify the game files in Steam to get it back)\n";
    else std::cout << "  steam_settings was kept, it does nothing without PupBerg\n";
}

static bool is_installed(const fs::path &game_dir)
{
    for (const auto &lib : find_steam_api_libs(game_dir)) {
        std::error_code ec{};
        fs::path backup = lib.path;
        backup += BACKUP_SUFFIX;
        if (fs::exists(backup, ec) || is_emulator(lib.path)) return true;
    }
    return false;
}

static uint32_t ask_appid(const fs::path &dir, const std::vector<SteamGame> &games)
{
    // a steam_appid.txt the game or an older install left behind
    std::error_code ec{};
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
        if (lower(it->path().filename().u8string()) != "steam_appid.txt") continue;
        try {
            uint32_t id = (uint32_t)std::stoul(read_file(it->path()));
            if (id) {
                std::cout << "Found appid " << id << " in " << it->path().u8string() << "\n";
                return id;
            }
        } catch (...) {}
    }

    // a game that was moved out of its Steam library keeps its folder name
    std::string folder = lower(dir.filename().u8string());
    for (const auto &g : games) {
        if (lower(g.installdir) == folder || lower(g.name) == folder) {
            std::cout << "Looks like '" << g.name << "' (appid " << g.appid << ")\n";
            return g.appid;
        }
    }

    while (true) {
        std::cout << "Enter the game's Steam App ID (the number in its store page URL, store.steampowered.com/app/<ID>),\n"
                     "or leave empty to skip: ";
        std::string in = read_line();
        if (in.empty()) return 0;
        try {
            uint32_t id = (uint32_t)std::stoul(in);
            if (id) return id;
        } catch (...) {}
        std::cout << "[X] that's not a number\n";
    }
}

int main()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "PupBerg installer\n=================\n\n";

    Payload payload = find_payload();
    if (payload.win64.empty() && payload.win32.empty() && payload.linux64.empty() && payload.linux32.empty()) {
        std::cout << "[X] PupBerg's steam_api files weren't found next to the installer.\n"
                     "    Keep the installer in the folder you extracted the PupBerg package to.\n";
        wait_for_enter();
        return 1;
    }

    auto games = installed_games();
    while (true) {
        std::cout << "\nYour Steam games:\n";
        for (size_t i = 0; i < games.size(); ++i) {
            std::cout << "  [" << (i + 1) << "] " << games[i].name << "  (appid " << games[i].appid << ")"
                      << (is_installed(games[i].dir) ? "  <- PupBerg installed" : "") << "\n";
        }
        if (games.empty()) std::cout << "  (no Steam library found)\n";
        std::cout << "  [f] a game folder that isn't in a Steam library\n"
                     "  [a] change the Steam account PupBerg uses\n"
                     "  [q] quit\n\nChoose: ";

        std::string in = lower(read_line());
        if (in == "q" || in.empty()) break;
        if (in == "a") {
            std::cout << "\n";
            pick_account_interactive();
            continue;
        }

        fs::path dir{};
        uint32_t appid = 0;
        if (in == "f") {
            std::cout << "Game folder path: ";
            std::string p = read_line();
            if (p.size() >= 2 && p.front() == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
            dir = host_path(p);
            std::error_code ec{};
            if (!fs::is_directory(dir, ec)) {
                std::cout << "[X] not a folder: " << p << "\n";
                continue;
            }
            appid = ask_appid(dir, games);
        } else {
            size_t n = 0;
            try { n = std::stoul(in); } catch (...) {}
            if (n < 1 || n > games.size()) {
                std::cout << "[X] invalid choice\n";
                continue;
            }
            dir = games[n - 1].dir;
            appid = games[n - 1].appid;
            std::cout << "\n" << games[n - 1].name << "\n";
        }

        if (is_installed(dir)) {
            std::cout << "PupBerg is already installed here. [u] update  [r] remove, restore the original  [c] cancel: ";
            std::string a = lower(read_line());
            if (a == "r") {
                uninstall_game(dir);
                continue;
            }
            if (a != "u") continue;
        }

        install_game(dir, appid, payload);

        SteamAccount acc = current_account();
        if (acc.steamid.empty() || acc.persona_name.empty() || acc.persona_name == "gse orca") {
            std::cout << "\nPupBerg doesn't know your Steam account yet, pick it so friends see your real name:\n\n";
            pick_account_interactive();
        } else {
            std::cout << "\nPlaying as '" << acc.persona_name << "' ([a] in the menu to change)\n";
        }
        std::cout << "\nDone! Start the game, Shift+Tab opens the PupBerg overlay.\n";
    }

    return 0;
}
