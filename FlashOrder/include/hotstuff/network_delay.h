#ifndef _HOTSTUFF_NETWORK_DELAY_H
#define _HOTSTUFF_NETWORK_DELAY_H

#include <chrono>
#include <unordered_map>
#include <mutex>
#include "hotstuff/type.h"

namespace hotstuff {

/**
 * NetworkDelayEstimator -- RTT-based network delay measurement.
 *
 * Each replica periodically sends Ping messages to all peers.
 * Peers immediately reply with Pong.
 * RTT = send_time -> receive_time.
 * One-way delay estimate = RTT / 2.
 * EWMA smoothing: SRTT = alpha * SRTT + (1-alpha) * RTT_new.
 */
class NetworkDelayEstimator {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

private:
    double srtt_ms;           // smoothed RTT (ms), EWMA
    double alpha;             // smoothing factor
    size_t sample_count;
    mutable std::mutex mtx;

    // pending ping timestamps keyed by ping_seq
    std::unordered_map<uint64_t, time_point> pending_pings;

public:
    NetworkDelayEstimator(double init_ms = 100.0, double alpha = 0.1)
        : srtt_ms(init_ms), alpha(alpha), sample_count(0) {}

    /**
     * Called when a Ping is sent. Records the send time.
     * @param seq  monotonically increasing sequence number for matching.
     */
    void on_ping_sent(uint64_t seq) {
        std::lock_guard<std::mutex> lock(mtx);
        pending_pings[seq] = clock::now();
    }

    /**
     * Called when the corresponding Pong is received.
     * Computes RTT and updates EWMA.
     * @param seq  the sequence number from the Pong message.
     */
    void on_pong_received(uint64_t seq) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = pending_pings.find(seq);
        if (it == pending_pings.end())
            return;

        double rtt_ms = std::chrono::duration_cast<
            std::chrono::microseconds>(clock::now() - it->second).count() / 1000.0;

        pending_pings.erase(it);

        // EWMA update
        if (sample_count == 0)
            srtt_ms = rtt_ms;   // first sample: initialize directly
        else
            srtt_ms = alpha * srtt_ms + (1.0 - alpha) * rtt_ms;
        sample_count++;

        HOTSTUFF_LOG_INFO("[[NetDelay]] RTT=%.3f ms, SRTT=%.3f ms (samples=%zu)",
                         rtt_ms, srtt_ms, sample_count);
    }

    /**
     * Returns the estimated one-way network delay (RTT / 2) in milliseconds.
     */
    double get_one_way_delay_ms() const {
        std::lock_guard<std::mutex> lock(mtx);
        return srtt_ms / 2.0;
    }

    /**
     * Returns the smoothed RTT in milliseconds.
     */
    double get_srtt_ms() const {
        std::lock_guard<std::mutex> lock(mtx);
        return srtt_ms;
    }

    size_t get_sample_count() const {
        std::lock_guard<std::mutex> lock(mtx);
        return sample_count;
    }

    bool is_ready() const {
        std::lock_guard<std::mutex> lock(mtx);
        return sample_count >= 3;   // need at least a few samples
    }
};

} // namespace hotstuff

#endif
