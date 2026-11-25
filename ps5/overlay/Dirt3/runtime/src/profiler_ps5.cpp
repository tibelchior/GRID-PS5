// In-process sampling profiler for the PS5 title, where nothing can attach.
//
// A thread signals each guest thread in turn about 50 times a second; the handler notes
// where the thread is: its program counter when that is in the title's code, else (the
// thread is inside a system module: waiting, copying, the kernel) the first return address
// on its stack that is. Every 20 s the log gets, per guest thread, its hottest 64-byte
// blocks of code as a share of that thread's samples ("prof:" lines; "wait" marks a sample
// taken outside the title's code and names the caller).
//
// Each report starts with a "cpu:" line: how many processor cores' worth of time the whole
// process used, how much of that was the guest threads (and which), and the rest: the
// threads this profiler does not see (DXVK, the Vulkan driver, sound).
//
// A frame that takes over 200 ms (profile_frame is called at every present) gets a "stall:"
// report: where every guest thread was, with the return addresses on its stack.
//
// Addresses are as the console loaded the title; the anchor line gives one known function's
// address to convert them (llvm-symbolizer on the linked ELF).
#include "recomp/host.h"

#if defined(__PROSPERO__)
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" const char __eh_frame_hdr_start[];

namespace rt {

namespace {

constexpr int kStack = 8;  // return addresses kept per sample

struct Slot {
    pthread_t thread{};
    uint32_t id = 0, entry = 0;
    bool used = false;
    char name[16] = {};  // set for the console's own threads (DXVK's), empty for guest threads
    uintptr_t stack_hi = 0;  // the stack above this address may not be mapped: never read it
};
Slot g_slots[128];
std::mutex g_mu;  // the slots; held while a thread is signalled, so it cannot unregister meanwhile
std::atomic<bool> g_running{false};
std::atomic<int64_t> g_last_frame_ms{0};

// One sample in flight at a time: written by the signalled thread's handler.
struct Sample {
    std::atomic<int> done{0};
    uintptr_t pc = 0;
    uintptr_t stack[kStack] = {};
    int depth = 0;
    int64_t cpu_ns = 0;  // the thread's processor time so far
    uintptr_t hi = 0;    // set before the signal: the limit for reading the stack
};
Sample g_sample;
uintptr_t g_lo = 0, g_hi = 0;  // the title's code: from its load address to the unwind tables

int64_t clock_ns(clockid_t id) {
    timespec t;
    if (clock_gettime(id, &t) != 0) return 0;
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

int64_t now_ms() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

bool in_title(uintptr_t a) { return a >= g_lo && a < g_hi; }

void on_signal(int, siginfo_t*, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
    g_sample.pc = uc->uc_mcontext.mc_rip;
    const auto* sp = reinterpret_cast<const uintptr_t*>(uc->uc_mcontext.mc_rsp & ~uintptr_t(7));
    int depth = 0;
    // A thread that has only just started has little stack above it: stop at the limit the
    // thread gave when it registered (0: no scan).
    const uintptr_t hi = g_sample.hi;
    for (int i = 0; i < 160 && depth < kStack; ++i) {
        if (reinterpret_cast<uintptr_t>(sp + i + 1) > hi) break;
        if (in_title(sp[i])) g_sample.stack[depth++] = sp[i];
    }
    g_sample.depth = depth;
    g_sample.cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    g_sample.done.store(1, std::memory_order_release);
}

// Signals the thread and waits for its handler; false when it did not answer in 5 ms.
bool take(const Slot& slot) {
    g_sample.hi = slot.stack_hi;
    g_sample.done.store(0, std::memory_order_release);
    if (pthread_kill(slot.thread, SIGUSR2) != 0) return false;
    for (int i = 0; i < 100; ++i) {
        if (g_sample.done.load(std::memory_order_acquire)) return true;
        usleep(50);
    }
    return false;
}

void* run(void*) {
    struct Key {
        uint32_t id;
        uintptr_t block;
        bool waiting;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        size_t operator()(const Key& k) const { return k.block * 31 + k.id * 2 + k.waiting; }
    };
    std::unordered_map<Key, uint32_t, Hash> counts;
    std::unordered_map<uint32_t, uint32_t> totals, entries;
    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, int64_t> cpu_first, cpu_last;  // per guest thread, this period
    int64_t process_cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    unsigned rounds = 0;
    int64_t last_report = now_ms();
    bool in_stall = false;
    unsigned stalls = 0, unanswered = 0;
    for (;;) {
        usleep(20000);
        const int64_t now = now_ms();
        const int64_t frame = g_last_frame_ms.load();
        const bool stall = frame && now - frame > 200;
        const bool report_stall = stall && !in_stall && stalls < 40;
        in_stall = stall;
        if (report_stall) log("stall %u: no frame for %lld ms", ++stalls, static_cast<long long>(now - frame));
        {
            std::lock_guard lk(g_mu);
            for (const Slot& slot : g_slots) {
                if (!slot.used) continue;
                if (!take(slot)) {
                    ++unanswered;
                    continue;
                }
                const bool waiting = !in_title(g_sample.pc);
                const uintptr_t at = waiting ? (g_sample.depth ? g_sample.stack[0] : 0) : g_sample.pc;
                ++counts[Key{slot.id, at & ~uintptr_t(63), waiting}];
                ++totals[slot.id];
                cpu_first.try_emplace(slot.id, g_sample.cpu_ns);
                cpu_last[slot.id] = g_sample.cpu_ns;
                entries[slot.id] = slot.entry;
                if (slot.name[0]) names[slot.id] = slot.name;
                if (report_stall) {
                    char line[256];
                    int n = std::snprintf(line, sizeof line, "stall: tid %u (entry %08X %s) at %p:", slot.id, slot.entry,
                                          slot.name, reinterpret_cast<void*>(g_sample.pc));
                    for (int i = 0; i < g_sample.depth && n < int(sizeof line) - 20; ++i)
                        n += std::snprintf(line + n, sizeof line - n, " %p", reinterpret_cast<void*>(g_sample.stack[i]));
                    log("%s", line);
                }
            }
        }
        ++rounds;
        if (now - last_report < 20000) continue;
        {
            const double wall = (now - last_report) * 1e6;
            const int64_t process_now = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
            std::vector<std::pair<int64_t, uint32_t>> used;
            int64_t guest = 0;
            for (const auto& [id, first] : cpu_first) {
                const int64_t ns = cpu_last[id] - first;
                guest += ns;
                used.push_back({ns, id});
            }
            std::sort(used.rbegin(), used.rend());
            char line[512];
            int n = std::snprintf(line, sizeof line, "cpu: %u rounds; process %.2f cores, guest threads %.2f, other threads %.2f; busiest:",
                                  rounds, (process_now - process_cpu) / wall, guest / wall,
                                  (process_now - process_cpu - guest) / wall);
            for (size_t i = 0; i < used.size() && i < 12 && n < int(sizeof line) - 24; ++i)
                n += std::snprintf(line + n, sizeof line - n, " tid %u(%s) %.0f%%", used[i].second,
                                   names.count(used[i].second) ? names[used[i].second].c_str() : "game", 100.0 * used[i].first / wall);
            log("%s", line);
            process_cpu = process_now;
            cpu_first.clear();
            cpu_last.clear();
            rounds = 0;
        }
        last_report = now;
        // Threads that were anywhere but one waiting place, busiest first; the rest are idle.
        std::vector<std::pair<Key, uint32_t>> all(counts.begin(), counts.end());
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second > b.second; });
        log("prof: 20 s, %zu threads, %u unanswered signals", totals.size(), unanswered);
        for (const auto& [id, total] : totals) {
            uint32_t top_wait = 0;
            for (const auto& [key, count] : all)
                if (key.id == id && key.waiting) top_wait = std::max(top_wait, count);
            if (top_wait * 100 >= total * 97) continue;  // idle: one waiting place nearly all the time
            int shown = 0;
            for (const auto& [key, count] : all) {
                if (key.id != id) continue;
                if (shown++ >= 14 || count * 200 < total) break;
                log("prof: tid %u (entry %08X %s) %5.1f%% %s %p", id, entries[id], names.count(id) ? names[id].c_str() : "",
                    100.0 * count / total, key.waiting ? "wait" : "code", reinterpret_cast<void*>(key.block));
            }
        }
        counts.clear();
        totals.clear();
        unanswered = 0;
    }
    return nullptr;
}

}  // namespace

