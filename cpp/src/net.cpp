#include "net.hpp"

#include <cctype>
#include <cstdlib>
#include <iterator>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "clock.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace net {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

SSL_CTX* client_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(c);
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        SSL_CTX_set_alpn_protos(c, alpn, sizeof alpn);
        return c;
    }();
    return ctx;
}

std::string ssl_errstr() {
    unsigned long e = ERR_get_error();
    if (!e) return "ssl error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

Addrs gai_lookup(const std::string& host, int port, int flags, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0) {
        err = gai_strerror(rc);
        return {};
    }
    Addrs out;
    for (addrinfo* a = res; a; a = a->ai_next) {
        sockaddr_storage ss{};
        std::memcpy(&ss, a->ai_addr, a->ai_addrlen);
        out.emplace_back(ss, (socklen_t)a->ai_addrlen);
    }
    freeaddrinfo(res);
    if (out.empty()) err = "no addresses";
    return out;
}

// Name lookups never block the event loop once a host has been resolved: an
// expired entry keeps serving its addresses while ONE background thread
// re-resolves it. aiohttp's ThreadedResolver and asyncio's create_connection
// resolve in a thread too; an inline getaddrinfo froze every feed and order for
// the resolver's timeout whenever DNS was slow. Only a host's very first lookup
// (startup) runs inline. A failed refresh keeps the old addresses and retries
// 30 s later.
struct DnsCache {
    struct Rec {
        Addrs addrs;
        double stamp = 0;
        uint64_t version = 0;  // bumps when a refresh lands
        bool refreshing = false;
    };
    std::mutex mu;
    std::map<std::string, Rec> recs;
};

const std::shared_ptr<DnsCache>& dns_cache() {
    static const auto cache = std::make_shared<DnsCache>();  // threads hold their own reference
    return cache;
}

Addrs cached_lookup(const std::string& host, int port, int flags, double ttl, std::string& err,
                    uint64_t* version = nullptr) {
    const auto& cache = dns_cache();
    std::string key = host + ":" + std::to_string(port) + ":" + std::to_string(flags);
    double now = ev::Loop::monotonic();
    {
        std::lock_guard<std::mutex> lk(cache->mu);
        auto it = cache->recs.find(key);
        if (it != cache->recs.end() && !it->second.addrs.empty()) {
            auto& r = it->second;
            if (now - r.stamp > ttl && !r.refreshing) {
                r.refreshing = true;
                std::thread([cache, key, host, port, flags, ttl] {
                    std::string e2;
                    Addrs fresh = gai_lookup(host, port, flags, e2);
                    std::lock_guard<std::mutex> lk2(cache->mu);
                    auto& r2 = cache->recs[key];
                    r2.refreshing = false;
                    if (!fresh.empty()) {
                        r2.addrs = std::move(fresh);
                        r2.stamp = ev::Loop::monotonic();
                        ++r2.version;
                    } else {
                        r2.stamp = ev::Loop::monotonic() - ttl + 30;
                    }
                }).detach();
            }
            if (version) *version = r.version;
            return r.addrs;
        }
    }
    Addrs fresh = gai_lookup(host, port, flags, err);
    if (fresh.empty()) return {};
    std::lock_guard<std::mutex> lk(cache->mu);
    auto& r = cache->recs[key];
    if (r.addrs.empty()) {
        r.addrs = std::move(fresh);
        r.stamp = now;
        ++r.version;
    }
    if (version) *version = r.version;
    return r.addrs;
}

// asyncio create_connection: getaddrinfo(type=SOCK_STREAM), no flags.
Addrs resolve(const std::string& host, int port, std::string& err) { return cached_lookup(host, port, 0, 600, err); }

std::string addr_str(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET, &((const sockaddr_in*)&ss)->sin_addr, buf, sizeof buf);
    else if (ss.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((const sockaddr_in6*)&ss)->sin6_addr, buf, sizeof buf);
    return buf;
}

