#include "log.hpp"

#include <utility>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace logx {
namespace {

std::mutex g_mu;          // guards the queue
std::mutex g_out_mu;      // serializes writes to stdout
std::condition_variable g_cv;
std::deque<std::string> g_queue;
size_t g_dropped = 0;
bool g_stop = false;
bool g_started = false;
bool g_quiet = false;
std::thread g_thread;

constexpr size_t kMax = 2048;
constexpr size_t kBatch = 64;

void run() {
    std::vector<std::string> batch;
    while (true) {
        size_t dropped = 0;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_stop || !g_queue.empty(); });
            if (g_queue.empty() && g_stop) return;
            while (!g_queue.empty() && batch.size() < kBatch) {
                batch.push_back(std::move(g_queue.front()));
                g_queue.pop_front();
            }
            dropped = g_dropped;
            g_dropped = 0;
        }
        {
            std::lock_guard<std::mutex> lk(g_out_mu);
            for (const std::string& s : batch) {
                std::fwrite(s.data(), 1, s.size(), stdout);
                std::fputc('\n', stdout);
            }
            if (dropped) std::fprintf(stdout, "  ⚠ Dropped %zu log messages (slow output)\n", dropped);
            std::fflush(stdout);
        }
        batch.clear();
    }
}

}  // namespace

void start() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_started) return;
    g_started = true;
    g_stop = false;
    g_thread = std::thread(run);
}

void log(std::string line) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_started || g_stop) return;
        if (g_queue.size() >= kMax) {
            ++g_dropped;
            return;
        }
        g_queue.push_back(std::move(line));
    }
    g_cv.notify_one();
}

void set_quiet(bool q) { g_quiet = q; }

void out(const std::string& line) {
    if (g_quiet) return;
    std::lock_guard<std::mutex> lk(g_out_mu);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void close() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_started) return;
        g_stop = true;
    }
    g_cv.notify_one();
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lk(g_mu);
    g_started = false;
}

}  // namespace logx
