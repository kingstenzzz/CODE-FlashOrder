#include <iostream>
#include <vector>
#include <string>
#include "hotstuff/auncel.h"

using namespace hotstuff;
using hotstuff::uint256_t;

static int test_pass = 0;
static int test_fail = 0;

#define ASSERT_EQ(actual, expected, name)                                      \
    do {                                                                       \
        if ((actual) == (expected)) {                                          \
            std::cout << "[PASS] " << (name) << std::endl;                     \
            ++test_pass;                                                       \
        } else {                                                               \
            std::cout << "[FAIL] " << (name)                                   \
                      << " — expected size " << (expected).size()              \
                      << ", got " << (actual).size() << std::endl;             \
            ++test_fail;                                                       \
        }                                                                      \
    } while (0)

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

static int to_id(const uint256_t& tx) {
    bytearray_t arr = tx;
    if (arr.size() < 4) return -1;
    int id = 0;
    for (int i = 0; i < 4; i++) id |= static_cast<int>(arr[i]) << (i * 8);
    return id;
}

static std::vector<int> to_ids(const std::vector<uint256_t>& txs) {
    std::vector<int> ids;
    ids.reserve(txs.size());
    for (const auto& tx : txs) ids.push_back(to_id(tx));
    return ids;
}

static void test_fig4() {
    std::vector<std::vector<uint256_t>> local_orderings = {
        make_txs({0, 1, 4, 3, 2, 5}),
        make_txs({0, 1, 2, 3, 4, 5}),
        make_txs({0, 2, 3, 5, 1, 4}),
        make_txs({0, 2, 3, 5, 1, 4})
    };
    auto result = WeightOrderer::order(local_orderings, 0.5);
    auto result_ids = to_ids(result);
    std::vector<int> expected = {0, 2, 1, 3, 4, 5};

    std::cout << "Fig.4 result: [";
    for (size_t i = 0; i < result_ids.size(); ++i) {
        std::cout << result_ids[i] << (i + 1 == result_ids.size() ? "" : ", ");
    }
    std::cout << "]" << std::endl;

    bool ok = (result_ids == expected);
    if (ok) { std::cout << "[PASS] test_fig4" << std::endl; ++test_pass; }
    else    { std::cout << "[FAIL] test_fig4 — expected [0, 2, 1, 3, 4, 5]" << std::endl; ++test_fail; }
}

static void test_empty() {
    auto result = WeightOrderer::order({}, 0.5);
    ASSERT_EQ(result, std::vector<uint256_t>{}, "test_empty");
}

static void test_single_replica() {
    auto ordering = make_txs({3, 1, 2});
    auto result = WeightOrderer::order({ordering}, 0.5);
    auto ids = to_ids(result);
    std::vector<int> expected = {3, 1, 2};
    bool ok = (ids == expected);
    if (ok) { std::cout << "[PASS] test_single_replica" << std::endl; ++test_pass; }
    else    { std::cout << "[FAIL] test_single_replica — expected [3, 1, 2]" << std::endl; ++test_fail; }
}

static void test_weight_order_phase_only() {
    auto local = std::vector<std::vector<uint256_t>>{
        make_txs({0, 1, 4, 3, 2, 5}),
        make_txs({0, 1, 2, 3, 4, 5}),
        make_txs({0, 2, 3, 5, 1, 4}),
        make_txs({0, 2, 3, 5, 1, 4})
    };
    auto linearized = WeightOrderer::weight_order_phase(local, 0.5);
    auto ids = to_ids(linearized);
    std::cout << "Weight-only linearized: [";
    for (size_t i = 0; i < ids.size(); ++i) {
        std::cout << ids[i] << (i + 1 == ids.size() ? "" : ", ");
    }
    std::cout << "]" << std::endl;

    std::vector<int> expected = {0, 2, 1, 3, 4, 5};
    bool ok = (ids == expected);
    if (ok) { std::cout << "[PASS] test_weight_order_phase_only" << std::endl; ++test_pass; }
    else    { std::cout << "[FAIL] test_weight_order_phase_only" << std::endl; ++test_fail; }
}