bool is_ip_literal(const std::string& h) {
    unsigned char b[16];
    return inet_pton(AF_INET, h.c_str(), b) == 1 || inet_pton(AF_INET6, h.c_str(), b) == 1;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// xorshift for WebSocket masks (masking is not a secrecy mechanism)
uint64_t g_rng = 0;
uint32_t fast_rand() {
    if (!g_rng) {
        RAND_bytes((unsigned char*)&g_rng, sizeof g_rng);
        g_rng |= 1;
    }
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

}  // namespace

Addrs DnsTable::next_addrs(const std::string& host, int port, std::string& err) {
    const std::string key = host + ":" + std::to_string(port);
    // ThreadedResolver: getaddrinfo(type=SOCK_STREAM, family=AF_UNSPEC, flags=AI_ADDRCONFIG),
    // cached for ttl_; refreshed off the loop thread (cached_lookup).
    uint64_t version = 0;
    Addrs resolved = cached_lookup(host, port, AI_ADDRCONFIG, ttl_, err, &version);
    if (resolved.empty()) return {};
    Entry* e = nullptr;
    for (auto& [k, v] : table_)
        if (k == key) e = &v;
    if (!e) {
        table_.emplace_back(key, Entry{});
        e = &table_.back().second;
    }
    if (e->addrs.empty() || e->version != version) {
        e->addrs = std::move(resolved);
        e->offset = 0;  // add(): a fresh cycle
        e->version = version;
    }
    // next_addrs(): the whole list from the current offset, then shift by one.
    const size_t n = e->addrs.size();
    Addrs rotated;
    for (size_t i = 0; i < n; ++i) rotated.push_back(e->addrs[(e->offset + i) % n]);
    e->offset = (e->offset + 1) % n;
    // _interleave_addrinfos(first_address_family_count=1): families in order of
    // first appearance, then zip_longest across them.
    std::vector<int> fams;
    std::vector<Addrs> groups;
    for (const auto& a : rotated) {
        int f = a.first.ss_family;
        size_t gi = 0;
        while (gi < fams.size() && fams[gi] != f) ++gi;
        if (gi == fams.size()) {
            fams.push_back(f);
            groups.emplace_back();
        }
        groups[gi].push_back(a);
    }
    Addrs out;
    for (size_t i = 0;; ++i) {
        bool any = false;
        for (const auto& g : groups)
            if (i < g.size()) {
                out.push_back(g[i]);
                any = true;
            }
        if (!any) break;
    }
    return out;
}

bool parse_url(const std::string& url, Url& out) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    std::string scheme = lower(url.substr(0, p));
    if (scheme == "wss" || scheme == "https") out.tls = true;
    else if (scheme == "ws" || scheme == "http") out.tls = false;
    else return false;
    out.port = out.tls ? 443 : 80;
    size_t hs = p + 3;
    size_t slash = url.find('/', hs);
    std::string hostport = url.substr(hs, slash == std::string::npos ? std::string::npos : slash - hs);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (!hostport.empty() && hostport[0] == '[') {  // [v6]:port
        size_t rb = hostport.find(']');
        if (rb == std::string::npos) return false;
        out.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') out.port = std::atoi(hostport.c_str() + rb + 2);
    } else {
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            out.host = hostport.substr(0, colon);
            out.port = std::atoi(hostport.c_str() + colon + 1);
        } else {
            out.host = hostport;
        }
    }
    return !out.host.empty() && out.port > 0;
}

std::string base64(const unsigned char* data, size_t n) {
    std::string out(4 * ((n + 2) / 3) + 1, '\0');
    int len = EVP_EncodeBlock((unsigned char*)out.data(), data, (int)n);
    out.resize((size_t)len);
    return out;
}

std::string url_quote(std::string_view s) {
    static const char* HEX = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(HEX[c >> 4]);
            out.push_back(HEX[c & 15]);
        }
    }
    return out;
}

void random_bytes(unsigned char* out, size_t n) { RAND_bytes(out, (int)n); }

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

Stream::Stream(std::string host, int port, bool tls) : host_(std::move(host)), port_(port), tls_(tls) {}

Stream::~Stream() { close(); }

void Stream::connect(double timeout_sec, std::function<void(bool, const std::string&)> cb) {
    connect_cb_ = std::move(cb);
    state_ = State::Connecting;
    std::weak_ptr<Stream> w = weak_from_this();
    connect_timer_ = ev::Loop::get().call_later(timeout_sec, [w] {
        if (auto s = w.lock()) s->fail("connect timeout");
    });
    // Start on the next iteration so every failure is reported asynchronously.
    ev::Loop::get().call_soon([w] {
        auto s = w.lock();
        if (!s || s->state_ != State::Connecting) return;
        std::string err;
        if (!s->preset_.empty()) s->addrs_ = std::move(s->preset_);
        else s->addrs_ = resolve(s->host_, s->port_, err);
        if (s->addrs_.empty()) {
            s->fail("dns: " + err);
            return;
        }
        s->try_addr(0);
    });
}

