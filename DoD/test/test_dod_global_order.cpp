#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hotstuff/graph.h"

using namespace hotstuff;

namespace {

uint256_t tx(uint64_t value) {
    DataStream stream;
    stream << value;
    return stream.get_hash();
}

using Graph = std::unordered_map<uint256_t, std::unordered_set<uint256_t>>;

Graph local(std::initializer_list<std::pair<uint256_t, std::initializer_list<uint256_t>>> rows) {
    Graph graph;
    for (const auto &row : rows)
        graph.emplace(row.first, std::unordered_set<uint256_t>(row.second));
    return graph;
}

void cycle_becomes_one_batch() {
    const auto a = tx(1), b = tx(2), c = tx(3);
    const std::vector<Graph> locals = {
        local({{a, {b}}, {b, {c}}, {c, {}}}),
        local({{b, {c}}, {c, {a}}, {a, {}}}),
        local({{c, {a}}, {a, {b}}, {b, {}}}),
    };

    const auto result = build_dod_global_order(locals, 2.0, 2.0);
    assert(result.graph.size() == 3);
    assert(result.scc_count == 1);
    assert(result.batches.size() == 1);
    assert(result.batches.front().size() == 3);
    assert(result.missing_edges.empty());
    for (const auto &entry : result.graph)
        assert(entry.second.empty());
}

void pending_reaches_fixed_and_unrelated_pending_is_pruned() {
    const auto fixed = tx(11), reaches = tx(12), dangling = tx(13);
    const std::vector<Graph> locals = {
        local({{fixed, {dangling}}, {reaches, {fixed}}, {dangling, {}}}),
        local({{fixed, {dangling}}, {reaches, {fixed}}, {dangling, {}}}),
        local({{fixed, {}}, {reaches, {}}, {dangling, {}}}),
    };

    const auto result = build_dod_global_order(locals, 3.0, 2.0);
    assert(result.graph.count(fixed) == 1);
    assert(result.graph.count(reaches) == 1);
    assert(result.graph.count(dangling) == 0);
    assert(result.pruned_pending == 1);
}

void unresolved_pair_is_carried_forward() {
    const auto a = tx(21), b = tx(22);
    const std::vector<Graph> locals = {
        local({{a, {b}}, {b, {}}}),
        local({{a, {}}, {b, {}}}),
        local({{a, {}}, {b, {}}}),
    };

    const auto result = build_dod_global_order(locals, 3.0, 2.0);
    assert(result.graph.size() == 2);
    assert(result.graph.at(a).empty());
    assert(result.graph.at(b).empty());
    assert(result.missing_edges.size() == 1);
}

void l_update_only_round_counts_as_local_order_evidence() {
    EntityStorage storage;
    const auto a = tx(31), b = tx(32);
    storage.add_local_order(0, {}, {{a, b}});
    storage.add_local_order(1, {}, {{a, b}});
    assert(storage.get_local_order_cache_size() == 2);
}

} // namespace

int main() {
    cycle_becomes_one_batch();
    pending_reaches_fixed_and_unrelated_pending_is_pruned();
    unresolved_pair_is_carried_forward();
    l_update_only_round_counts_as_local_order_evidence();
    return 0;
}
