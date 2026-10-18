// SPDX-License-Identifier: MPL-2.0
#include "bounded_queue.h"
#include "read_snapshot.h"
#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
using namespace skyline::diagnostics;
struct Value { uint64_t id{}; std::string text; };
int main() {
    BoundedQueue<Value> queue;
    Value value; uint64_t sequence{};
    assert(queue.Empty() && !queue.Pop(value, sequence));
    queue.Reset(64);
    for (uint64_t i{}; i < 56; ++i) assert(queue.Push({i, std::to_string(i)}, 56) == Admission::Accepted);
    assert(queue.Push({56, "ordinary"}, 56) == Admission::Full);
    for (uint64_t i = 56; i < 64; ++i) assert(queue.Push({i, std::to_string(i)}, 64) == Admission::Accepted);
    auto start = std::chrono::steady_clock::now();
    for (int i{}; i < 100000; ++i) assert(queue.Push({999, "full"}, 64) == Admission::Full);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    for (uint64_t i{}; i < 64; ++i) {
        assert(queue.Pop(value, sequence)); assert(sequence == i + 1 && value.id == i && value.text == std::to_string(i));
    }
    assert(queue.Empty() && !queue.Pop(value, sequence));
    // Several writers wrap the production ring many times while its consumer
    // verifies reservation order, owned strings and exactly-once delivery.
    queue.Reset(256);
    constexpr size_t writers = 4, perWriter = 5000;
    std::vector<std::thread> producers;
    for (size_t writer{}; writer < writers; ++writer) producers.emplace_back([&, writer] {
        for (size_t i{}; i < perWriter; ++i) {
            Value item{writer * perWriter + i, std::to_string(writer * perWriter + i)};
            while (queue.Push(std::move(item), 256) != Admission::Accepted) std::this_thread::yield();
        }
    });
    std::vector<bool> seen(writers * perWriter);
    for (size_t i{}; i < seen.size();) {
        if (!queue.Pop(value, sequence)) { std::this_thread::yield(); continue; }
        assert(sequence == i + 1 && value.id < seen.size() && !seen[value.id]);
        assert(value.text == std::to_string(value.id)); seen[value.id] = true; ++i;
    }
    for (auto &thread : producers) thread.join();
    assert(queue.Empty());
    ReadSnapshot<Value> snapshot;
    std::atomic<bool> done{};
    std::thread setter([&] {
        for (uint64_t i = 1; i < 10000; ++i) snapshot.Publish({i, std::to_string(i)});
        done = true;
    });
    producers.clear();
    for (size_t i{}; i < writers; ++i) producers.emplace_back([&] {
        Value item;
        do { if (snapshot.Read(item) && item.id) assert(item.text == std::to_string(item.id)); } while (!done);
    });
    setter.join(); for (auto &thread : producers) thread.join();
    assert(snapshot.Read(value) && value.id == 9999 && value.text == "9999");
}