void Stream::try_addr(size_t k) {
    if (state_ != State::Connecting) return;
    if (fd_ >= 0) {
        ev::Loop::get().unwatch(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    if (k >= addrs_.size()) {
        fail("connect failed: " + last_err_);
        return;
    }
    addr_idx_ = k;
    const auto& [ss, len] = addrs_[k];
    int fd = ::socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        last_err_ = std::strerror(errno);
        try_addr(k + 1);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fd_ = fd;
    peer_ = addr_str(ss);
    std::weak_ptr<Stream> w = weak_from_this();
    ev::Loop::get().watch(fd, false, true, [w](bool r, bool wr, bool e) {
        if (auto s = w.lock()) s->on_io(r, wr, e);
    });
    int r = ::connect(fd, (const sockaddr*)&ss, len);
    if (r == 0) {
        tcp_connected();
        return;
    }
    if (errno != EINPROGRESS) {
        last_err_ = std::strerror(errno);
        try_addr(k + 1);
        return;
    }
    // A blackholed address family must not eat the whole connect budget, so
    // after 1.5 s move on to the next address. The LAST address keeps the whole
    // budget (the caller's connect timeout), as asyncio/websockets wait for it:
    // cutting it too made a slow-but-working server (a lost SYN, an overloaded
    // accept queue) fail where Python connects.
    if (k + 1 < addrs_.size()) {
        ev::Loop::get().call_later(1.5, [w, k] {
            auto s = w.lock();
            if (s && s->state_ == State::Connecting && s->addr_idx_ == k && !s->ssl_) {
                s->last_err_ = "attempt timeout";
                s->try_addr(k + 1);
            }
        });
    }
}

void Stream::on_io(bool r, bool w, bool err) {
    switch (state_) {
        case State::Connecting: {
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) {
                last_err_ = std::strerror(soerr);
                try_addr(addr_idx_ + 1);
                return;
            }
            if (!w && !err) return;
            tcp_connected();
            return;
        }
        case State::Handshaking:
            do_handshake();
            return;
        case State::Open:
            if (r || err) do_read();
            if (state_ == State::Open && w) do_write();
            return;
        default:
            return;
    }
}

void Stream::tcp_connected() {
    if (!tls_) {
        opened();
        return;
    }
    ssl_ = SSL_new(client_ctx());
    SSL_set_fd(ssl_, fd_);
    if (!is_ip_literal(host_)) SSL_set_tlsext_host_name(ssl_, host_.c_str());
    SSL_set1_host(ssl_, host_.c_str());
    SSL_set_connect_state(ssl_);
    state_ = State::Handshaking;
    do_handshake();
}

void Stream::do_handshake() {
    ERR_clear_error();
    int r = SSL_do_handshake(ssl_);
    if (r == 1) {
        opened();
        return;
    }
    int e = SSL_get_error(ssl_, r);
    if (e == SSL_ERROR_WANT_READ) {
        ev::Loop::get().set_interest(fd_, true, false);
        return;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        ev::Loop::get().set_interest(fd_, false, true);
        return;
    }
    long vr = SSL_get_verify_result(ssl_);
    fail(std::string("tls handshake failed: ") +
         (vr != X509_V_OK ? X509_verify_cert_error_string(vr) : ssl_errstr()));
}

void Stream::opened() {
    state_ = State::Open;
    ev::Loop::get().cancel(connect_timer_);
    update_interest();
    auto cb = std::move(connect_cb_);
    connect_cb_ = nullptr;
    auto self = shared_from_this();
    if (cb) cb(true, "");
    if (state_ == State::Open && tls_ && SSL_pending(ssl_) > 0) do_read();
}

void Stream::update_interest() {
    if (fd_ >= 0) ev::Loop::get().set_interest(fd_, true, want_write_);
}

void Stream::do_read() {
    char buf[65536];
    bool got = false;
    std::string closing;
    for (int rounds = 0; state_ == State::Open && rounds < 32; ++rounds) {
        if (tls_) {
            ERR_clear_error();
            int n = SSL_read(ssl_, buf, (int)sizeof buf);
            if (n > 0) {
                rx.append(buf, (size_t)n);
                got = true;
                continue;
            }
            int e = SSL_get_error(ssl_, n);
            if (e == SSL_ERROR_WANT_READ) break;
            if (e == SSL_ERROR_WANT_WRITE) {
                want_write_ = true;
                update_interest();
                break;
            }
            closing = e == SSL_ERROR_ZERO_RETURN ? "closed by peer"
                      : e == SSL_ERROR_SYSCALL   ? "connection reset"
                                                 : ssl_errstr();
            break;
        } else {
            ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n > 0) {
                rx.append(buf, (size_t)n);
                got = true;
                continue;
            }
            if (n == 0) {
                closing = "closed by peer";
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            closing = std::strerror(errno);
            break;
        }
    }
    auto self = shared_from_this();
    if (got && on_data) on_data();
    if (!closing.empty()) fail(closing);
}

void Stream::do_write() {
    while (!tx_.empty() && state_ == State::Open) {
        if (tls_) {
            ERR_clear_error();
            int n = SSL_write(ssl_, tx_.data(), (int)tx_.size());
            if (n > 0) {
                tx_.erase(0, (size_t)n);
                continue;
            }
            int e = SSL_get_error(ssl_, n);
            if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                want_write_ = true;
                update_interest();
                return;
            }
            fail(ssl_errstr());
            return;
        } else {
            ssize_t n = ::send(fd_, tx_.data(), tx_.size(), MSG_NOSIGNAL);
            if (n > 0) {
                tx_.erase(0, (size_t)n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                want_write_ = true;
                update_interest();
                return;
            }
            if (n < 0 && errno == EINTR) continue;
            fail(std::strerror(errno));
            return;
        }
    }
    want_write_ = false;
    update_interest();
}

bool Stream::send(std::string_view data) {
    if (state_ != State::Open) return false;
    if (!tx_.empty()) {
        tx_.append(data);
        return true;
    }
    // Fast path: straight to the socket, no buffering, no loop hop.
    size_t written = 0;
    if (tls_) {
        ERR_clear_error();
        int n = SSL_write(ssl_, data.data(), (int)data.size());
        if (n > 0) {
            written = (size_t)n;
        } else {
            int e = SSL_get_error(ssl_, n);
            if (e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ) {
                fail(ssl_errstr());
                return false;
            }
        }
    } else {
        ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n > 0) {
            written = (size_t)n;
        } else if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
            fail(std::strerror(errno));
            return false;
        }
    }
    if (written == data.size()) return true;
    tx_.assign(data.substr(written));
    want_write_ = true;
    update_interest();
    return true;
}

