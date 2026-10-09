// PupBerg account picker
// finds the Steam accounts that logged in on this PC (config/loginusers.vdf)
// and writes the chosen one into the emu's global configs.user.ini

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

struct SteamAccount {
    std::string steamid{};
    std::string account_name{};
    std::string persona_name{};
    bool most_recent = false;
};

static std::vector<fs::path> steam_install_candidates()
{
    std::vector<fs::path> dirs{};
#if defined(_WIN32)
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        WCHAR buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
            dirs.emplace_back(buf);
        }
        RegCloseKey(key);
    }
    dirs.emplace_back(L"C:\\Program Files (x86)\\Steam");
    dirs.emplace_back(L"C:\\Program Files\\Steam");
#else
    const char *home = std::getenv("HOME");
    if (home) {
        dirs.emplace_back(fs::path(home) / ".steam/steam");
        dirs.emplace_back(fs::path(home) / ".local/share/Steam");
        dirs.emplace_back(fs::path(home) / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
    }
#endif
    return dirs;
}

// read the next quoted token, returns false when there are no more quotes
static bool next_token(const std::string &line, size_t &pos, std::string &out)
{
    size_t start = line.find('"', pos);
    if (start == std::string::npos) return false;
    size_t end = start + 1;
    out.clear();
    while (end < line.size() && line[end] != '"') {
        if (line[end] == '\\' && end + 1 < line.size()) ++end;
        out.push_back(line[end]);
        ++end;
    }
    pos = end + 1;
    return true;
}

static std::vector<SteamAccount> parse_loginusers(const fs::path &file)
{
    std::vector<SteamAccount> accounts{};
    std::ifstream in(file);
    if (!in.is_open()) return accounts;

    SteamAccount *current = nullptr;
    std::string line{};
    while (std::getline(in, line)) {
        size_t pos = 0;
        std::string key{}, value{};
        if (!next_token(line, pos, key)) continue;
        bool has_value = next_token(line, pos, value);

        if (!has_value) {
            // a section header, user sections are named by their steamid64
            if (key.size() == 17 && std::all_of(key.begin(), key.end(), ::isdigit)) {
                accounts.push_back({ key });
                current = &accounts.back();
            }
            continue;
        }

        if (!current) continue;
        std::string lower_key(key);
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if (lower_key == "accountname") current->account_name = value;
        else if (lower_key == "personaname") current->persona_name = value;
        else if (lower_key == "mostrecent") current->most_recent = (value == "1");
    }
    return accounts;
}

static fs::path global_settings_dir()
{
#if defined(_WIN32)
    WCHAR buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, buf))) {
        return fs::path(buf) / L"GSE Saves" / L"settings";
    }
    return {};
#else
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) return fs::path(xdg) / "GSE Saves/settings";
    const char *home = std::getenv("HOME");
    if (home) return fs::path(home) / ".local/share/GSE Saves/settings";
    return {};
#endif
}

// update account_name/account_steamid in place, keep every other line as is
static bool write_user_ini(const fs::path &file, const SteamAccount &acc)
{
    std::vector<std::string> lines{};
    {
        std::ifstream in(file);
        std::string line{};
        while (std::getline(in, line)) {
            if (line.size() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    const std::string name_line = "account_name=" + acc.persona_name;
    const std::string id_line = "account_steamid=" + acc.steamid;
    bool has_section = false, has_name = false, has_id = false;
    bool in_user_section = false;
    size_t section_end = lines.size();

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        if (l.size() && l[0] == '[') {
            if (in_user_section) section_end = i;
            in_user_section = (l.rfind("[user::general]", 0) == 0);
            if (in_user_section) {
                has_section = true;
                section_end = lines.size();
            }
            continue;
        }
        if (!in_user_section) continue;
        if (l.rfind("account_name=", 0) == 0) { lines[i] = name_line; has_name = true; }
        else if (l.rfind("account_steamid=", 0) == 0) { lines[i] = id_line; has_id = true; }
    }

    if (!has_section) {
        if (lines.size() && lines.back().size()) lines.push_back("");
        lines.push_back("[user::general]");
        section_end = lines.size();
    }
    if (!has_id) lines.insert(lines.begin() + section_end, id_line);
    if (!has_name) lines.insert(lines.begin() + section_end, name_line);

    std::error_code ec{};
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto &l : lines) out << l << "\n";
    return out.good();
}

static void wait_for_enter()
{
#if defined(_WIN32)
    std::cout << "\nPress Enter to close...";
    std::string dummy{};
    std::getline(std::cin, dummy);
#endif
}

int main()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "PupBerg account picker\n======================\n\n";

    std::vector<SteamAccount> accounts{};
    for (const auto &dir : steam_install_candidates()) {
        fs::path vdf = dir / "config" / "loginusers.vdf";
        std::error_code ec{};
        if (!fs::exists(vdf, ec)) continue;
        accounts = parse_loginusers(vdf);
        if (accounts.size()) {
            std::cout << "Found Steam at: " << dir.u8string() << "\n\n";
            break;
        }
    }

    if (accounts.empty()) {
        std::cout << "[X] couldn't find any Steam accounts on this PC (is Steam installed and have you logged in?)\n";
        wait_for_enter();
        return 1;
    }

    for (size_t i = 0; i < accounts.size(); ++i) {
        const auto &a = accounts[i];
        std::cout << "  [" << (i + 1) << "] " << a.persona_name
                  << "  (" << a.account_name << ", " << a.steamid << ")"
                  << (a.most_recent ? "  <- last used" : "") << "\n";
    }

    size_t choice = 0;
    while (true) {
        std::cout << "\nPick your account [1-" << accounts.size() << "]: ";
        std::string input{};
        if (!std::getline(std::cin, input)) return 1;
        try {
            choice = std::stoul(input);
        } catch (...) {
            choice = 0;
        }
        if (choice >= 1 && choice <= accounts.size()) break;
        std::cout << "[X] invalid choice\n";
    }

    const SteamAccount &acc = accounts[choice - 1];
    fs::path dir = global_settings_dir();
    if (dir.empty()) {
        std::cout << "[X] couldn't find the user data folder\n";
        wait_for_enter();
        return 1;
    }

    fs::path ini = dir / "configs.user.ini";
    if (!write_user_ini(ini, acc)) {
        std::cout << "[X] failed to write " << ini.u8string() << "\n";
        wait_for_enter();
        return 1;
    }

    std::cout << "\nDone! Games will now use '" << acc.persona_name << "' (" << acc.steamid << ")\n"
              << "Saved to: " << ini.u8string() << "\n";
    wait_for_enter();
    return 0;
}
