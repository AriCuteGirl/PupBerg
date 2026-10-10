/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef NETWORK_INCLUDE
#define NETWORK_INCLUDE

#include "base.h"
#include <curl/curl.h>

#define DEFAULT_PORT 47584
#define NUM_QUERY_PORTS 10

#if defined(STEAM_WIN32)
typedef unsigned int sock_t;
#else
typedef int sock_t;
#endif

static inline bool protobuf_message_equal(
    const google::protobuf::MessageLite& msg_a,
    const google::protobuf::MessageLite& msg_b)
{
  return (msg_a.GetTypeName() == msg_b.GetTypeName()) &&
      (msg_a.SerializeAsString() == msg_b.SerializeAsString());
}


struct IP_PORT {
    uint32 ip{};
    uint16 port{};
    bool operator <(const IP_PORT& other) const
    {
        return (ip < other.ip) || (ip == other.ip && port < other.port);
    }
};

struct Network_Callback {
    void (*message_callback)(void *object, Common_Message *msg) = nullptr;
    void *object{};
    CSteamID steam_id{};
};

enum Callback_Ids {
    CALLBACK_ID_USER_STATUS,
    CALLBACK_ID_LOBBY,
    CALLBACK_ID_NETWORKING,
    CALLBACK_ID_GAMESERVER,
    CALLBACK_ID_FRIEND,
    CALLBACK_ID_AUTH_TICKET,
    CALLBACK_ID_FRIEND_MESSAGES,
    CALLBACK_ID_NETWORKING_SOCKETS,
    CALLBACK_ID_STEAM_MESSAGES,
    CALLBACK_ID_NETWORKING_MESSAGES,
    CALLBACK_ID_GAMESERVER_STATS,
    CALLBACK_ID_LEADERBOARDS_STATS,
    CALLBACK_ID_USER_STATS,
    CALLBACK_ID_GAMESERVER_ITEMS,

    CALLBACK_IDS_MAX
};

struct Network_Callback_Container {
    std::vector<struct Network_Callback> callbacks{};
};

struct TCP_Socket {
    sock_t sock = static_cast<sock_t>(~0);
    bool received_data = false;
    std::vector<char> recv_buffer{};
    std::vector<char> send_buffer{};
    std::chrono::high_resolution_clock::time_point last_heartbeat_sent{}, last_heartbeat_received{};
};

struct Connection {
    struct TCP_Socket tcp_socket_outgoing{}, tcp_socket_incoming{};
    bool connected = false;
    IP_PORT udp_ip_port{};
    bool udp_pinged = false;
    IP_PORT tcp_ip_port{};
    std::vector<CSteamID> ids{};
    uint32 appid{};
    std::chrono::high_resolution_clock::time_point last_received{};
    bool relayed = false; // PupBerg: a lobby server room member, all traffic goes through the server
};

// PupBerg: lobby server (room codes over the internet, see tools/lobby_server)
struct Relay_Member {
    uint64 id{};
    uint32 appid{};
    std::string name{};
    std::vector<uint64> extra_ids{}; // ex: the friend's game server id
};

struct Relay_Room_Info {
    std::string code{};
    std::string host{};
    uint32 members{};
    uint32 appid{};
};

struct Relay_Status {
    enum class State { Off, Connecting, Connected, Error };
    State state = State::Off;
    std::string server{};
    std::string room{};
    bool is_public = false;
    std::string error{};
    std::vector<Relay_Member> members{}; // everyone else in the room
    std::vector<Relay_Room_Info> rooms{}; // last public room list
    bool rooms_loading = false;
};

class Networking
{
    bool enabled = false;
    bool query_alive{};
    std::chrono::high_resolution_clock::time_point last_run{};
    sock_t query_socket, udp_socket{}, tcp_socket{};
    uint16 udp_port{}, tcp_port{};
    uint32 own_ip{};
    std::vector<struct Connection> connections{};

    std::vector<CSteamID> ids;
    uint32 appid;
    std::chrono::high_resolution_clock::time_point last_broadcast;
    std::vector<IP_PORT> custom_broadcasts;
    // PupBerg: broadcast targets added at runtime (ex: ZeroTier peers), merged into custom_broadcasts in Run()
    std::mutex pending_custom_broadcasts_mutex{};
    std::vector<IP_PORT> pending_custom_broadcasts{};

    std::vector<struct TCP_Socket> accepted;
    std::recursive_mutex mutex;

    struct Network_Callback_Container callbacks[CALLBACK_IDS_MAX];
    std::vector<Common_Message> local_send;

