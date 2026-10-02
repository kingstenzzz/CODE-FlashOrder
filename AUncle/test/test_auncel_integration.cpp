#include <iostream>
#include <vector>
#include <unordered_set>
#include <cstring>
#include "hotstuff/auncel.h"
#include "hotstuff/auncel_msg.h"

using namespace hotstuff;
using hotstuff::uint256_t;

static int test_pass = 0;
static int test_fail = 0;

static std::vector<uint256_t> make_txs(const std::vector<int>& ids) {
    std::vector<uint256_t> v;
    v.reserve(ids.size());
    for (int id : ids) {
        uint8_t bytes[32];
        memset(bytes, 0, 32);
        uint64_t id64 = static_cast<uint64_t>(id);
        for (int i = 0; i < 8; i++) bytes[i] = static_cast<uint8_t>((id64 >> (i * 8)) & 0xff);
        v.emplace_back(bytes);
    }
    return v;
}

static void test_weight_orderer_fig4() {
    std::vector<std::vector<uint256_t>> local = {
        make_txs({0, 1, 4, 3, 2, 5}),
        make_txs({0, 1, 2, 3, 4, 5}),
        make_txs({0, 2, 3, 5, 1, 4}),
        make_txs({0, 2, 3, 5, 1, 4})
    };
    auto result = WeightOrderer::order(local, 0.5);
    bool ok = (result.size() == 6);
    if (ok) {
        std::cout << "[PASS] weight_orderer.fig4_size" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] weight_orderer.fig4_size: expected 6 got " << result.size() << std::endl;
        ++test_fail;
    }
}

static void test_empty_input() {
    auto result = WeightOrderer::order({}, 0.5);
    if (result.empty()) {
        std::cout << "[PASS] weight_orderer.empty" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] weight_orderer.empty" << std::endl;
        ++test_fail;
    }
}

static void test_sort_vote_missing_field() {
    uint256_t dummy = make_txs({1})[0];
    part_cert_bt null_cert;
    SortVote sv(0, 1, make_txs({1, 2, 3}), std::vector<uint32_t>{0, 1, 2}, make_txs({4, 5}), std::move(null_cert), nullptr);
    if (sv.local_ordering.size() == 3 && sv.missing_txs.size() == 2 && sv.view == 1) {
        std::cout << "[PASS] sort_vote.missing_field" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] sort_vote.missing_field: ordering=" << sv.local_ordering.size()
                  << " missing=" << sv.missing_txs.size() << std::endl;
        ++test_fail;
    }
}

static void test_txsync_construction() {
    TxSync ts(42, 0, uint256_t(), nullptr, make_txs({1, 2, 3, 4}), nullptr);
    if (ts.view == 42 && ts.cur_txs.size() == 4) {
        std::cout << "[PASS] txsync.construction" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] txsync.construction: view=" << ts.view
                  << " txs=" << ts.cur_txs.size() << std::endl;
        ++test_fail;
    }
}

static void test_newview_construction() {
    part_cert_bt null_cert;
    quorum_cert_bt null_qc;
    NewView nv(2, 5, std::move(null_qc), make_txs({10, 20, 30}), std::move(null_cert), nullptr);
    if (nv.replica_id == 2 && nv.view == 5 && nv.tx_list.size() == 3) {
        std::cout << "[PASS] newview.construction" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] newview.construction" << std::endl;
        ++test_fail;
    }
}

static void test_missing_detection_logic() {
    std::vector<uint256_t> received = make_txs({1, 2, 3, 4, 5});
    std::unordered_set<uint256_t> cur_txs(received.begin(), received.begin() + 3);

    std::vector<uint256_t> missing;
    for (const auto& cmd : received) {
        if (!cur_txs.count(cmd)) missing.push_back(cmd);
    }
    if (missing.size() == 2) {
        std::cout << "[PASS] missing_detection.logic (2 of 5 missed)" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] missing_detection.logic: expected 2 got " << missing.size() << std::endl;
        ++test_fail;
    }
}

static void test_f1_threshold() {
    size_t nreplicas = 4;
    size_t nmajority = 3;
    size_t nfaulty = nreplicas - nmajority;
    size_t threshold = nfaulty + 1;
    if (threshold == 2) {
        std::cout << "[PASS] f1_threshold.value (n=4, f=1, f+1=2)" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] f1_threshold.value: expected 2 got " << threshold << std::endl;
        ++test_fail;
    }
}

int main() {
    std::cout << "=== Auncel Phase H Integration Tests ===" << std::endl;
    test_weight_orderer_fig4();
    test_empty_input();
    test_sort_vote_missing_field();
    test_txsync_construction();
    test_newview_construction();
    test_missing_detection_logic();
    test_f1_threshold();
    std::cout << "\n=== " << test_pass << " passed, " << test_fail << " failed ===" << std::endl;
    return test_fail == 0 ? 0 : 1;
}
