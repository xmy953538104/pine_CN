// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include "test_hooks.h"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
using namespace skyline::diagnostics;
namespace {
std::mutex mutex;
std::condition_variable condition;
bool publicationPaused{}, releasePublication{}, writerPaused{}, releaseWriter{}, registrationPaused{}, releaseRegistration{};
thread_local bool pauseRegistration{};
void Hook(testing::Point point, std::string_view event) {
    std::unique_lock lock(mutex);
    if (point == testing::Point::ProducerStateRead && pauseRegistration) {
        registrationPaused = true; condition.notify_all(); condition.wait(lock, [] { return releaseRegistration; });
    } else if (point == testing::Point::PublishedBeforeCount && event == "A") {
        publicationPaused = true; condition.notify_all(); condition.wait(lock, [] { return releasePublication; });
    } else if (point == testing::Point::BeforeRecordWrite && event == "B") {
        writerPaused = true; condition.notify_all(); condition.wait(lock, [] { return releaseWriter; });
    }
}
template<class Predicate> void Await(Predicate predicate) {
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(5), predicate));
}
}
int main(int argc, char **argv) {
    assert(argc == 2);
    Config config{argv[1], "ticket-race", 1024, 65536, 3, false, {}};
    assert(Start(config)); testing::hook = Hook;
    std::thread a([] { Emit("test", "A"); });
    Await([] { return publicationPaused; });
    Emit("test", "B"); // B has returned although A's accepted count is paused.
    Await([] { return writerPaused; }); // A is on disk; B is not.
    assert(Stats().accepted == 1 && Stats().written == 1);
    assert(!Flush(20));
    { std::lock_guard lock(mutex); releasePublication = true; condition.notify_all(); }
    a.join(); assert(!Flush(20));
    { std::lock_guard lock(mutex); releaseWriter = true; condition.notify_all(); }
    assert(Flush(2000)); Stop();
    assert(Stats().accepted == 2 && Stats().written == 2);

    config.sessionId = "old-registration"; assert(Start(config));
    std::thread old([] { pauseRegistration = true; Emit("test", "OLD_SESSION"); });
    Await([] { return registrationPaused; });
    Stop(); // The paused caller owns no registration and cannot mutate counters.
    const auto stopped = Stats();
    config.sessionId = "new-registration"; assert(Start(config));
    { std::lock_guard lock(mutex); releaseRegistration = true; condition.notify_all(); }
    old.join();
    assert(stopped.accepted == 0 && stopped.dropped == 0);
    assert(Stats().accepted == 0 && Stats().dropped == 0);
    testing::hook = nullptr;
    Emit("test", "NEW_SESSION"); assert(Flush(2000)); Stop();
    assert(Stats().accepted == 1 && Stats().written == 1 && Stats().dropped == 0);
}
