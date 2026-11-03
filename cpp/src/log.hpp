// log.hpp — bot_core._AsyncLog: a bounded queue (2048) drained in batches of up
// to 64 by a writer thread, so stdout writes never block the event loop.
// out() is the synchronous print() used for startup and dry-run output.
#pragma once

#include <string>

namespace logx {

void start();                 // start the writer thread (idempotent)
void log(std::string line);   // async; drops (and later reports) when 2048 are pending
void out(const std::string& line);  // synchronous print()
void close();                 // drain and stop the writer
void set_quiet(bool q);       // replay harness: drop out() too

}  // namespace logx