void Stream::close() {
    ev::Loop::get().cancel(connect_timer_);
    if (ssl_) {
        if (state_ == State::Open) SSL_shutdown(ssl_);  // best-effort close_notify
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        ev::Loop::get().unwatch(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    state_ = State::Closed;
    connect_cb_ = nullptr;
}

void Stream::fail(const std::string& why) {
    if (state_ == State::Closed) return;
    bool connecting = state_ == State::Connecting || state_ == State::Handshaking;
    auto cb = std::move(connect_cb_);
    connect_cb_ = nullptr;
    auto self = shared_from_this();
    close();
    if (connecting) {
        if (cb) cb(false, why);
    } else if (on_close) {
        auto f = on_close;
        f(why);
    }
}

// ---------------------------------------------------------------------------
// WsClient
// ---------------------------------------------------------------------------

namespace {
std::string ws_accept(const std::string& key) {
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    EVP_Digest(src.data(), src.size(), md, &mdlen, EVP_sha1(), nullptr);
    return base64(md, mdlen);
}
}  // namespace

WsClient::~WsClient() {
    ev::Loop& L = ev::Loop::get();
    L.cancel(keepalive_timer_);
    L.cancel(keepalive_wait_);
    L.cancel(close_timer_);
    L.cancel(open_timer_);
    for (auto& p : pings_) L.cancel(p.timer);
    if (stream_) stream_->close();
}

void WsClient::connect(const std::string& url, WsOptions opt) {
    opt_ = std::move(opt);
    std::weak_ptr<WsClient> w = weak_from_this();
    if (!parse_url(url, url_)) {
        state_ = State::Connecting;
        ev::Loop::get().call_soon([w] {
            if (auto c = w.lock()) c->finish(false, 1006, "bad url");
        });
        return;
    }
    state_ = State::Connecting;
    stream_ = std::make_shared<Stream>(url_.host, url_.port, url_.tls);
    stream_->on_data = [w] {
        if (auto c = w.lock()) c->on_stream_data();
    };
    stream_->on_close = [w](const std::string& why) {
        if (auto c = w.lock()) c->finish(false, 1006, why);
    };
    open_timer_ = ev::Loop::get().call_later(opt_.open_timeout, [w] {
        auto c = w.lock();
        if (c && c->state_ == State::Connecting) c->abort("timed out during opening handshake");
    });
    stream_->connect(opt_.open_timeout, [w](bool ok, const std::string& err) {
        auto c = w.lock();
        if (!c) return;
        if (!ok) {
            c->finish(false, 1006, err);
            return;
        }
        unsigned char k[16];
        random_bytes(k, sizeof k);
        c->key_ = base64(k, sizeof k);
        std::string host = c->url_.host;
        if (host.find(':') != std::string::npos) host = "[" + host + "]";
        if (c->url_.port != (c->url_.tls ? 443 : 80)) host += ":" + std::to_string(c->url_.port);
        std::string req = "GET " + c->url_.path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + c->key_ +
                          "\r\nSec-WebSocket-Version: 13\r\n";
        for (const auto& [k2, v] : c->opt_.headers) req += k2 + ": " + v + "\r\n";
        req += "User-Agent: manifold-bot-cpp/1.0\r\n\r\n";
        c->stream_->send(req);
    });
}

void WsClient::on_stream_data() {
    if (state_ == State::Connecting) {
        on_handshake_data();
        return;
    }
    handle_frames();
}

void WsClient::on_handshake_data() {
    std::string& rx = stream_->rx;
    size_t pos = rx.find("\r\n\r\n");
    if (pos == std::string::npos) {
        if (rx.size() > 65536) abort("handshake response too large");
        return;
    }
    std::string head = rx.substr(0, pos);
    rx.erase(0, pos + 4);
    int status = 0;
    if (head.size() > 12 && head.compare(0, 5, "HTTP/") == 0) status = std::atoi(head.c_str() + 9);
    if (status != 101) {
        abort("server rejected WebSocket connection: HTTP " + std::to_string(status));
        return;
    }
    std::string accept;
    size_t lp = head.find("\r\n");
    while (lp != std::string::npos) {
        size_t next = head.find("\r\n", lp + 2);
        std::string line = head.substr(lp + 2, next == std::string::npos ? std::string::npos : next - lp - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos && lower(line.substr(0, colon)) == "sec-websocket-accept") {
            size_t vb = colon + 1;
            while (vb < line.size() && line[vb] == ' ') ++vb;
            accept = line.substr(vb);
        }
        lp = next;
    }
    if (accept != ws_accept(key_)) {
        abort("invalid Sec-WebSocket-Accept");
        return;
    }
    state_ = State::Open;
    ev::Loop::get().cancel(open_timer_);
    if (opt_.ping_interval > 0) schedule_keepalive(opt_.ping_interval);
    auto self = shared_from_this();
    if (on_open) on_open();
    if (state_ == State::Open && stream_ && !stream_->rx.empty()) handle_frames();
}

void WsClient::handle_frames() {
    auto self = shared_from_this();
    auto st = stream_;  // keep the buffer alive across callbacks
    if (!st) return;
    std::string& rx = st->rx;
    size_t off = 0;
    int delivered = 0;
    while ((state_ == State::Open || state_ == State::Closing) && stream_ == st) {
        size_t avail = rx.size() - off;
        if (avail < 2) break;
        const unsigned char* p = (const unsigned char*)rx.data() + off;
        bool fin = (p[0] & 0x80) != 0;
        int rsv = p[0] & 0x70;
        int opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t len = p[1] & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = ((uint64_t)p[2] << 8) | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int k = 0; k < 8; ++k) len = (len << 8) | p[2 + k];
            hdr = 10;
        }
        if (rsv) {
            rx.erase(0, off);
            fail(1002, "reserved bits must be 0");
            return;
        }
        if (masked) {
            rx.erase(0, off);
            fail(1002, "incorrect masking");
            return;
        }
        bool control = opcode >= 0x8;
        if (control && (len > 125 || !fin)) {  // RFC 6455 5.5; also keeps hdr + len from wrapping
            rx.erase(0, off);
            fail(1002, "invalid control frame");
            return;
        }
        if (!control && (len > opt_.max_size || (in_message_ && msg_.size() + len > opt_.max_size))) {
            rx.erase(0, off);
            fail(1009, "message too big");
            return;
        }
        if (avail < hdr + len) break;
        const char* payload = rx.data() + off + hdr;
        size_t plen = (size_t)len;
        off += hdr + plen;
        switch (opcode) {
            case 0x1:
            case 0x2:
                if (in_message_) {
                    rx.erase(0, off);
                    fail(1002, "expected a continuation frame");
                    return;
                }
                if (fin) {
                    ++delivered;
                    if (on_message) on_message(std::string_view(payload, plen));
                } else {
                    msg_.assign(payload, plen);
                    in_message_ = true;
                }
                break;
            case 0x0:
                if (!in_message_) {
                    rx.erase(0, off);
                    fail(1002, "unexpected continuation frame");
                    return;
                }
                msg_.append(payload, plen);
                if (fin) {
                    in_message_ = false;
                    std::string m;
                    m.swap(msg_);
                    ++delivered;
                    if (on_message) on_message(m);
                }
                break;
            case 0x8: {
                int code = 1005;
                std::string reason;
                if (plen >= 2) {
                    code = ((unsigned char)payload[0] << 8) | (unsigned char)payload[1];
                    reason.assign(payload + 2, plen - 2);
                }
                rx.erase(0, off);
                bool we_closed = close_sent_;
                if (!close_sent_) {
                    unsigned char cb[2] = {(unsigned char)(code >> 8), (unsigned char)(code & 0xff)};
                    send_frame(0x8, code == 1005 ? std::string_view() : std::string_view((const char*)cb, 2));
                    close_sent_ = true;
                }
                st->close();
                bool clean = we_closed || code == 1000 || code == 1001 || code == 1005;
                finish(clean, code, reason);
                return;
            }
            case 0x9:
                send_frame(0xA, std::string_view(payload, plen));
                break;
            case 0xA:
                resolve_pings(std::string(payload, plen));
                break;
            default:
                rx.erase(0, off);
                fail(1002, "invalid opcode");
                return;
        }
        if (delivered >= 64 && rx.size() > off) {
            // websockets yields every 64 frames; let timers and other sockets run.
            rx.erase(0, off);
            std::weak_ptr<WsClient> w = weak_from_this();
            ev::Loop::get().call_soon([w] {
                if (auto c = w.lock()) c->handle_frames();
            });
            return;
        }
    }
    if (stream_ == st) rx.erase(0, std::min(off, rx.size()));
}