void profile_register_thread(uint32_t entry, uint32_t guest_id) {
    std::lock_guard lk(g_mu);
    for (Slot& slot : g_slots)
        if (!slot.used) {
            slot = Slot{pthread_self(), guest_id, entry, true, {}, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) + 16};
            return;
        }
}

// Called by the console's DXVK build when one of its threads names itself (util_env.cpp).
extern "C" void dirt3_ps5_profile_thread(const char* name) {
    static std::atomic<uint32_t> next{1000};
    std::lock_guard lk(g_mu);
    const pthread_t self = pthread_self();
    for (Slot& slot : g_slots)
        if (slot.used && pthread_equal(slot.thread, self)) return;
    for (Slot& slot : g_slots)
        if (!slot.used) {
            slot = Slot{self, next.fetch_add(1), 0, true, {}, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) + 16};
            std::snprintf(slot.name, sizeof slot.name, "%s", name);
            return;
        }
}

void profile_unregister_thread() {
    std::lock_guard lk(g_mu);
    const pthread_t self = pthread_self();
    for (Slot& slot : g_slots)
        if (slot.used && pthread_equal(slot.thread, self)) slot.used = false;
}

void profile_frame() {
    if (g_running.load(std::memory_order_relaxed)) g_last_frame_ms.store(now_ms(), std::memory_order_relaxed);
}

void start_profiler() {
    if (g_running.exchange(true)) return;
    const auto self = reinterpret_cast<uintptr_t>(&start_profiler);
    // Code only, so that a waiting thread is named by its caller and not by a pointer to
    // data that happens to be on its stack: .text ends where the unwind tables start
    // (the title's linker script), and is under 256 MB.
    g_hi = reinterpret_cast<uintptr_t>(__eh_frame_hdr_start);
    if (g_hi <= self) g_hi = self + (64u << 20);
    g_lo = g_hi > (256u << 20) ? g_hi - (256u << 20) : 0x1000;
    struct sigaction sa {};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, nullptr) != 0) {
        log("prof: no signal handler; not profiling");
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, run, nullptr) != 0) {
        log("prof: no thread; not profiling");
        return;
    }
    pthread_detach(thread);
    log("prof: sampling started; anchor: rt::start_profiler is at %p", reinterpret_cast<void*>(self));
}

}  // namespace rt
#endif
