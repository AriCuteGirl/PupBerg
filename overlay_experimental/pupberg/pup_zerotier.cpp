#ifdef EMU_OVERLAY

#include "pupberg/pup_zerotier.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>

#include <curl/curl.h>
#include "json/json.hpp"

namespace pupberg {

static constexpr auto FAST_POLL = std::chrono::seconds(3);
static constexpr auto SLOW_POLL = std::chrono::seconds(30);

ZeroTierClient::ZeroTierClient(std::string api_url, std::string token, std::string token_path) :
    api_url(std::move(api_url)),
    token(std::move(token)),
    token_path_override(std::move(token_path))
{
    while (!this->api_url.empty() && this->api_url.back() == '/') this->api_url.pop_back();
    worker = std::thread(&ZeroTierClient::worker_proc, this);
    refresh();
}

ZeroTierClient::~ZeroTierClient()
{
    {
        std::lock_guard lock(jobs_mutex);
        stop = true;
    }
    jobs_cv.notify_all();
    if (worker.joinable()) worker.join();
}

void ZeroTierClient::set_address_callback(AddressCallback cb)
{
    std::lock_guard lock(state_mutex);
    address_cb = std::move(cb);
}

ZeroTierClient::State ZeroTierClient::snapshot()
{
    std::lock_guard lock(state_mutex);
    return state;
}

void ZeroTierClient::refresh()
{
    push_job([this] { do_refresh(); });
}

void ZeroTierClient::join(const std::string &network_id)
{
    if (!valid_network_id(network_id)) {
        set_error("Network ID must be 16 hex characters");
        return;
    }
    push_job([this, network_id] {
        std::string resp{};
        long code = request("POST", "/network/" + network_id, resp);
        if (code != 200) set_error("Join failed (HTTP " + std::to_string(code) + ")");
        do_refresh();
    });
}

void ZeroTierClient::leave(const std::string &network_id)
{
    if (!valid_network_id(network_id)) return;
    push_job([this, network_id] {
        std::string resp{};
        long code = request("DELETE", "/network/" + network_id, resp);
        if (code != 200) set_error("Leave failed (HTTP " + std::to_string(code) + ")");
        do_refresh();
    });
}

void ZeroTierClient::set_fast_polling(bool fast)
{
    bool was = fast_polling.exchange(fast);
    if (fast && !was) refresh();
}

void ZeroTierClient::push_job(std::function<void()> job)
{
    {
        std::lock_guard lock(jobs_mutex);
        jobs.push_back(std::move(job));
    }
    {
        std::lock_guard lock(state_mutex);
        state.busy = true;
    }
    jobs_cv.notify_one();
}

void ZeroTierClient::worker_proc()
{
    while (true) {
        std::function<void()> job{};
        {
            std::unique_lock lock(jobs_mutex);
            auto wait = fast_polling ? FAST_POLL : SLOW_POLL;
            jobs_cv.wait_for(lock, wait, [this] { return stop || !jobs.empty(); });
            if (stop) return;
            if (!jobs.empty()) {
                job = std::move(jobs.front());
                jobs.pop_front();
            }
        }

        if (job) job();
        else do_refresh(); // poll timeout

        std::lock_guard lock(jobs_mutex);
        if (jobs.empty()) {
            std::lock_guard slock(state_mutex);
            state.busy = false;
        }
    }
}

static std::string read_first_line(const std::string &path)
{
    if (path.empty()) return {};
    std::error_code ec{};
    auto p = std::filesystem::u8path(path);
    if (!std::filesystem::is_regular_file(p, ec)) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    std::string line{};
    std::getline(f, line);
    line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
    return line;
}

static std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool ZeroTierClient::load_token()
{
    if (!token.empty()) return true;

    std::vector<std::string> candidates{};
    if (!token_path_override.empty()) candidates.push_back(token_path_override);

    // the file zerotier-cli uses for non-root users
    std::string home = env("HOME");
    if (!home.empty()) candidates.push_back(home + "/.zeroTierOneAuthToken");
#if defined(_WIN32)
    // running under Wine/Proton: the unix home is exposed as \??\Z:\home\user
    std::string wine_home = env("WINEHOMEDIR");
    if (wine_home.rfind("\\??\\", 0) == 0) wine_home = wine_home.substr(4);
    if (!wine_home.empty()) candidates.push_back(wine_home + "\\.zeroTierOneAuthToken");
    if (!home.empty() && home[0] == '/') {
        std::string z = "Z:" + home;
        std::replace(z.begin(), z.end(), '/', '\\');
        candidates.push_back(z + "\\.zeroTierOneAuthToken");
    }
    std::string local_appdata = env("LOCALAPPDATA");
    if (!local_appdata.empty()) candidates.push_back(local_appdata + "\\ZeroTier\\authtoken.secret");
    std::string program_data = env("ProgramData");
    if (program_data.empty()) program_data = "C:\\ProgramData";
    candidates.push_back(program_data + "\\ZeroTier\\One\\authtoken.secret");
    candidates.push_back("Z:\\var\\lib\\zerotier-one\\authtoken.secret");
#else
    candidates.push_back("/var/lib/zerotier-one/authtoken.secret");
    candidates.push_back("/Library/Application Support/ZeroTier/One/authtoken.secret");
#endif

    for (const auto &c : candidates) {
        std::string t = read_first_line(c);
        if (!t.empty()) {
            token = t;
            return true;
        }
    }
    return false;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

long ZeroTierClient::request(const char *method, const std::string &path, std::string &response)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    std::string url = api_url + path;
    std::string auth = "X-ZT1-Auth: " + token;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (std::string(method) == "POST") curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    long code = 0;
    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return code;
}

void ZeroTierClient::set_error(const std::string &err)
{
    std::lock_guard lock(state_mutex);
    state.last_error = err;
    keep_error_once = true;
}

void ZeroTierClient::do_refresh()
{
    State fresh{};
    fresh.token_found = load_token();
    fresh.last_update = std::chrono::steady_clock::now();

    if (fresh.token_found) {
        std::string resp{};
        long code = request("GET", "/status", resp);
        if (code == 200) {
            fresh.service_online = true;
            try {
                auto j = nlohmann::json::parse(resp);
                fresh.node_id = j.value("address", "");
                fresh.version = j.value("version", "");
                fresh.node_online = j.value("online", false);
            } catch (...) {}

            resp.clear();
            if (request("GET", "/network", resp) == 200) {
                try {
                    auto j = nlohmann::json::parse(resp);
                    for (const auto &n : j) {
                        Network net{};
                        net.id = n.value("id", n.value("nwid", ""));
                        net.name = n.value("name", "");
                        net.status = n.value("status", "");
                        net.device = n.value("portDeviceName", "");
                        if (n.contains("assignedAddresses") && n["assignedAddresses"].is_array()) {
                            for (const auto &a : n["assignedAddresses"]) {
                                if (a.is_string()) net.addresses.push_back(a.get<std::string>());
                            }
                        }
                        fresh.networks.push_back(std::move(net));
                    }
                } catch (...) {}
            }
        } else if (code == 401) {
            fresh.service_online = true;
            fresh.last_error = "ZeroTier rejected the auth token";
        } else {
            fresh.last_error = "ZeroTier service is not running";
        }
    } else {
        fresh.last_error = "ZeroTier auth token not found";
    }

    AddressCallback cb{};
    {
        std::lock_guard lock(state_mutex);
        // keep the error of a failed join/leave visible after the refresh that follows it
        if (fresh.last_error.empty() && keep_error_once) fresh.last_error = state.last_error;
        keep_error_once = false;
        fresh.busy = state.busy;
        state = fresh;
        cb = address_cb;
    }

    if (cb) {
        for (const auto &n : fresh.networks) {
            if (n.status != "OK") continue;
            for (const auto &a : n.addresses) {
                uint32_t ip{};
                int prefix{};
                if (parse_ipv4_cidr(a, ip, prefix)) cb(ip, prefix);
            }
        }
    }
}

bool ZeroTierClient::valid_network_id(const std::string &id)
{
    return id.size() == 16 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); });
}

bool ZeroTierClient::parse_ipv4(const std::string &str, uint32_t &ip)
{
    unsigned a, b, c, d;
    char tail = 0;
    if (std::sscanf(str.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

bool ZeroTierClient::parse_ipv4_cidr(const std::string &cidr, uint32_t &ip, int &prefix)
{
    auto slash = cidr.find('/');
    if (!parse_ipv4(cidr.substr(0, slash), ip)) return false;
    prefix = 32;
    if (slash != std::string::npos) {
        prefix = std::atoi(cidr.c_str() + slash + 1);
        if (prefix < 0 || prefix > 32) return false;
    }
    return true;
}

std::string ZeroTierClient::ipv4_to_string(uint32_t ip)
{
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

}

#endif // EMU_OVERLAY