void WsClient::send_frame(int opcode, std::string_view payload) {
    if (!stream_) return;
    size_t n = payload.size();
    std::string frame;
    frame.resize(14 + n);
    unsigned char* f = (unsigned char*)frame.data();
    size_t h = 0;
    f[h++] = (unsigned char)(0x80 | opcode);
    if (n < 126) {
        f[h++] = (unsigned char)(0x80 | n);
    } else if (n < 65536) {
        f[h++] = 0x80 | 126;
        f[h++] = (unsigned char)(n >> 8);
        f[h++] = (unsigned char)(n & 0xff);
    } else {
        f[h++] = 0x80 | 127;
        for (int k = 7; k >= 0; --k) f[h++] = (unsigned char)((n >> (8 * k)) & 0xff);
    }
    uint32_t m = fast_rand();
    unsigned char mask[4] = {(unsigned char)m, (unsigned char)(m >> 8), (unsigned char)(m >> 16),
                             (unsigned char)(m >> 24)};
    std::memcpy(f + h, mask, 4);
    h += 4;
    for (size_t k = 0; k < n; ++k) f[h + k] = (unsigned char)payload[k] ^ mask[k & 3];
    frame.resize(h + n);
    stream_->send(frame);
}

bool WsClient::send_text(std::string_view s) {
    if (state_ != State::Open) return false;
    send_frame(0x1, s);
    return true;
}

void WsClient::ping(double timeout, std::function<void(bool, double)> cb) {
    std::weak_ptr<WsClient> w = weak_from_this();
    if (state_ != State::Open) {
        ev::Loop::get().call_soon([cb] { cb(false, 0); });
        return;
    }
    std::string payload;
    do {
        uint32_t r = fast_rand();
        payload.assign((const char*)&r, 4);
    } while (std::any_of(pings_.begin(), pings_.end(), [&](const PingWait& p) { return p.payload == payload; }));
    PingWait pw;
    pw.payload = payload;
    pw.sent_at = clk::perf_counter();
    pw.cb = std::move(cb);
    pw.timer = ev::Loop::get().call_later(timeout, [w, payload] {
        auto c = w.lock();
        if (!c) return;
        for (auto it = c->pings_.begin(); it != c->pings_.end(); ++it) {
            if (it->payload == payload) {
                auto f = std::move(it->cb);
                c->pings_.erase(it);
                if (f) f(false, 0);
                return;
            }
        }
    });
    pings_.push_back(std::move(pw));
    send_frame(0x9, payload);
}

