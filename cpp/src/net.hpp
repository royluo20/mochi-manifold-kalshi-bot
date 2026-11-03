// net.hpp — non-blocking TCP/TLS streams, an RFC 6455 WebSocket client (the
// `websockets` library's role) and a keep-alive HTTP/1.1 pool (aiohttp's
// TCPConnector: limit 100, LIFO reuse, 300 s keepalive). OpenSSL only.
#pragma once

#include <openssl/ssl.h>
#include <sys/socket.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "loop.hpp"

namespace net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Url {
    bool tls = true;
    std::string host;
    int port = 443;
    std::string path = "/";  // path + query
};
bool parse_url(const std::string& url, Url& out);  // ws:// wss:// http:// https://

using Addrs = std::vector<std::pair<sockaddr_storage, socklen_t>>;

// aiohttp's _DNSCacheTable + aiohappyeyeballs ordering, per connection pool:
// each new connection gets the resolved list rotated one step further, then
// interleaved by address family (first_address_family_count=1). Successive
// connections therefore spread over every address DNS returned (both families),
// exactly as aiohttp's do, instead of all dialing the first one.
class DnsTable {
public:
    explicit DnsTable(double ttl_sec = 600) : ttl_(ttl_sec) {}
    Addrs next_addrs(const std::string& host, int port, std::string& err);

private:
    struct Entry {
        Addrs addrs;
        size_t offset = 0;
        uint64_t version = 0;  // the resolver result this rotation cycle started from
    };
    std::vector<std::pair<std::string, Entry>> table_;
    double ttl_;
};

std::string base64(const unsigned char* data, size_t n);
std::string url_quote(std::string_view s);  // percent-encode a query value
void random_bytes(unsigned char* out, size_t n);

// ---------------------------------------------------------------------------

class Stream : public std::enable_shared_from_this<Stream> {
public:
    enum class State { Idle, Connecting, Handshaking, Open, Closed };

    Stream(std::string host, int port, bool tls);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Resolve (cached 600 s), connect each address in getaddrinfo order, TLS
    // handshake (SNI + hostname verification, ALPN http/1.1).
    void connect(double timeout_sec, std::function<void(bool ok, const std::string& err)> cb);
    // Dial these addresses, in this order, instead of resolving (HttpPool's DnsTable).
    void set_addresses(Addrs addrs) { preset_ = std::move(addrs); }
    // Write now if possible; the remainder is queued. False if not open.
    bool send(std::string_view data);
    // Local close: no on_close callback.
    void close();

    bool is_open() const { return state_ == State::Open; }
    State state() const { return state_; }
    const std::string& host() const { return host_; }
    const std::string& peer() const { return peer_; }

    std::string rx;                                    // unread input
    std::function<void()> on_data;                     // rx grew
    std::function<void(const std::string&)> on_close;  // remote close / error, once

private:
    void try_addr(size_t k);
    void on_io(bool r, bool w, bool err);
    void tcp_connected();
    void do_handshake();
    void opened();
    void do_read();
    void do_write();
    void update_interest();
    void fail(const std::string& why);

    std::string host_;
    int port_;
    bool tls_;
    State state_ = State::Idle;
    int fd_ = -1;
    SSL* ssl_ = nullptr;
    std::string tx_;
    bool want_write_ = false;
    Addrs addrs_, preset_;
    size_t addr_idx_ = 0;
    std::string last_err_;
    std::string peer_;
    ev::Timer connect_timer_;
    std::function<void(bool, const std::string&)> connect_cb_;
};

// ---------------------------------------------------------------------------

struct WsOptions {
    size_t max_size = 1 << 20;
    double ping_interval = 20;  // library keepalive; <= 0 disables
    double ping_timeout = 20;
    double close_timeout = 10;
    double open_timeout = 10;
    Headers headers;
};

class WsClient : public std::enable_shared_from_this<WsClient> {
public:
    enum class State { Idle, Connecting, Open, Closing, Closed };

