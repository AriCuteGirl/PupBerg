#ifndef __INCLUDED_PUP_ZEROTIER_H__
#define __INCLUDED_PUP_ZEROTIER_H__

#ifdef EMU_OVERLAY

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <functional>
#include <deque>
#include <atomic>
#include <chrono>

namespace pupberg {

// talks to the local ZeroTier One service (http://127.0.0.1:9993) on a worker thread
// docs: https://docs.zerotier.com/api/service/ref-v1
class ZeroTierClient
{
public:
    struct Network
    {
        std::string id{};
        std::string name{};
        std::string status{}; // OK, REQUESTING_CONFIGURATION, ACCESS_DENIED, NOT_FOUND, ...
        std::string device{};
        std::vector<std::string> addresses{}; // CIDR, ex: 10.147.17.5/24
    };

    struct State
    {
        bool token_found = false;
        bool service_online = false; // service answered /status
        bool node_online = false;    // node reached the ZeroTier roots
        bool busy = false;
        std::string node_id{};
        std::string version{};
        std::string last_error{};
        std::vector<Network> networks{};
        std::chrono::steady_clock::time_point last_update{};
    };

    // called from the worker thread with the IPv4 (host byte order) and prefix of each assigned address
    using AddressCallback = std::function<void(uint32_t ip, int prefix)>;

    ZeroTierClient(std::string api_url, std::string token, std::string token_path);
    ~ZeroTierClient();

    void set_address_callback(AddressCallback cb);

    State snapshot();
    void refresh();
    void join(const std::string &network_id);
    void leave(const std::string &network_id);
    // refresh periodically while `fast` (overlay shown) or slowly in the background
    void set_fast_polling(bool fast);

    static bool valid_network_id(const std::string &id);
    // parses "10.147.17.5/24", returns false for IPv6
    static bool parse_ipv4_cidr(const std::string &cidr, uint32_t &ip, int &prefix);
    static std::string ipv4_to_string(uint32_t ip);
    static bool parse_ipv4(const std::string &str, uint32_t &ip);

private:
    std::string api_url{};
    std::string token{};
    std::string token_path_override{};

    std::mutex state_mutex{};
    State state{};
    bool keep_error_once = false;
    AddressCallback address_cb{};

    std::mutex jobs_mutex{};
    std::condition_variable jobs_cv{};
    std::deque<std::function<void()>> jobs{};
    std::atomic<bool> fast_polling = false;
    bool stop = false;
    std::thread worker{};

    void push_job(std::function<void()> job);
    void worker_proc();

    bool load_token();
    // returns HTTP status code, or 0 on connection error
    long request(const char *method, const std::string &path, std::string &response);
    void do_refresh();
    void set_error(const std::string &err);
};

}

#endif // EMU_OVERLAY

#endif // __INCLUDED_PUP_ZEROTIER_H__