void WsClient::resolve_pings(const std::string& payload) {
    // A pong acknowledges its own ping AND every earlier one (websockets semantics).
    auto it = std::find_if(pings_.begin(), pings_.end(), [&](const PingWait& p) { return p.payload == payload; });
    if (it == pings_.end()) return;
    double now = clk::perf_counter();
    std::vector<PingWait> done(std::make_move_iterator(pings_.begin()), std::make_move_iterator(it + 1));
    pings_.erase(pings_.begin(), it + 1);
    for (auto& p : done) {
        ev::Loop::get().cancel(p.timer);
        if (p.cb) p.cb(true, now - p.sent_at);
    }
}

void WsClient::schedule_keepalive(double delay) {
    std::weak_ptr<WsClient> w = weak_from_this();
    keepalive_timer_ = ev::Loop::get().call_later(delay, [w] {
        auto c = w.lock();
        if (!c || c->state_ != State::Open) return;
        c->ping(c->opt_.ping_timeout, [w](bool ok, double rtt) {
            auto c2 = w.lock();
            if (!c2 || c2->state_ != State::Open) return;
            if (!ok) {
                c2->fail(1011, "keepalive ping timeout");
                return;
            }
            c2->latency_ = rtt;
            c2->schedule_keepalive(std::max(0.0, c2->opt_.ping_interval - c2->latency_));
        });
    });
}

void WsClient::close(int code) {
    if (state_ == State::Open) {
        state_ = State::Closing;
        unsigned char cb[2] = {(unsigned char)(code >> 8), (unsigned char)(code & 0xff)};
        send_frame(0x8, std::string_view((const char*)cb, 2));
        close_sent_ = true;
        std::weak_ptr<WsClient> w = weak_from_this();
        close_timer_ = ev::Loop::get().call_later(opt_.close_timeout, [w, code] {
            if (auto c = w.lock()) {
                if (c->stream_) c->stream_->close();
                c->finish(true, code, "close timeout");
            }
        });
    } else if (state_ == State::Connecting || state_ == State::Idle) {
        abort("closed before open");
    }
}

void WsClient::abort(const std::string& why) {
    if (stream_) stream_->close();
    finish(false, 1006, why);
}

void WsClient::fail(int code, const std::string& why) {
    if (state_ == State::Open && !close_sent_) {
        std::string payload;
        payload.push_back((char)(code >> 8));
        payload.push_back((char)(code & 0xff));
        payload += why;
        send_frame(0x8, payload);
        close_sent_ = true;
    }
    if (stream_) stream_->close();
    finish(false, code, why);
}

void WsClient::finish(bool clean, int code, const std::string& why) {
    if (finished_) return;
    finished_ = true;
    auto self = shared_from_this();
    state_ = State::Closed;
    close_code_ = code;
    close_reason_ = why;
    ev::Loop& L = ev::Loop::get();
    L.cancel(keepalive_timer_);
    L.cancel(keepalive_wait_);
    L.cancel(close_timer_);
    L.cancel(open_timer_);
    if (stream_) stream_->close();
    std::deque<PingWait> pending;
    pending.swap(pings_);
    for (auto& p : pending) {
        L.cancel(p.timer);
        if (p.cb) p.cb(false, 0);
    }
    if (on_close) {
        auto f = on_close;
        f(clean, code, why);
    }
}

// ---------------------------------------------------------------------------
// HttpPool — aiohttp TCPConnector semantics: LIFO reuse of idle connections,
// at most `limit` connections (excess requests wait), idle ones expire after
// `keepalive` seconds, a total per-request timeout that covers waiting for a
// connection, connecting, sending and reading.
// ---------------------------------------------------------------------------

struct HttpPool::Pending {
    std::function<void(HttpResult)> cb;
    ev::Timer timer;
    bool done = false;
    std::weak_ptr<Conn> conn;
};

struct HttpPool::Conn {
    std::shared_ptr<Stream> s;
    bool busy = false;
    double idle_since = 0;
    std::shared_ptr<Pending> pending;
    // response parser
    bool head_done = false;
    int status = 0;
    bool chunked = false;
    long long clen = -1;
    bool keep = true;
    bool to_close = false;
    int chunk_state = 0;  // 0 size line, 1 data, 2 CRLF after data, 3 trailers
    size_t chunk_left = 0;
    std::string body;

    void reset() {
        head_done = false;
        status = 0;
        chunked = false;
        clen = -1;
        keep = true;
        to_close = false;
        chunk_state = 0;
        chunk_left = 0;
        body.clear();
    }
};

namespace {
void finish_req(const std::shared_ptr<HttpPool::Pending>& p, HttpResult res);
}

HttpPool::HttpPool(std::string host, int port, bool tls, Headers default_headers, size_t limit,
                   double keepalive_sec)
    : host_(std::move(host)), port_(port), tls_(tls), headers_(std::move(default_headers)),
      limit_(limit), keepalive_(keepalive_sec) {
    prefix_cache_ = "Host: " + host_ + ((port_ == (tls_ ? 443 : 80)) ? "" : ":" + std::to_string(port_)) + "\r\n";
    for (const auto& [k, v] : headers_) prefix_cache_ += k + ": " + v + "\r\n";
}

HttpPool::~HttpPool() { close_all(); }

std::string HttpPool::build(const Req& r) const {
    std::string out;
    out.reserve(r.method.size() + r.path.size() + prefix_cache_.size() + r.body.size() + 64);
    out += r.method;
    out.push_back(' ');
    out += r.path;
    out += " HTTP/1.1\r\n";
    out += prefix_cache_;
    for (const auto& [k, v] : r.extra) out += k + ": " + v + "\r\n";
    if (!r.body.empty() || r.method == "POST") {
        out += "Content-Length: ";
        out += std::to_string(r.body.size());
        out += "\r\n";
    }
    out += "\r\n";
    out += r.body;
    return out;
}