    std::function<void()> on_open;
    std::function<void(std::string_view)> on_message;
    // clean = closed with 1000/1001/1005 by a close handshake (ConnectionClosedOK)
    std::function<void(bool clean, int code, const std::string& reason)> on_close;

    void connect(const std::string& url, WsOptions opt);
    bool send_text(std::string_view s);
    // ws.ping(): ok=true when this ping (or a later one) is answered.
    void ping(double timeout, std::function<void(bool ok, double rtt_sec)> cb);
    // Closing handshake (async with __aexit__): on_close fires when done.
    void close(int code = 1000);
    void abort(const std::string& why);  // drop the connection now
    bool is_open() const { return state_ == State::Open; }
    State state() const { return state_; }
    ~WsClient();

private:
    struct PingWait {
        std::string payload;
        double sent_at;
        std::function<void(bool, double)> cb;
        ev::Timer timer;
    };
    void on_stream_data();
    void on_handshake_data();
    void handle_frames();
    void send_frame(int opcode, std::string_view payload);
    void fail(int code, const std::string& why);
    void finish(bool clean, int code, const std::string& why);
    void schedule_keepalive(double delay);
    void resolve_pings(const std::string& payload);

    std::shared_ptr<Stream> stream_;
    WsOptions opt_;
    Url url_;
    State state_ = State::Idle;
    std::string key_;
    std::string msg_;       // fragmented message being assembled
    bool in_message_ = false;
    std::deque<PingWait> pings_;
    ev::Timer keepalive_timer_, keepalive_wait_, close_timer_, open_timer_;
    double latency_ = 0;
    bool close_sent_ = false;
    bool finished_ = false;
    int close_code_ = 1006;
    std::string close_reason_;
};

// ---------------------------------------------------------------------------

struct RequestTiming {
    double headers_at = -1;     // perf_counter when the request bytes were handed to the socket
    double pool_queued_at = -1; // waiting for connector capacity since
    double pool_wait_ms = 0;
    int reused = -1;            // -1 unknown, 0 new connection, 1 reused
};

struct HttpResult {
    enum Kind { Ok, ConnectError, Timeout, Disconnected, Protocol };
    Kind kind = Ok;
    int status = 0;
    std::string body;
    std::string error;
};

class HttpPool {
public:
    HttpPool(std::string host, int port, bool tls, Headers default_headers, size_t limit = 100,
             double keepalive_sec = 300);
    ~HttpPool();

    // timeout = total (connect + send + response), like aiohttp ClientTimeout(total=...).
    void request(const std::string& method, const std::string& path, const std::string& body,
                 double timeout, std::function<void(HttpResult)> cb, RequestTiming* timing = nullptr,
                 const Headers& extra = {});
    void close_all();
    size_t idle() const { return idle_.size(); }
    size_t total() const { return conns_.size(); }

    struct Pending;  // one request's completion state (callback, timeout, serving conn)
    struct Conn;

private:
    struct Req {
        std::string method, path, body;
        std::shared_ptr<Pending> p;
        RequestTiming* timing = nullptr;
        Headers extra;
    };
    void dispatch(Req req);
    void start_on(const std::shared_ptr<Conn>& c, Req req, bool reused);
    void on_conn_data(const std::shared_ptr<Conn>& c);
    void complete(const std::shared_ptr<Conn>& c, HttpResult res, bool keep);
    void drop(const std::shared_ptr<Conn>& c);
    void release(const std::shared_ptr<Conn>& c);
    std::string build(const Req& r) const;

    std::string host_;
    int port_;
    bool tls_;
    Headers headers_;
    size_t limit_;
    double keepalive_;
    std::string prefix_cache_;
    DnsTable dns_;  // one per pool, like one TCPConnector per aiohttp session
    std::vector<std::shared_ptr<Conn>> conns_;  // all live connections
    std::vector<std::shared_ptr<Conn>> idle_;   // LIFO
    std::deque<Req> waiting_;
    bool closed_ = false;
};

}  // namespace net