static void test_final_order_phase_only() {
    struct TestCase {
        std::string name;
        std::vector<int> linearized_in;
        std::vector<int> expected;
    };
    std::vector<TestCase> cases = {
        {"empty", {}, {}},
        {"single", {42}, {42}},
        {"two", {1, 2}, {1, 2}},
        {"three", {1, 2, 3}, {1, 2, 3}},
        {"four_already_sorted", {1, 2, 3, 4}, {1, 2, 3, 4}},
        {"five_identity", {1, 2, 3, 4, 5}, {1, 2, 3, 4, 5}},
        {"six_fig4_linearized", {0, 2, 1, 3, 4, 5}, {0, 2, 1, 3, 4, 5}},
    };

    for (const auto& tc : cases) {
        auto txs = make_txs(tc.linearized_in);
        auto result = WeightOrderer::final_order_phase(txs);
        auto ids = to_ids(result);
        bool ok = (ids == tc.expected);
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << "final_order." << tc.name
                  << "  linearized=[";
        for (size_t i = 0; i < tc.linearized_in.size(); ++i)
            std::cout << tc.linearized_in[i] << (i+1==tc.linearized_in.size()?"":",");
        std::cout << "] got=[";
        for (size_t i = 0; i < ids.size(); ++i)
            std::cout << ids[i] << (i+1==ids.size()?"":",");
        std::cout << "]" << std::endl;
        if (ok) ++test_pass; else ++test_fail;
    }
}

static void test_final_phase_preserves_linearized() {
    auto local = std::vector<std::vector<uint256_t>>{
        make_txs({0, 1, 4, 3, 2, 5}),
        make_txs({0, 1, 2, 3, 4, 5}),
        make_txs({0, 2, 3, 5, 1, 4}),
        make_txs({0, 2, 3, 5, 1, 4})
    };
    auto only_weight = WeightOrderer::weight_order_phase(local, 0.5);
    auto with_final = WeightOrderer::order(local, 0.5);
    bool ok = (to_ids(only_weight) == to_ids(with_final));
    if (ok) {
        std::cout << "[PASS] test_final_phase_preserves_linearized "
                     "(weight-only == weight+final for Fig.4)" << std::endl;
        ++test_pass;
    } else {
        std::cout << "[FAIL] test_final_phase_preserves_linearized" << std::endl;
        std::cout << "  weight-only: [";
        for (auto id : to_ids(only_weight)) std::cout << id << ",";
        std::cout << "]" << std::endl;
        std::cout << "  with-final:  [";
        for (auto id : to_ids(with_final)) std::cout << id << ",";
        std::cout << "]" << std::endl;
        ++test_fail;
    }
}

static void test_fig4_k08() {
    std::vector<std::vector<uint256_t>> local_orderings = {
        make_txs({0, 1, 4, 3, 2, 5}),
        make_txs({0, 1, 2, 3, 4, 5}),
        make_txs({0, 2, 3, 5, 1, 4}),
        make_txs({0, 2, 3, 5, 1, 4})
    };
    auto result = WeightOrderer::order(local_orderings, 0.8);
    auto result_ids = to_ids(result);
    std::vector<int> expected = {0, 2, 1, 3, 4, 5};
    bool ok = (result_ids == expected);
    if (ok) { std::cout << "[PASS] test_fig4_k08 (cross-val with Python)" << std::endl; ++test_pass; }
    else    { std::cout << "[FAIL] test_fig4_k08 — expected [0, 2, 1, 3, 4, 5], got [";
              for (auto id : result_ids) std::cout << id << ","; std::cout << "]" << std::endl; ++test_fail; }
}

static void test_big_k08() {
    std::vector<std::vector<uint256_t>> local_orderings = {
        make_txs({7, 3, 1, 9, 5, 0, 8, 4, 2, 6}),
        make_txs({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}),
        make_txs({9, 8, 7, 6, 5, 4, 3, 2, 1, 0}),
        make_txs({3, 1, 4, 7, 5, 9, 2, 6, 8, 0}),
        make_txs({5, 0, 9, 3, 7, 2, 8, 1, 6, 4})
    };
    auto result = WeightOrderer::order(local_orderings, 0.8);
    auto result_ids = to_ids(result);
    std::vector<int> expected = {3, 7, 1, 9, 5, 0, 4, 8, 2, 6};
    bool ok = (result_ids == expected);
    if (ok) { std::cout << "[PASS] test_big_k08 (cross-val with Python)" << std::endl; ++test_pass; }
    else    { std::cout << "[FAIL] test_big_k08 — expected [3,7,1,9,5,0,4,8,2,6], got [";
              for (auto id : result_ids) std::cout << id << ","; std::cout << "]" << std::endl; ++test_fail; }
}

int main() {
    std::cout << "=== Auncel WeightOrderer Tests ===" << std::endl;
    test_fig4();
    test_fig4_k08();
    test_big_k08();
    test_empty();
    test_single_replica();
    test_weight_order_phase_only();
    std::cout << "\n=== Final Order Phase (Branch Sort) Tests ===" << std::endl;
    test_final_order_phase_only();
    test_final_phase_preserves_linearized();
    std::cout << "\n=== " << test_pass << " passed, " << test_fail << " failed ===" << std::endl;
    return test_fail == 0 ? 0 : 1;
}