namespace {
void finish_req(const std::shared_ptr<HttpPool::Pending>& p, HttpResult res) {
    if (!p || p->done) return;
    p->done = true;
    ev::Loop::get().cancel(p->timer);
    auto cb = std::move(p->cb);
    p->cb = nullptr;
    if (cb) cb(std::move(res));
}
}  // namespace

void HttpPool::request(const std::string& method, const std::string& path, const std::string& body,
                       double timeout, std::function<void(HttpResult)> cb, RequestTiming* timing,
                       const Headers& extra) {
    auto p = std::make_shared<Pending>();
    p->cb = std::move(cb);
    if (closed_) {
        ev::Loop::get().call_soon(
            [p] { finish_req(p, HttpResult{HttpResult::Disconnected, 0, "", "pool closed"}); });
        return;
    }
    std::weak_ptr<Pending> wp = p;
    p->timer = ev::Loop::get().call_later(timeout, [this, wp] {
        auto p2 = wp.lock();
        if (!p2 || p2->done) return;
        p2->timer = ev::Timer{};
        auto c = p2->conn.lock();
        finish_req(p2, HttpResult{HttpResult::Timeout, 0, "", "timeout"});
        // The response may still arrive; it must never be read as the next one's.
        if (c) drop(c);
    });
    Req r;
    r.method = method;
    r.path = path;
    r.body = body;
    r.p = p;
    r.timing = timing;
    r.extra = extra;
    dispatch(std::move(r));
}

void HttpPool::dispatch(Req r) {
    if (r.p->done) return;
    double now = ev::Loop::monotonic();
    while (!idle_.empty()) {
        auto c = idle_.back();  // most recently released first
        idle_.pop_back();
        if (!c->s->is_open() || now - c->idle_since > keepalive_) {
            drop(c);
            continue;
        }
        start_on(c, std::move(r), true);
        return;
    }
    if (conns_.size() >= limit_) {
        if (r.timing && r.timing->pool_queued_at < 0) r.timing->pool_queued_at = clk::perf_counter();
        waiting_.push_back(std::move(r));
        return;
    }
    auto c = std::make_shared<Conn>();
    c->s = std::make_shared<Stream>(host_, port_, tls_);
    {
        std::string err;
        Addrs addrs = dns_.next_addrs(host_, port_, err);
        if (addrs.empty()) {
            auto p = r.p;
            ev::Loop::get().call_soon([p, err] {
                finish_req(p, HttpResult{HttpResult::ConnectError, 0, "", "dns: " + err});
            });
            return;
        }
        c->s->set_addresses(std::move(addrs));
    }
    c->busy = true;
    conns_.push_back(c);
    r.p->conn = c;
    std::weak_ptr<Conn> wc = c;
    c->s->on_data = [this, wc] {
        if (auto cc = wc.lock()) on_conn_data(cc);
    };
    c->s->on_close = [this, wc](const std::string& why) {
        auto cc = wc.lock();
        if (!cc) return;
        if (cc->busy && cc->pending && !cc->pending->done) {
            if (cc->to_close && cc->head_done) complete(cc, HttpResult{HttpResult::Ok, cc->status, cc->body, ""}, false);
            else complete(cc, HttpResult{HttpResult::Disconnected, 0, "", why}, false);
        } else {
            drop(cc);
        }
    };
    auto holder = std::make_shared<Req>(std::move(r));
    c->s->connect(30.0, [this, wc, holder](bool ok, const std::string& err) {
        auto cc = wc.lock();
        if (!cc) return;  // dropped (e.g. the request timed out while connecting)
        if (!ok) {
            auto p = holder->p;
            drop(cc);
            finish_req(p, HttpResult{HttpResult::ConnectError, 0, "", err});
            return;
        }
        if (holder->p->done) {
            cc->busy = false;
            release(cc);
            return;
        }
        start_on(cc, std::move(*holder), false);
    });
}

void HttpPool::start_on(const std::shared_ptr<Conn>& c, Req r, bool reused) {
    c->busy = true;
    c->pending = r.p;
    c->reset();
    r.p->conn = c;
    if (r.timing) {
        r.timing->reused = reused ? 1 : 0;
        if (r.timing->pool_queued_at >= 0) {
            r.timing->pool_wait_ms += (clk::perf_counter() - r.timing->pool_queued_at) * 1000.0;
            r.timing->pool_queued_at = -1;
        }
    }
    std::string wire = build(r);
    if (r.timing) r.timing->headers_at = clk::perf_counter();
    if (!c->s->send(wire)) complete(c, HttpResult{HttpResult::Disconnected, 0, "", "send failed"}, false);
}