    // PupBerg: lobby server relay, only touched from Run() except the command/status members
    struct Relay {
        sock_t sock = static_cast<sock_t>(~0);
        std::vector<char> recv_buffer{};
        std::vector<char> send_buffer{};
        bool welcomed = false;
        bool joined = false;       // HELLO sent on this connection
        bool want_room = false;    // stay in a room, reconnect if dropped
        bool want_list = false;    // a room list request is pending
        uint32 list_appid = 0;
        std::string host{};
        uint16 port = 0;
        std::string room{};
        std::string name{};
        bool is_public = false;
        std::chrono::high_resolution_clock::time_point opened{}, last_ping{}, last_received{}, retry_at{};
        std::vector<Relay_Member> members{};
    } relay{};
    struct Relay_Command {
        bool join = false;
        bool leave = false;
        bool list = false;
        std::string server{};
        std::string room{};
        std::string name{};
        bool is_public = false;
        uint32 list_appid = 0;
    } relay_cmd{};
    std::mutex relay_mutex{}; // guards relay_cmd and relay_state
    Relay_Status relay_state{};

    void relay_run();
    bool relay_open();
    void relay_close(const std::string &error, bool keep_room);
    void relay_queue_frame(uint8 type, const std::string &payload);
    void relay_handle_frame(uint8 type, const char *data, size_t len);
    void relay_member_joined(const Relay_Member &member);
    void relay_member_left(uint64 id);
    void relay_drop_members();
    void relay_publish_state(Relay_Status::State state, const std::string &error);
    bool relay_send_message(uint64 dest, Common_Message *msg);
    bool relay_flush(); // false when the connection broke

    struct Connection *find_connection(CSteamID id, uint32 appid = 0);
    struct Connection *new_connection(CSteamID id, uint32 appid);

    bool handle_announce(Common_Message *msg, IP_PORT ip_port);
    bool handle_low_level_udp(Common_Message *msg, IP_PORT ip_port);
    bool handle_tcp(Common_Message *msg, struct TCP_Socket &socket);
    void send_announce_broadcasts();

    bool add_id_connection(struct Connection *connection, CSteamID steam_id);
    void run_callbacks(Callback_Ids id, Common_Message *msg);
    void run_callback_user(CSteamID steam_id, bool online, uint32 appid);
    void do_callbacks_message(Common_Message *msg);

    Common_Message create_announce(bool request);


public:
    Networking(CSteamID id, uint32 appid, uint16 port, std::set<IP_PORT> *custom_broadcasts, bool disable_sockets);
    ~Networking();
    
    //NOTE: for all functions ips/ports are passed/returned in host byte order
    //ex: 127.0.0.1 should be passed as 0x7F000001
    static std::set<IP_PORT> resolve_ip(std::string dns);
    
    void addListenId(CSteamID id);
    // PupBerg: thread-safe, ip/port in host byte order, port 0 = our own listen port
    void add_custom_broadcast(uint32 ip, uint16 port = 0);

    // PupBerg lobby server, safe to call from any thread, applied on the next Run()
    // server is "host:port", room is a 4-32 chars code
    void relay_join(const std::string &server, const std::string &room, const std::string &name, bool is_public);
    void relay_leave();
    void relay_request_rooms(const std::string &server, uint32 appid);
    Relay_Status relay_status();
    static uint32 relay_virtual_ip(uint64 steam_id);
    void setAppID(uint32 appid);
    void Run();

    // send to a specific user, set_dest_id() must be called
    bool sendTo(Common_Message *msg, bool reliable, Connection *conn = NULL);
    
    // send to all users whose account type is Individual, no need to call set_dest_id(), this is done automatically
    bool sendToAllIndividuals(Common_Message *msg, bool reliable);

    // send to all users whose account type is GameServer, no need to call set_dest_id(), this is done automatically
    bool sendToAllGameservers(Common_Message *msg, bool reliable);

    // send to all active/current connections, no need to call set_dest_id(), this is done automatically
    bool sendToAll(Common_Message *msg, bool reliable);
    
    // send to active/current connections with specific ip/port, no need to call set_dest_id(), this is done automatically
    //TODO: actually send to ip/port
    bool sendToIPPort(Common_Message *msg, uint32 ip, uint16 port, bool reliable);

    bool setCallback(Callback_Ids id, CSteamID steam_id, void (*message_callback)(void *object, Common_Message *msg), void *object);
    void rmCallback(Callback_Ids id, CSteamID steam_id, void (*message_callback)(void *object, Common_Message *msg), void *object);

    uint32 getIP(CSteamID id);
    uint16 getPort(CSteamID id);
    uint32 getOwnIP();

    void startQuery(IP_PORT ip_port);
    void shutDownQuery();
    bool isQueryAlive();
};

#endif // NETWORK_INCLUDE_H
