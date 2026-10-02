#pragma once

#include <cstdio>
#include <chrono>
#include <unistd.h>
#include <string.h>

/**
 * Stage-level profiling for Order-Fairness protocols.
 * Writes directly to stderr (bypasses HOTSTUFF_LOG_INFO which is compiled out).
 *
 * Output format: [stage_perf] replica=R stage=S height=H txns=N elapsed_us=U
 */

#ifndef HOTSTUFF_STAGE_PROF
#define HOTSTUFF_STAGE_PROF
#endif

#ifdef HOTSTUFF_STAGE_PROF

#define STAGE_NOW() std::chrono::steady_clock::now()

#define STAGE_US(_start) \
    (double)std::chrono::duration_cast<std::chrono::microseconds>( \
        std::chrono::steady_clock::now() - (_start)).count()

#define STAGE_PROF(stage, height, txns, elapsed_us) \
    do { \
        char _stage_buf[256]; \
        int _stage_len = snprintf(_stage_buf, sizeof(_stage_buf), \
            "[stage_perf] stage=%s height=%u txns=%zu elapsed_us=%.3f\n", \
            stage, (unsigned)(height), (size_t)(txns), (double)(elapsed_us)); \
        if (_stage_len > 0) { \
            if (_stage_len > (int)sizeof(_stage_buf)) _stage_len = sizeof(_stage_buf); \
            int _stage_wr = write(STDERR_FILENO, _stage_buf, _stage_len); \
            (void)_stage_wr; \
        } \
    } while(0)

struct StageScopeTimer {
    const char* stage_name;
    unsigned height_val;
    size_t txns_val;
    std::chrono::steady_clock::time_point start_ts;

    StageScopeTimer(const char* s, unsigned h, size_t n)
        : stage_name(s), height_val(h), txns_val(n),
          start_ts(std::chrono::steady_clock::now()) {}

    ~StageScopeTimer() {
        double us = (double)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_ts).count();
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "[stage_perf] stage=%s height=%u txns=%zu elapsed_us=%.3f\n",
            stage_name, height_val, txns_val, us);
        if (len > 0) {
            if (len > (int)sizeof(buf)) len = sizeof(buf);
            write(STDERR_FILENO, buf, len);
        }
    }
};

#define STAGE_SCOPE(stage, height, txns) \
    StageScopeTimer _stage_timer_##__LINE__(stage, (unsigned)(height), (size_t)(txns))

#else // !HOTSTUFF_STAGE_PROF

#define STAGE_NOW() std::chrono::steady_clock::now()
#define STAGE_US(_start) 0.0
#define STAGE_PROF(stage, height, txns, elapsed_us) ((void)0)
#define STAGE_SCOPE(stage, height, txns) ((void)0)

#endif // HOTSTUFF_STAGE_PROF
