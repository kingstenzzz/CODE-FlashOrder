/**
 * Copyright 2018 VMware
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 *          Author: Heena Nagda
 *           Project: Themis
 */

#include <unordered_map>
#include <algorithm>
#include <functional>
#include <queue>
#include <string>
#include "hotstuff/graph.h"
#include "hotstuff/entity.h"

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN
#define LOG_PROTO HOTSTUFF_LOG_PROTO

namespace hotstuff {

namespace {

using Graph = std::unordered_map<uint256_t, std::unordered_set<uint256_t>>;

bool tx_less(const uint256_t &lhs, const uint256_t &rhs) {
    return get_hex(lhs) < get_hex(rhs);
}

std::vector<uint256_t> sorted_nodes(const std::unordered_set<uint256_t> &nodes) {
    std::vector<uint256_t> result(nodes.begin(), nodes.end());
    std::sort(result.begin(), result.end(), tx_less);
    return result;
}

} // namespace

DoDGlobalOrderResult build_dod_global_order(
        const std::vector<std::unordered_map<uint256_t, std::unordered_set<uint256_t>>> &local_graphs,
        double fixed_tx_threshold,
        double pending_tx_threshold,
        const std::unordered_map<uint256_t, size_t> &tx_weight,
        const std::unordered_map<uint256_t, std::unordered_map<uint256_t, size_t>> &edge_weight) {
    DoDGlobalOrderResult result;
    std::unordered_set<uint256_t> all_nodes;
    std::unordered_map<uint256_t, size_t> tx_count;
    std::unordered_map<uint256_t, std::unordered_map<uint256_t, size_t>> edge_count;

    if (!tx_weight.empty() || !edge_weight.empty())
    {
        /* Persistent leader-side weights: every distinct (replica, tx) and
         * (replica, edge) observation ever received, accumulated across
         * rounds.  This is the DoD Algorithm 1/2 carry-over that keeps
         * pruned-pending transactions from being lost between rounds. */
        for (const auto &entry : tx_weight)
        {
            all_nodes.insert(entry.first);
            tx_count[entry.first] = entry.second;
        }
        for (const auto &from : edge_weight)
            for (const auto &to : from.second)
            {
                all_nodes.insert(from.first);
                all_nodes.insert(to.first);
                edge_count[from.first][to.first] = to.second;
            }
    }

    for (const auto &local : local_graphs) {
        for (const auto &entry : local) {
            all_nodes.insert(entry.first);
            for (const auto &to : entry.second) {
                all_nodes.insert(to);
                if (tx_weight.empty() && edge_weight.empty())
                    ++edge_count[entry.first][to];
            }
            if (tx_weight.empty() && edge_weight.empty())
                ++tx_count[entry.first];
        }
    }

    const auto nodes = sorted_nodes(all_nodes);
    /* Rank map: integer comparison key matching the hex-string order.
     * One O(V log V) string sort per call; every downstream sort then uses
     * cheap integer comparisons (behaviorally identical output). */
    std::unordered_map<uint256_t, size_t> rank;
    rank.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        rank.emplace(nodes[i], i);
    auto rank_less = [&rank](const uint256_t &lhs, const uint256_t &rhs) {
        return rank.at(lhs) < rank.at(rhs);
    };

    Graph graph;
    for (const auto &tx : nodes) {
        const auto count = static_cast<double>(tx_count[tx]);
        // DoD labels a transaction fixed at n-2f observations and pending at
        // the receive-order threshold. Pending transactions may be pruned
        // later if they cannot reach a fixed transaction.
        if (count >= fixed_tx_threshold || count >= pending_tx_threshold)
            graph.emplace(tx, std::unordered_set<uint256_t>());
        if (count >= fixed_tx_threshold)
            result.fixed_transactions.insert(tx);
    }

    /* Fast path (identical output, skips the O(V^2)+SCC work): without any
     * fixed transaction no component can be retained, so the whole round is
     * pruned-pending carry-over. */
    if (result.fixed_transactions.empty()) {
        result.pruned_pending = all_nodes.size();
        return result;
    }

    // Enumerate every pair, including equal-strength opposing observations.
    // Such a pair remains unresolved and is carried as a missing edge.
    std::vector<char> in_graph(nodes.size(), 0);
    std::vector<const std::unordered_map<uint256_t, size_t> *> edge_rows(nodes.size(), nullptr);
    for (size_t i = 0; i < nodes.size(); ++i) {
        in_graph[i] = graph.count(nodes[i]) != 0;
        const auto it = edge_count.find(nodes[i]);
        edge_rows[i] = (it == edge_count.end()) ? nullptr : &it->second;
    }
    std::vector<std::pair<uint256_t, uint256_t>> missing;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!in_graph[i]) continue;
        const auto &ni = nodes[i];
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            if (!in_graph[j]) continue;
            const auto &nj = nodes[j];
            size_t ab = 0, ba = 0;
            if (edge_rows[i]) {
                const auto it = edge_rows[i]->find(nj);
                if (it != edge_rows[i]->end()) ab = it->second;
            }
            if (edge_rows[j]) {
                const auto it = edge_rows[j]->find(ni);
                if (it != edge_rows[j]->end()) ba = it->second;
            }
            bool inserted = false;
            if (static_cast<double>(ab) >= pending_tx_threshold && ab > ba) {
                graph[ni].insert(nj);
                inserted = true;
            } else if (static_cast<double>(ba) >= pending_tx_threshold && ba > ab) {
                graph[nj].insert(ni);
                inserted = true;
            }
            if (!inserted && (ab > 0 || ba > 0))
                missing.emplace_back(ni, nj);
        }
    }

    // Kosaraju SCC decomposition. SCC members become one DoD batch, so their
    // internal ordering remains intentionally unspecified.
    Graph reverse;
    for (const auto &tx : nodes) {
        graph[tx];
        reverse[tx];
    }
    for (const auto &entry : graph)
        for (const auto &to : entry.second)
            reverse[to].insert(entry.first);

    std::unordered_set<uint256_t> visited;
    std::vector<uint256_t> finish;
    std::function<void(const uint256_t &)> visit = [&](const uint256_t &tx) {
        if (!visited.insert(tx).second) return;
        std::vector<uint256_t> children(graph[tx].begin(), graph[tx].end());
        std::sort(children.begin(), children.end(), rank_less);
        for (const auto &child : children) visit(child);
        finish.push_back(tx);
    };
    for (const auto &tx : nodes) visit(tx);

    visited.clear();
    std::vector<std::vector<uint256_t>> components;
    std::function<void(const uint256_t &, std::vector<uint256_t> &)> collect =
        [&](const uint256_t &tx, std::vector<uint256_t> &component) {
            if (!visited.insert(tx).second) return;
            component.push_back(tx);
            std::vector<uint256_t> parents(reverse[tx].begin(), reverse[tx].end());
            std::sort(parents.begin(), parents.end(), rank_less);
            for (const auto &parent : parents) collect(parent, component);
        };
    for (auto it = finish.rbegin(); it != finish.rend(); ++it) {
        if (visited.count(*it)) continue;
        std::vector<uint256_t> component;
        collect(*it, component);
        std::sort(component.begin(), component.end(), rank_less);
        components.push_back(std::move(component));
    }
    result.scc_count = components.size();

    std::unordered_map<uint256_t, size_t> component_of;
    for (size_t c = 0; c < components.size(); ++c)
        for (const auto &tx : components[c]) component_of[tx] = c;

    std::vector<std::unordered_set<size_t>> component_edges(components.size());
    std::vector<std::unordered_set<size_t>> component_reverse(components.size());
    for (const auto &entry : graph) {
        const size_t from = component_of[entry.first];
        for (const auto &to_tx : entry.second) {
            const size_t to = component_of[to_tx];
            if (from != to) {
                component_edges[from].insert(to);
                component_reverse[to].insert(from);
            }
        }
    }

    // Keep fixed components and pending components that can reach a fixed
    // component in the condensation DAG.
    std::unordered_set<size_t> keep_components;
    std::queue<size_t> keep_queue;
    for (size_t c = 0; c < components.size(); ++c) {
        bool fixed = false;
        for (const auto &tx : components[c]) {
            if (result.fixed_transactions.count(tx)) {
                fixed = true;
                break;
            }
        }
        if (fixed && keep_components.insert(c).second) keep_queue.push(c);
    }
    while (!keep_queue.empty()) {
        const size_t current = keep_queue.front();
        keep_queue.pop();
        for (const auto predecessor : component_reverse[current])
            if (keep_components.insert(predecessor).second) keep_queue.push(predecessor);
    }
    for (size_t c = 0; c < components.size(); ++c)
        if (!keep_components.count(c)) result.pruned_pending += components[c].size();

    for (const auto &tx : nodes) {
        const size_t c = component_of[tx];
        if (keep_components.count(c)) result.graph.emplace(tx, std::unordered_set<uint256_t>());
    }
    for (const auto &entry : graph) {
        const size_t from_component = component_of[entry.first];
        if (!keep_components.count(from_component)) continue;
        for (const auto &to : entry.second) {
            const size_t to_component = component_of[to];
            if (from_component != to_component && keep_components.count(to_component))
                result.graph[entry.first].insert(to);
        }
    }

    for (const auto &pair : missing) {
        if (result.graph.count(pair.first) == 0 || result.graph.count(pair.second) == 0)
            continue;
        if (result.graph[pair.first].count(pair.second) == 0 &&
            result.graph[pair.second].count(pair.first) == 0)
            result.missing_edges.push_back(pair);
    }

    // Deterministic topological order of retained SCC batches.
    std::vector<size_t> indegree(components.size(), 0);
    for (size_t from = 0; from < components.size(); ++from)
        if (keep_components.count(from))
            for (const auto to : component_edges[from])
                if (keep_components.count(to)) ++indegree[to];
    auto component_less = [&](size_t lhs, size_t rhs) {
        return rank_less(components[lhs].front(), components[rhs].front());
    };
    std::vector<size_t> ready;
    for (const auto c : keep_components)
        if (indegree[c] == 0) ready.push_back(c);
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end(), component_less);
        const size_t current = ready.front();
        ready.erase(ready.begin());
        result.batches.push_back(components[current]);
        for (const auto next : component_edges[current]) {
            if (!keep_components.count(next)) continue;
            if (--indegree[next] == 0) ready.push_back(next);
        }
    }
    return result;
}

    CondensationGraph::CondensationGraph(std::unordered_map<uint256_t, std::unordered_set<uint256_t>> const& graph){
        CondensationGraph::graph = graph;
        reset_visited();
        create_transposed_graph();
    }

    void CondensationGraph::reset_visited(){
        for(auto const &entry: graph) {
            visited[entry.first] = false;
        }
    }

    void CondensationGraph::create_transposed_graph(){
        for(auto const &entry: graph){
            uint256_t vertex = entry.first;
            /* We need all the vertices preset as a key in transposed graph even if value is an empty set */
            transposed_graph.insert(std::make_pair(vertex, std::unordered_set<uint256_t>()));
            /* Explore all the childs of the vertex of original graph */
            for(auto const &child_vertex: entry.second){
                transposed_graph[child_vertex].insert(vertex);
            }
        }
    }

    std::unordered_set<uint256_t> CondensationGraph::get_decendents_from_transposed_graph(uint256_t tx) {
        return transposed_graph[tx];
    }

    std::vector<std::vector<uint256_t>> CondensationGraph::get_condensation_graph() {
        /* Order vertices by their finish time */
        for(auto const &entry: graph) {
            uint256_t vertex = entry.first;
            if(visited[vertex]){
                continue;
            }
            create_order_by_finish_time(vertex);
        }

        /* reset visited as we are re-using it */
        reset_visited();

        /* Generate SCC in topological order */
        while(!order_by_finish_time.empty()) {
            uint256_t vertex = order_by_finish_time.top();
            order_by_finish_time.pop();

            if(visited[vertex]){
                continue;
            }
            std::vector<uint256_t> scc;
            generate_scc(vertex, scc);
            condensation_graph.push_back(scc);
        }

        /* Topologically sorted condensation graph */
        return condensation_graph;
    }

    void CondensationGraph::create_order_by_finish_time(uint256_t vertex) {
        visited[vertex] = true;

        /* explore children of this vertex */
        for(auto const child_vertex: graph[vertex]) {
            if(visited[child_vertex]){
                continue;
            }
            create_order_by_finish_time(child_vertex);
        } 
        order_by_finish_time.push(vertex);
    }

    void CondensationGraph::generate_scc(uint256_t vertex, std::vector<uint256_t> &scc){
        visited[vertex] = true;
        scc.push_back(vertex);

        /* explore children of this vertex */
        for(auto const child_vertex: transposed_graph[vertex]) {
            if(visited[child_vertex]){
                continue;
            }
            generate_scc(child_vertex, scc);
        } 
    }

    LocalOrderDAG::LocalOrderDAG(const std::vector<uint256_t> &order){
        this->order = order;
    }

    void LocalOrderDAG::add_dependency(uint256_t cmd_hash, std::unordered_map<uint64_t, char> dependency_info){
        for(auto info: dependency_info){
            this->dependencies[cmd_hash].insert(info.first);
            this->dependency_types[cmd_hash][info.first] = info.second;
        }
    }

    std::unordered_map<uint256_t, std::unordered_set<uint256_t>> LocalOrderDAG::create_dag(){
        HOTSTUFF_LOG_DEBUG("[[create_dag]] start");
        /** Create an empty graph G=(V,E) **/
        std::unordered_map<uint256_t, std::unordered_set<uint256_t>> dag;

        /** For v = 1 -> m: V = V U v **/
        for(auto cmd: order){
            dag[cmd] = std::unordered_set<uint256_t>();
        }

        /** Update edges based on dependency **/
        size_t n = order.size();
        for(int curr=1; curr<n; curr++){
            auto curr_tx = order[curr];

            /** Check the dependency between T[i] and T[j] **/
            for(auto dependency: dependencies[curr_tx]){
 
                for(int prev=curr-1; prev>=0; prev--){
                    auto prev_tx = order[prev];
                    // HOTSTUFF_LOG_DEBUG("curr_tx [%ld] %.10s, prev_tx [%ld] %.10s", curr, get_hex(curr_tx).c_str(), prev, get_hex(prev_tx).c_str());
                    // HOTSTUFF_LOG_DEBUG("dependency = %ld, curr type = %c, dependencies[prev_tx].count(dependency) = %d", dependency, dependency_types[curr_tx][dependency], dependencies[prev_tx].count(dependency));
                
                    if(dependencies[prev_tx].count(dependency)>0 
                        && (dependency_types[curr_tx][dependency]=='w'
                        ||(dependency_types[curr_tx][dependency]=='r'
                        && dependency_types[prev_tx][dependency]=='w'))){
                            /** dependency found **/
                            /** Add an edge prev->curr **/

                            dag[prev_tx].insert(curr_tx);
                            // HOTSTUFF_LOG_DEBUG("added edge %.10s -> %.10s", get_hex(prev_tx).c_str(), get_hex(curr_tx).c_str());
                    }
                }
            }
        }

        HOTSTUFF_LOG_DEBUG("[[create_dag]] ends");
        return dag;
    }

    TransitiveReduction::TransitiveReduction(std::unordered_map<uint256_t, std::unordered_set<uint256_t>> graph){
        this->reduced_graph = graph;
    }

    std::unordered_map<uint256_t, std::unordered_set<uint256_t>> TransitiveReduction::reduce(){
        // u -> v ----> w
        // |            ^
        // ------------>^     
        for(auto g: reduced_graph){
            auto u = g.first;
            for(auto v: g.second){
                std::unordered_set<uint256_t> visited;
                visited.insert(v);
                reduce_by_dfs(u, v, visited);
            }
        }
        return reduced_graph;
    }

    void TransitiveReduction::reduce_by_dfs(uint256_t u, uint256_t intermediate_node, std::unordered_set<uint256_t>& visited){
        for(uint256_t w: reduced_graph[intermediate_node]){
            if(visited.count(w)==0){
                if(reduced_graph[u].count(w)>0){
                    /** redundant edge found : Remove from the graph**/
                    reduced_graph[u].erase(w);
                }
                visited.insert(w);
                reduce_by_dfs(u, w, visited);
            }
        }
    }

    WeaklyConnectedGraph::WeaklyConnectedGraph(std::unordered_map<uint256_t, std::unordered_set<uint256_t>>& graph) {
        this->graph = graph;
    }

    std::vector<std::vector<uint256_t>> WeaklyConnectedGraph::get_wcc() {
        create_undirected_graph();

        /** Traverse undirected graph and find the connected components **/
        std::vector<std::vector<uint256_t>> wcc;
        for(auto g: undirected_graph){
            auto node = g.first;
            if(visited.count(node)==0){
                visited.insert(node);
                std::vector<uint256_t> component;
                dfs(node, component);
                wcc.push_back(component);
            }
        }

        return wcc;
    }

    void WeaklyConnectedGraph::create_undirected_graph(){
        for(auto g: graph){
            undirected_graph[g.first] = std::unordered_set<uint256_t>();
        }

        for(auto g: graph){
            auto from = g.first;
            for(auto to: g.second){
                undirected_graph[from].insert(to);
                undirected_graph[to].insert(from);
            }
        }
    }

    void WeaklyConnectedGraph::dfs(uint256_t node, std::vector<uint256_t>& component){
        component.push_back(node);
        for(auto child: undirected_graph[node]){
            if(visited.count(child)==0){
                visited.insert(child);
                dfs(child, component);
            }
        }
    }

    GraphFormatConversion::GraphFormatConversion(){

    }

    void GraphFormatConversion::serialize(DataStream &s, const std::unordered_map<uint256_t, std::unordered_set<uint256_t>> &graph){
        HOTSTUFF_LOG_DEBUG("[[serialize]] G size = %ld", graph.size());
        s << htole((uint32_t)graph.size());

        /* store all the graph nodes into a vector */
        std::vector<uint256_t> keys;
        for (auto g: graph){
            keys.push_back(g.first);
        }

        /* Sort all the nodes of graph */
        std::sort(keys.begin(), keys.end(), 
            [](const uint256_t & a, const uint256_t & b) { return a.operator<(b);});

        /* Create a node_to_idx hash map */
        std::unordered_map<uint256_t, size_t> node_to_idx;
        for(size_t idx=0; idx<graph.size(); idx++){
            node_to_idx[keys[idx]] = idx;
        }

        /* Adjacency-list encoding: for each vertex, emit its out-degree
         * followed by the indices of its neighbors. Wire size is
         * O(V + E) instead of the old bitmap matrix O(V^2/64) words,
         * which exploded to multiple MB per block at V >= 4000
         * (n >= 21 node-scaling runs). */
        for (auto const &key: keys){
            const auto &neighbors = graph.at(key);
            /* Count in-graph neighbors FIRST: the encoded degree must equal
             * the number of written indices or the stream desyncs. */
            std::vector<uint32_t> out_idx;
            out_idx.reserve(neighbors.size());
            for (auto const &neighbor: neighbors){
                auto nit = node_to_idx.find(neighbor);
                if (nit == node_to_idx.end())
                    continue; /* foreign entry: not encodable */
                out_idx.push_back((uint32_t)nit->second);
            }
            s << key;
            s << htole((uint32_t)out_idx.size());
            /* Canonical byte order: identical logical graphs must produce
             * identical bytes on every replica (unordered_set iteration
             * order is not deterministic across processes). */
            std::sort(out_idx.begin(), out_idx.end());
            for (uint32_t idx : out_idx)
                s << htole(idx);
        }
    }

    void GraphFormatConversion::unserialize(DataStream &s, std::unordered_map<uint256_t, std::unordered_set<uint256_t>> &graph){
        uint32_t n;
        s >> n;
        n = letoh(n);
        HOTSTUFF_LOG_DEBUG("[[unserialize]] G size = %ld", n);

        /* Adjacency-list decoding: pass 1 reads each vertex key and its
         * neighbor-index list; pass 2 resolves indices back to keys. */
        std::vector<uint256_t> keys(n);
        std::vector<std::vector<uint32_t>> adj(n);
        for (size_t key_i = 0; key_i < n; key_i++){
            uint32_t deg;
            s >> keys[key_i];
            s >> deg;
            deg = letoh(deg);
            adj[key_i].reserve(deg);
            for (uint32_t e = 0; e < deg; e++){
                uint32_t idx;
                s >> idx;
                adj[key_i].push_back(letoh(idx));
            }
        }

        for (size_t key_i = 0; key_i < n; key_i++){
            auto &out = graph[keys[key_i]];
            out.clear();
            for (uint32_t idx : adj[key_i]){
                if ((size_t)idx < n)
                    out.insert(keys[idx]);
            }
        }
    }

    GraphOperation::GraphOperation(std::unordered_map<uint256_t, std::unordered_set<uint256_t>> graph){
        this->graph = graph;
        this->leaf_count=0;

        for(auto g: graph){
            n_incoming[g.first] = 0; 
        }
        
        for(auto g: graph){
            if(g.second.empty()){
                leaf_count++;
                continue;
            }
            for(auto child: g.second){
                n_incoming[child]++; 
            }  
        }
        
        for(auto n_inc: n_incoming){
            if(n_inc.second==0){
                roots.insert(n_inc.first);
            }   
        }
    }

    std::unordered_set<uint256_t> GraphOperation::get_roots(){
        return this->roots;
    }

    std::unordered_map<salticidae::uint256_t, size_t> GraphOperation::get_incoming_count(){
        return this->n_incoming;
    }

    size_t GraphOperation::get_leaf_count(){
        return this->leaf_count;
    }
}
