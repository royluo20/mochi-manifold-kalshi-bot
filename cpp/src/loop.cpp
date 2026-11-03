#include "loop.hpp"

#include <utility>
#include <fcntl.h>
#include <poll.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define EV_KQUEUE 1
#elif defined(__linux__)
#include <sys/epoll.h>
#define EV_EPOLL 1
#endif
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <ctime>

namespace ev {

namespace {
int g_sig_write_fd = -1;
void on_signal(int) {
    if (g_sig_write_fd >= 0) {
        char c = 1;
        ssize_t r = write(g_sig_write_fd, &c, 1);
        (void)r;
    }
}
}  // namespace

Loop& Loop::get() {
    static Loop loop;
    return loop;
}

double Loop::monotonic() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char* Loop::backend_name() {
#if defined(EV_KQUEUE)
    return "kqueue";
#elif defined(EV_EPOLL)
    return "epoll";
#else
    return "poll()";
#endif
}

void Loop::backend_init() {
    if (bfd_ >= 0) return;
#if defined(EV_KQUEUE)
    bfd_ = kqueue();
#elif defined(EV_EPOLL)
    bfd_ = epoll_create1(EPOLL_CLOEXEC);
#endif
}

void Loop::backend_set(int fd, bool old_r, bool old_w, bool r, bool w) {
    backend_init();
    if (bfd_ < 0) return;  // poll() fallback reads watches_ directly
#if defined(EV_KQUEUE)
    struct kevent ch[2];
    int n = 0;
    if (r != old_r) EV_SET(&ch[n++], fd, EVFILT_READ, r ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    if (w != old_w) EV_SET(&ch[n++], fd, EVFILT_WRITE, w ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    if (n) kevent(bfd_, ch, n, nullptr, 0, nullptr);
#elif defined(EV_EPOLL)
    epoll_event ev{};
    ev.events = (r ? EPOLLIN : 0) | (w ? EPOLLOUT : 0);
    ev.data.fd = fd;
    bool was = old_r || old_w, now = r || w;
    if (!was && now) epoll_ctl(bfd_, EPOLL_CTL_ADD, fd, &ev);
    else if (was && !now) epoll_ctl(bfd_, EPOLL_CTL_DEL, fd, &ev);
    else if (was && now) epoll_ctl(bfd_, EPOLL_CTL_MOD, fd, &ev);
#else
    (void)fd; (void)old_r; (void)old_w; (void)r; (void)w;
#endif
}

int Loop::backend_wait(int timeout_ms) {
    ready_.clear();
    backend_init();
#if defined(EV_KQUEUE)
    if (bfd_ >= 0) {
        struct kevent evs[256];
        struct timespec ts, *tsp = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
            tsp = &ts;
        }
        int n = kevent(bfd_, nullptr, 0, evs, 256, tsp);
        for (int i = 0; i < n; ++i) {
            int fd = (int)evs[i].ident;
            bool r = evs[i].filter == EVFILT_READ, w = evs[i].filter == EVFILT_WRITE;
            bool err = (evs[i].flags & (EV_EOF | EV_ERROR)) != 0;
            bool merged = false;  // read + write for one fd arrive as two events
            for (auto& x : ready_)
                if (x.fd == fd) {
                    x.r |= r;
                    x.w |= w;
                    x.err |= err;
                    merged = true;
                }
            if (!merged) ready_.push_back(Ready{fd, r, w, err});
        }
        return n;
    }
#elif defined(EV_EPOLL)
    if (bfd_ >= 0) {
        epoll_event evs[256];
        int n = epoll_wait(bfd_, evs, 256, timeout_ms);
        for (int i = 0; i < n; ++i)
            ready_.push_back(Ready{evs[i].data.fd, (evs[i].events & EPOLLIN) != 0, (evs[i].events & EPOLLOUT) != 0,
                                   (evs[i].events & (EPOLLERR | EPOLLHUP)) != 0});
        return n;
    }
#endif
    std::vector<pollfd> fds;
    fds.reserve(watches_.size());
    for (auto& [fd, w] : watches_) {
        short ev = 0;
        if (w.r) ev |= POLLIN;
        if (w.w) ev |= POLLOUT;
        fds.push_back(pollfd{fd, ev, 0});
    }
    int n = ::poll(fds.data(), (nfds_t)fds.size(), timeout_ms);
    for (const pollfd& p : fds)
        if (p.revents)
            ready_.push_back(Ready{p.fd, (p.revents & POLLIN) != 0, (p.revents & POLLOUT) != 0,
                                   (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0});
    return n;
}

void Loop::watch(int fd, bool want_read, bool want_write, IoFn cb) {
    auto it = watches_.find(fd);
    bool old_r = false, old_w = false;
    if (it != watches_.end()) {
        old_r = it->second.r;
        old_w = it->second.w;
    }
    Watch& w = watches_[fd];
    w.r = want_read;
    w.w = want_write;
    w.cb = std::move(cb);
    backend_set(fd, old_r, old_w, want_read, want_write);
}

void Loop::set_interest(int fd, bool want_read, bool want_write) {
    auto it = watches_.find(fd);
    if (it == watches_.end()) return;
    if (it->second.r == want_read && it->second.w == want_write) return;
    backend_set(fd, it->second.r, it->second.w, want_read, want_write);
    it->second.r = want_read;
    it->second.w = want_write;
}

void Loop::unwatch(int fd) {
    auto it = watches_.find(fd);
    if (it == watches_.end()) return;
    backend_set(fd, it->second.r, it->second.w, false, false);
    watches_.erase(it);
}

Timer Loop::call_later(double delay_sec, Fn fn) {
    uint64_t id = next_id_++;
    timer_fns_.emplace(id, std::move(fn));
    timers_.push(Due{monotonic() + (delay_sec > 0 ? delay_sec : 0), seq_++, id});
    return Timer{id};
}

void Loop::cancel(Timer& t) {
    if (t.id) timer_fns_.erase(t.id);
    t.id = 0;
}

void Loop::call_soon(Fn fn) { soon_.push_back(std::move(fn)); }

void Loop::drain_soon() {
    for (int rounds = 0; !soon_.empty() && rounds < 1000; ++rounds) {
        std::vector<Fn> now;
        now.swap(soon_);
        for (Fn& f : now) f();
    }
}

void Loop::install_signal_stop() {
    if (sig_pipe_[0] >= 0) return;
    if (pipe(sig_pipe_) != 0) return;
    fcntl(sig_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(sig_pipe_[1], F_SETFL, O_NONBLOCK);
    g_sig_write_fd = sig_pipe_[1];
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    int rfd = sig_pipe_[0];
    watch(rfd, true, false, [this, rfd](bool, bool, bool) {
        char buf[16];
        while (read(rfd, buf, sizeof buf) > 0) {
        }
        interrupted_ = true;
        stopping_ = true;
    });
}

void Loop::iterate(int max_wait_ms) {
    // 1. callbacks queued before this iteration
    if (!soon_.empty()) {
        std::vector<Fn> now;
        now.swap(soon_);
        for (Fn& f : now) f();
    }
    // 2. wait for IO or the next timer
    int timeout_ms = max_wait_ms;
    if (!soon_.empty() || stopping_) {
        timeout_ms = 0;
    } else {
        while (!timers_.empty() && !timer_fns_.count(timers_.top().id)) timers_.pop();
        if (!timers_.empty()) {
            double wait = timers_.top().at - monotonic();
            int ms = wait <= 0 ? 0 : (int)std::ceil(wait * 1000.0);
            if (timeout_ms < 0 || ms < timeout_ms) timeout_ms = ms;
        }
    }
    if (backend_wait(timeout_ms) > 0) {
        std::vector<Ready> ready;
        ready.swap(ready_);
        for (const Ready& p : ready) {
            auto it = watches_.find(p.fd);
            if (it == watches_.end()) continue;  // removed by an earlier callback
            bool r = p.r && it->second.r, w = p.w && it->second.w;
            if (!r && !w && !p.err) continue;     // interest changed meanwhile
            IoFn cb = it->second.cb;  // copy: the callback may unwatch itself
            cb(r, w, p.err);
        }
    }
    // 3. due timers
    double now = monotonic();
    while (!timers_.empty() && timers_.top().at <= now) {
        Due d = timers_.top();
        timers_.pop();
        auto it = timer_fns_.find(d.id);
        if (it == timer_fns_.end()) continue;
        Fn f = std::move(it->second);
        timer_fns_.erase(it);
        f();
    }
}

void Loop::run() {
    while (!stopping_) iterate(1000);
}

bool Loop::run_until(const std::function<bool()>& done, double timeout_sec) {
    double deadline = timeout_sec >= 0 ? monotonic() + timeout_sec : -1;
    while (!done() && !stopping_) {
        if (deadline >= 0 && monotonic() >= deadline) break;
        iterate(50);
    }
    return done();
}

}  // namespace ev