void HttpPool::on_conn_data(const std::shared_ptr<Conn>& c) {
    if (!c->busy || !c->pending) {
        drop(c);  // unsolicited bytes on an idle connection
        return;
    }
    std::string& rx = c->s->rx;
    while (!c->head_done) {
        size_t pos = rx.find("\r\n\r\n");
        if (pos == std::string::npos) {
            if (rx.size() > (1u << 20))
                complete(c, HttpResult{HttpResult::Protocol, 0, "", "header too large"}, false);
            return;
        }
        std::string head = rx.substr(0, pos);
        rx.erase(0, pos + 4);
        if (head.size() < 12 || head.compare(0, 5, "HTTP/") != 0) {
            complete(c, HttpResult{HttpResult::Protocol, 0, "", "bad status line"}, false);
            return;
        }
        bool http10 = head.compare(0, 8, "HTTP/1.0") == 0;
        c->status = std::atoi(head.c_str() + 9);
        if (c->status >= 100 && c->status < 200) continue;  // interim response
        c->head_done = true;
        bool conn_close = false, conn_keep = false;
        size_t lp = head.find("\r\n");
        while (lp != std::string::npos) {
            size_t next = head.find("\r\n", lp + 2);
            std::string line = head.substr(lp + 2, next == std::string::npos ? std::string::npos : next - lp - 2);
            lp = next;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = lower(line.substr(0, colon));
            size_t vb = colon + 1;
            while (vb < line.size() && (line[vb] == ' ' || line[vb] == '\t')) ++vb;
            std::string v = lower(line.substr(vb));
            if (k == "content-length") c->clen = std::atoll(v.c_str());
            else if (k == "transfer-encoding" && v.find("chunked") != std::string::npos) c->chunked = true;
            else if (k == "connection") {
                if (v.find("close") != std::string::npos) conn_close = true;
                if (v.find("keep-alive") != std::string::npos) conn_keep = true;
            }
        }
        c->keep = http10 ? conn_keep : !conn_close;
        if (c->chunked) c->clen = -1;
        if (!c->chunked && c->clen < 0) {
            if (c->status == 204 || c->status == 304) c->clen = 0;
            else {
                c->to_close = true;
                c->keep = false;
            }
        }
    }
    if (c->chunked) {
        while (true) {
            if (c->chunk_state == 0) {
                size_t pos = rx.find("\r\n");
                if (pos == std::string::npos) return;
                size_t n = std::strtoul(rx.substr(0, pos).c_str(), nullptr, 16);
                rx.erase(0, pos + 2);
                if (n == 0) c->chunk_state = 3;
                else {
                    c->chunk_left = n;
                    c->chunk_state = 1;
                }
            } else if (c->chunk_state == 1) {
                size_t take = std::min(rx.size(), c->chunk_left);
                c->body.append(rx, 0, take);
                rx.erase(0, take);
                c->chunk_left -= take;
                if (c->chunk_left) return;
                c->chunk_state = 2;
            } else if (c->chunk_state == 2) {
                if (rx.size() < 2) return;
                rx.erase(0, 2);
                c->chunk_state = 0;
            } else {
                size_t pos = rx.find("\r\n");
                if (pos == std::string::npos) return;
                bool last = pos == 0;
                rx.erase(0, pos + 2);
                if (last) {
                    complete(c, HttpResult{HttpResult::Ok, c->status, std::move(c->body), ""}, c->keep);
                    return;
                }
            }
        }
    }
    if (c->clen >= 0) {
        size_t need = (size_t)c->clen - c->body.size();
        size_t take = std::min(rx.size(), need);
        c->body.append(rx, 0, take);
        rx.erase(0, take);
        if (c->body.size() == (size_t)c->clen)
            complete(c, HttpResult{HttpResult::Ok, c->status, std::move(c->body), ""}, c->keep);
        return;
    }
    c->body += rx;  // read-to-close body; completes in on_close
    rx.clear();
}

void HttpPool::complete(const std::shared_ptr<Conn>& c, HttpResult res, bool keep) {
    auto p = c->pending;
    c->pending.reset();
    c->busy = false;
    if (keep && c->s->is_open() && c->s->rx.empty()) release(c);
    else drop(c);
    finish_req(p, std::move(res));
}

void HttpPool::release(const std::shared_ptr<Conn>& c) {
    c->idle_since = ev::Loop::monotonic();
    while (!waiting_.empty()) {
        Req r = std::move(waiting_.front());
        waiting_.pop_front();
        if (r.p->done) continue;
        start_on(c, std::move(r), true);
        return;
    }
    idle_.push_back(c);
}

void HttpPool::drop(const std::shared_ptr<Conn>& c) {
    c->s->on_data = nullptr;
    c->s->on_close = nullptr;
    c->s->close();
    c->busy = false;
    c->pending.reset();
    conns_.erase(std::remove(conns_.begin(), conns_.end(), c), conns_.end());
    idle_.erase(std::remove(idle_.begin(), idle_.end(), c), idle_.end());
    // Capacity freed: start a waiting request on a new connection.
    while (!waiting_.empty() && conns_.size() < limit_ && !closed_) {
        Req r = std::move(waiting_.front());
        waiting_.pop_front();
        if (r.p->done) continue;
        dispatch(std::move(r));
        break;
    }
}

void HttpPool::close_all() {
    if (closed_) return;
    closed_ = true;
    std::vector<std::shared_ptr<Pending>> open;
    for (auto& c : conns_) {
        c->s->on_data = nullptr;
        c->s->on_close = nullptr;
        c->s->close();
        if (c->pending) open.push_back(c->pending);
    }
    conns_.clear();
    idle_.clear();
    for (auto& r : waiting_) open.push_back(r.p);
    waiting_.clear();
    for (auto& p : open) finish_req(p, HttpResult{HttpResult::Disconnected, 0, "", "pool closed"});
}

}  // namespace net
