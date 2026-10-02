#include "src/fairdag_graph.h"

#include <algorithm>
#include <functional>
#include <queue>

namespace hotstuff {

void FairDAGGraph::AddNode(uint64_t id) {
    std::lock_guard<std::mutex> guard(mutex_);
    nodes_.insert(id);
    edges_.emplace(id, std::unordered_set<uint64_t>());
}

void FairDAGGraph::AddEdge(uint64_t from, uint64_t to) {
    if (from == to) return;
    std::lock_guard<std::mutex> guard(mutex_);
    nodes_.insert(from);
    nodes_.insert(to);
    edges_.emplace(from, std::unordered_set<uint64_t>());
    edges_.emplace(to, std::unordered_set<uint64_t>());
    edges_[from].insert(to);
}

bool FairDAGGraph::HasEdgeUnlocked(uint64_t from, uint64_t to) const {
    auto it = edges_.find(from);
    return it != edges_.end() && it->second.count(to) > 0;
}

bool FairDAGGraph::IsTournament() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<uint64_t> nodes(nodes_.begin(), nodes_.end());
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (HasEdgeUnlocked(nodes[i], nodes[i])) return false;
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            bool forward = HasEdgeUnlocked(nodes[i], nodes[j]);
            bool reverse = HasEdgeUnlocked(nodes[j], nodes[i]);
            if (forward == reverse) return false;
        }
    }
    return true;
}

std::vector<std::vector<uint64_t>> FairDAGGraph::TarjanSCCUnlocked() const {
    std::vector<uint64_t> nodes(nodes_.begin(), nodes_.end());
    std::sort(nodes.begin(), nodes.end());

    std::unordered_map<uint64_t, int> index;
    std::unordered_map<uint64_t, int> lowlink;
    std::unordered_set<uint64_t> on_stack;
    std::vector<uint64_t> stack;
    std::vector<std::vector<uint64_t>> sccs;
    int next_index = 0;

    std::function<void(uint64_t)> strong_connect = [&](uint64_t v) {
        index[v] = next_index;
        lowlink[v] = next_index;
        ++next_index;
        stack.push_back(v);
        on_stack.insert(v);

        std::vector<uint64_t> out;
        auto it = edges_.find(v);
        if (it != edges_.end()) {
            out.assign(it->second.begin(), it->second.end());
            std::sort(out.begin(), out.end());
        }

        for (uint64_t w : out) {
            if (nodes_.count(w) == 0) continue;
            if (index.find(w) == index.end()) {
                strong_connect(w);
                lowlink[v] = std::min(lowlink[v], lowlink[w]);
            } else if (on_stack.count(w) > 0) {
                lowlink[v] = std::min(lowlink[v], index[w]);
            }
        }

        if (lowlink[v] == index[v]) {
            std::vector<uint64_t> scc;
            while (!stack.empty()) {
                uint64_t w = stack.back();
                stack.pop_back();
                on_stack.erase(w);
                scc.push_back(w);
                if (w == v) break;
            }
            std::sort(scc.begin(), scc.end());
            sccs.push_back(std::move(scc));
        }
    };

    for (uint64_t v : nodes) {
        if (index.find(v) == index.end()) strong_connect(v);
    }
    return sccs;
}

std::vector<int> FairDAGGraph::TopologicalSCCOrderUnlocked(
    const std::vector<std::vector<uint64_t>> &sccs) const {
    const int n = static_cast<int>(sccs.size());
    std::unordered_map<uint64_t, int> comp;
    for (int i = 0; i < n; ++i) {
        for (uint64_t node : sccs[i]) comp[node] = i;
    }

    std::vector<std::unordered_set<int>> dag(n);
    std::vector<int> indeg(n, 0);
    for (const auto &entry : edges_) {
        auto from_it = comp.find(entry.first);
        if (from_it == comp.end()) continue;
        int from_comp = from_it->second;
        for (uint64_t to : entry.second) {
            auto to_it = comp.find(to);
            if (to_it == comp.end()) continue;
            int to_comp = to_it->second;
            if (from_comp != to_comp && dag[from_comp].insert(to_comp).second) {
                ++indeg[to_comp];
            }
        }
    }

    auto comp_less = [&sccs](int a, int b) {
        uint64_t amin = sccs[a].empty() ? 0 : sccs[a].front();
        uint64_t bmin = sccs[b].empty() ? 0 : sccs[b].front();
        return amin > bmin;
    };
    std::priority_queue<int, std::vector<int>, decltype(comp_less)> q(comp_less);
    for (int i = 0; i < n; ++i) {
        if (indeg[i] == 0) q.push(i);
    }

    std::vector<int> order;
    order.reserve(n);
    while (!q.empty()) {
        int u = q.top();
        q.pop();
        order.push_back(u);

        std::vector<int> children(dag[u].begin(), dag[u].end());
        std::sort(children.begin(), children.end(), [&sccs](int a, int b) {
            return sccs[a].front() < sccs[b].front();
        });
        for (int v : children) {
            if (--indeg[v] == 0) q.push(v);
        }
    }

    if (static_cast<int>(order.size()) != n) {
        std::vector<bool> seen(n, false);
        for (int c : order) seen[c] = true;
        for (int i = 0; i < n; ++i) {
            if (!seen[i]) order.push_back(i);
        }
    }
    return order;
}

std::vector<uint64_t> FairDAGGraph::HamiltonianPathUnlocked(
    std::vector<uint64_t> nodes) const {
    std::sort(nodes.begin(), nodes.end());
    std::vector<uint64_t> path;
    path.reserve(nodes.size());

    for (uint64_t v : nodes) {
        auto pos = path.begin();
        while (pos != path.end() && HasEdgeUnlocked(*pos, v)) ++pos;
        path.insert(pos, v);
    }
    return path;
}

std::vector<uint64_t> FairDAGGraph::GetOrder() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<uint64_t> order;
    order.reserve(nodes_.size());
    if (nodes_.empty()) return order;

    auto sccs = TarjanSCCUnlocked();
    auto scc_order = TopologicalSCCOrderUnlocked(sccs);
    for (int comp_id : scc_order) {
        auto path = HamiltonianPathUnlocked(sccs[comp_id]);
        order.insert(order.end(), path.begin(), path.end());
    }
    return order;
}

void FairDAGGraph::ClearGraph() {
    std::lock_guard<std::mutex> guard(mutex_);
    nodes_.clear();
    edges_.clear();
}

std::vector<uint64_t> FairDAGGraph::GetAllNode() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<uint64_t> nodes(nodes_.begin(), nodes_.end());
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

size_t FairDAGGraph::Size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return nodes_.size();
}

} // namespace hotstuff
