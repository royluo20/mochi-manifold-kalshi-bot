// loop.hpp — a single-threaded event loop (the asyncio loop of the port).
// fd readiness via poll(), timers, call_soon. Everything the engine touches
// runs on this one thread, so the decision path needs no locks — the same
// "await-free decision block" guarantee the Python engine relies on.
#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace ev {

using Fn = std::function<void()>;
// readable, writable, error/hangup
using IoFn = std::function<void(bool, bool, bool)>;

struct Timer {
    uint64_t id = 0;
    explicit operator bool() const { return id != 0; }
};

class Loop {
public:
    static Loop& get();

    void watch(int fd, bool want_read, bool want_write, IoFn cb);
    void set_interest(int fd, bool want_read, bool want_write);
    void unwatch(int fd);

    Timer call_later(double delay_sec, Fn fn);
    void cancel(Timer& t);
    void call_soon(Fn fn);
    // Run queued call_soon callbacks (and any they queue) without polling.
    void drain_soon();

    void run();  // until stop()
    // Run until done() is true or timeout (seconds, <0 = none). Returns done().
    bool run_until(const std::function<bool()>& done, double timeout_sec = -1);
    void stop() { stopping_ = true; }
    void resume() { stopping_ = false; }  // e.g. to drain orders after a stop
    bool stopping() const { return stopping_; }

    // Stop the loop on SIGINT / SIGTERM (like KeyboardInterrupt in asyncio.run).
    void install_signal_stop();
    bool interrupted() const { return interrupted_; }

    static double monotonic();
    // "kqueue", "epoll" or "poll()": the readiness backend compiled in.
    static const char* backend_name();

private:
    struct Watch {
        bool r = false, w = false;
        IoFn cb;
    };
    struct Due {
        double at;
        uint64_t seq;
        uint64_t id;
        bool operator>(const Due& o) const { return at != o.at ? at > o.at : seq > o.seq; }
    };

    void iterate(int max_wait_ms);
    // Readiness backend: kqueue (macOS/BSD) or epoll (Linux), O(events) per
    // wakeup like uvloop; poll() only where neither exists. poll() rescans
    // every watched socket on every wakeup, which a 100-connection order pool
    // turns into real CPU at thousands of feed frames per second.
    void backend_init();
    void backend_set(int fd, bool old_r, bool old_w, bool r, bool w);
    int backend_wait(int timeout_ms);  // fills ready_
    struct Ready {
        int fd;
        bool r, w, err;
    };
    std::vector<Ready> ready_;
    int bfd_ = -1;  // kqueue / epoll descriptor

    std::unordered_map<int, Watch> watches_;
    std::priority_queue<Due, std::vector<Due>, std::greater<Due>> timers_;
    std::unordered_map<uint64_t, Fn> timer_fns_;
    std::vector<Fn> soon_;
    uint64_t next_id_ = 1;
    uint64_t seq_ = 0;
    bool stopping_ = false;
    bool interrupted_ = false;
    int sig_pipe_[2] = {-1, -1};
};

}  // namespace ev
