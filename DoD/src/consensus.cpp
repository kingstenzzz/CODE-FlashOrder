/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
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
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <stack>
#include <string>
#include <queue>
#include <vector>

#include "hotstuff/util.h"
#include "hotstuff/consensus.h"
#include "hotstuff/graph.h"
#include "hotstuff/stage_profiling.h"

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN
#define LOG_PROTO HOTSTUFF_LOG_PROTO

/* Sequential execution only: the parallel pool touched storage, timers and
 * the decision map from worker threads without synchronisation (heap
 * corruption under sustained load).  The sequential path runs entirely on
 * the event-loop thread and is race-free. */
#define PARALLEL_EXC false
#define THREAD_POOL_SIZE 20

namespace hotstuff {

template <typename T>
BlockingQueue<T>::BlockingQueue(){
    this->terminate = false;
}

template <typename T>
void BlockingQueue<T>::push(T const& value){
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        d_queue.push_front(value);
    }
    this->cond.notify_one();
}

template <typename T>
T BlockingQueue<T>::pop(){
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        this->cond.wait(lock, [=]{ return !this->d_queue.empty() || this->terminate; });
        if(this->terminate){
            uint8_t arr[] = {0};
            return uint256_t(arr);
        }
        T value(std::move(this->d_queue.back()));
        this->d_queue.pop_back();
        return value;
    }
}

/* Timed pop: returns false if the queue stayed empty for the full timeout.
 * Used by the ThreadPool as a liveness guard when the committed graph has
 * unreachable (cyclic) leftovers that would otherwise block workers
 * forever on an empty queue. */
template <typename T>
bool BlockingQueue<T>::pop_timeout(T &value, int timeout_ms){
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        if (!this->cond.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                [=]{ return !this->d_queue.empty() || this->terminate; }))
            return false;
        if(this->terminate){
            uint8_t arr[] = {0};
            value = uint256_t(arr);
            return true;
        }
        value = std::move(this->d_queue.back());
        this->d_queue.pop_back();
        return true;
    }
}

template <typename T>
bool BlockingQueue<T>::empty(){
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        return this->d_queue.empty();
    }
}

template <typename T>
void BlockingQueue<T>::do_terminate(){
    this->terminate = true;
    this->cond.notify_all();
}

template <typename T>
bool BlockingQueue<T>::termination_state(){
    return this->terminate;
}

ThreadPool::ThreadPool(size_t pool_size, std::unordered_map<salticidae::uint256_t, std::unordered_set<salticidae::uint256_t>> &graph, hotstuff::ReplicaID rid, uint32_t cmd_height, salticidae::uint256_t blk_hash, HotStuffCore *hsc){     // 
    this->rid = rid;
    this->cmd_height = cmd_height;
    this->blk_hash = blk_hash;
    this->hsc = hsc;

    this->pool_size = pool_size;
    this->graph = graph;

    GraphOperation *graph_opration = new GraphOperation(graph);
    this->n_incoming = graph_opration->get_incoming_count();
    for(auto n_inc: this->n_incoming){
        HOTSTUFF_LOG_DEBUG("[[ThreadPool]] n_incoming[%.10s] = %ld", get_hex(n_inc.first).c_str(), n_inc.second); 
    }
    
    
    std::unordered_set<uint256_t> roots = graph_opration->get_roots();
    for(auto root: roots){
        this->shared_queue.push(root);
    }
    HOTSTUFF_LOG_DEBUG("[[ThreadPool]] n roots = %ld", roots.size()); 

    this->leaf_count = graph_opration->get_leaf_count();
    HOTSTUFF_LOG_DEBUG("[[ThreadPool]] leaf_count = %ld", leaf_count); 
}

int ThreadPool::get_cmd_id(){
    return this->cmd_id++;
}

void ThreadPool::start_execute_parallel(){
    HOTSTUFF_LOG_DEBUG("[[start_execute_parallel]] Start"); 
    for(size_t i=0; i<this->pool_size; i++){
        std::thread *t = new std::thread(&ThreadPool::execute, this);
        this->threads.push_back(t);
    }
    for(auto t: this->threads){
        t->join();
        t->~thread();
    }
}

void ThreadPool::execute(){
    while(!this->shared_queue.termination_state()){
        uint256_t tx;
        if(!this->shared_queue.pop_timeout(tx, 500)){
            /* Queue idle for 500ms: every reachable node was executed and
             * either the graph is fully done or leftovers are cyclic /
             * unreachable.  Terminate the pool instead of blocking the
             * replica's event loop forever. */
            this->shared_queue.do_terminate();
            return;
        }

        if(this->shared_queue.termination_state()==true){
            return;
        }

        {
            // TODO: lock can be on individual tx instead of whole n_incoming queue
            std::unique_lock<std::mutex> lock(this->mtx_n_incoming);

            if(this->n_incoming[tx]>0){
                this->n_incoming[tx]--;
                if(this->n_incoming[tx]>0){
                    continue;
                }
            }
        }
        // execute
        this->hsc->do_decide(Finality(this->rid, 1, this->get_cmd_id(), this->cmd_height, tx, this->blk_hash));
        
        // {
        //     std::unique_lock<std::mutex> lock(this->mtx_storage);
        //     this->hsc->storage->remove_local_order_seen_execute_level(tx);
        //     this->hsc->storage->remove_from_proposed_cmds_cache(tx);
        // }
        this->hsc->storage->remove_local_order_seen_execute_level(tx);
        /* Keep the proposed-cache entry: executed commands must never
         * re-enter future proposals. The old unmark here let retransmitted
         * LOCAL-ORDERs re-include executed commands into later graphs
         * (~2.4x duplicate vertices/edges and duplicate executions). */
        // push child into the dequeue
        if(this->graph[tx].empty()){
            // this is a leaf node
            std::unique_lock<std::mutex> lock(this->mtx_leaf_count);
            this->leaf_count--;
            if(this->shared_queue.empty() && this->leaf_count==0){
                this->shared_queue.do_terminate();
                return;
            }
            continue;
        }

        for(auto child: this->graph[tx]){
            this->shared_queue.push(child);
        }
    }
}

/* The core logic of HotStuff, is fairly simple :). */
/*** begin HotStuff protocol logic ***/
HotStuffCore::HotStuffCore(ReplicaID id,
                            privkey_bt &&priv_key):
        b0(new Block(true, 1)),
        b_lock(b0),
        b_exec(b0),
        vheight(0),
        priv_key(std::move(priv_key)),
        tails{b0},
        vote_disabled(false),
        id(id),
        storage(new EntityStorage()) {
    storage->add_blk(b0);
}

void HotStuffCore::sanity_check_delivered(const block_t &blk) {
    if (!blk->delivered)
        throw std::runtime_error("block not delivered");
}

block_t HotStuffCore::get_delivered_blk(const uint256_t &blk_hash) {
    block_t blk = storage->find_blk(blk_hash);
    if (blk == nullptr || !blk->delivered)
        throw std::runtime_error("block not delivered");
    return blk;
}

bool HotStuffCore::on_deliver_blk(const block_t &blk) {
    if (blk->delivered)
    {
        LOG_WARN("attempt to deliver a block twice");
        return false;
    }
    blk->parents.clear();
    for (const auto &hash: blk->parent_hashes)
        blk->parents.push_back(get_delivered_blk(hash));
    blk->height = blk->parents[0]->height + 1;

    if (blk->qc)
    {
        block_t _blk = storage->find_blk(blk->qc->get_obj_hash());
        if (_blk == nullptr)
            throw std::runtime_error("block referred by qc not fetched");
        blk->qc_ref = std::move(_blk);
    } // otherwise blk->qc_ref remains null

    for (auto pblk: blk->parents) tails.erase(pblk);
    tails.insert(blk);

    blk->delivered = true;
    LOG_DEBUG("deliver %10s", std::string(*blk).c_str());

    return true;
}

void HotStuffCore::update_hqc(const block_t &_hqc, const quorum_cert_bt &qc) {
    if (_hqc->height > hqc.first->height)
    {
        hqc = std::make_pair(_hqc, qc->clone());
        on_hqc_update();
    }
}

// TODO: Themis
void HotStuffCore::update(const block_t &nblk) {
     // Themis
    /* Update missing edge cache */
    for(auto const &edge: nblk->get_e_update()) {
        storage->remove_missing_edge(edge.first, edge.second);
        for (auto bk = nblk; bk->height > b_exec->height; bk = bk->parents[0])
        { 
            bk->update_graph(edge);
        }
        HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Removing missing edge = %.10s -> %.10s", get_id(), get_hex(edge.first).c_str(), get_hex(edge.second).c_str());
    }
    for(auto const &edge: nblk->get_missing_edges()) {
        storage->add_missing_edge(edge.first, edge.second);
        HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Adding missing edge = %.10s -> %.10s", get_id(), get_hex(edge.first).c_str(), get_hex(edge.second).c_str());
    }
    /* Update proposal level local order cache */
    for(auto const &g: nblk->get_graph()) {
        storage->remove_local_order_seen_propose_level(g.first);
        // HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Removing Proposed cmd from seen = %.10s", get_id(), get_hex(g.first).c_str());
    }

    /* nblk = b*, blk2 = b'', blk1 = b', blk = b */
    HOTSTUFF_LOG_DEBUG("[[update Start]] [R-%d] [L-] new block = %.10s", get_id(),get_hex(nblk->get_hash()).c_str());
#ifndef HOTSTUFF_TWO_STEP
    /* three-step HotStuff */
    const block_t &blk2 = nblk->qc_ref;
    if (blk2 == nullptr) return;
    HOTSTUFF_LOG_DEBUG("[[update 1]] blk2 = %.10s, decision = %d, b0 = %.10s, decision = %d", get_hex(blk2->get_hash()).c_str(), blk2->decision, get_hex(b0->get_hash()).c_str(), b0->decision);
    /* decided blk could possible be incomplete due to pruning */
    if (blk2->decision) return;
    HOTSTUFF_LOG_DEBUG("[[update 2]]");
    update_hqc(blk2, nblk->qc);

    const block_t &blk1 = blk2->qc_ref;
    if (blk1 == nullptr) return;
    HOTSTUFF_LOG_DEBUG("[[update 3]]");
    if (blk1->decision) return;
    HOTSTUFF_LOG_DEBUG("[[update 4]]");
    if (blk1->height > b_lock->height) b_lock = blk1;

    const block_t &blk = blk1->qc_ref;
    if (blk == nullptr) return;
    HOTSTUFF_LOG_DEBUG("[[update 5]]");
    if (blk->decision) return;
    HOTSTUFF_LOG_DEBUG("[[update 6]]");

    /* commit requires direct parent */
    if (blk2->parents[0] != blk1 || blk1->parents[0] != blk) return;
    HOTSTUFF_LOG_DEBUG("[[update 7]]");
#else
    /* two-step HotStuff */
    const block_t &blk1 = nblk->qc_ref;
    if (blk1 == nullptr) return;
    if (blk1->decision) return;
    update_hqc(blk1, nblk->qc);
    if (blk1->height > b_lock->height) b_lock = blk1;

    const block_t &blk = blk1->qc_ref;
    if (blk == nullptr) return;
    if (blk->decision) return;

    /* commit requires direct parent */
    if (blk1->parents[0] != blk) return;
#endif
    /* b0 - - - - -> blk -> blk1 -> blk2 */
    /* otherwise commit */
    std::vector<block_t> commit_queue;
    block_t b;


// print_all_blocks(nblk, blk);


    for (b = blk; b->height > b_exec->height; b = b->parents[0])
    { /* TODO: also commit the uncles/aunts */
        commit_queue.push_back(b);
    }
    if (b != b_exec)
        throw std::runtime_error("safety breached :( " +
                                std::string(*blk) + " " +
                                std::string(*b_exec));

    HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Commit queue Size = %d", get_id(), commit_queue.size());
    auto _commit_t0 = STAGE_NOW();
    size_t _total_cmds = 0;
    for (auto it = commit_queue.rbegin(); it != commit_queue.rend(); it++)
    {
        const block_t &blk = *it;

        // Themis
        HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Graph Size = %d, block = %.10s", get_id(), blk->get_graph().size(), get_hex(blk->get_hash()).c_str());
        
        if(PARALLEL_EXC==true){
            auto order = fair_finalize(blk, nblk->get_e_update());
            HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Final Order Size = %d", get_id(), order.size());
            _total_cmds += order.size();
            if(order.empty() && !blk->get_graph().empty()) {
                /* this is not a tournament graph: stop looking at further blocks */
                HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Not a tournament Graph", get_id());
                break;
            }

             // Themis
            blk->decision = 1;
            do_consensus(blk);
            LOG_PROTO("commit %s", std::string(*blk).c_str());

             // Parallel Execution
             /* Guard: an empty order leaves the ThreadPool with no roots
              * and leaf_count == 0; its workers would block forever on
              * the empty queue and start_execute_parallel() joins them on
              * the event-loop thread, freezing the replica. */
             if (!order.empty())
             {
                 ThreadPool *thread_pool = new ThreadPool(THREAD_POOL_SIZE, order, id, blk->height, blk->get_hash(), this);
                 thread_pool->start_execute_parallel();
             }
         }
        else{
            auto order = fair_finalize_old(blk, nblk->get_e_update());
            HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Final Order Size = %d", get_id(), order.size());
            _total_cmds += order.size();
            if(order.empty() && !blk->get_graph().empty()) {
                /* this is not a tournament graph: stop looking at further blocks */
                HOTSTUFF_LOG_DEBUG("[[update]] [R-%d] [L-] Not a tournament Graph", get_id());
                break;
            }

            // Themis
            blk->decision = 1;
            do_consensus(blk);
            LOG_PROTO("commit %s", std::string(*blk).c_str());
                
            // Sequential Execution
            size_t n = order.size();
            for (size_t i=0; i<n; i++) {
                do_decide(Finality(id, 1, i, blk->height, order[i], blk->get_hash()));
                storage->remove_local_order_seen_execute_level(order[i]);
                /* Keep the proposed-cache entry (see ThreadPool::run):
                 * executed commands must never re-enter future graphs. */
            }
        }
        

        b_exec = blk;

        HOTSTUFF_LOG_DEBUG("[[update Decided]] [R-%d] [L-]", get_id());

        // blk->decision = 1;
        // do_consensus(blk);
        // LOG_PROTO("commit %s", std::string(*blk).c_str());
        // for (size_t i = 0; i < blk->cmds.size(); i++)
        //     do_decide(Finality(id, 1, i, blk->height,
        //                         blk->cmds[i], blk->get_hash()));
    }
    // b_exec = blk;                        
    STAGE_PROF("commit_exec", 0, _total_cmds, STAGE_US(_commit_t0));
    HOTSTUFF_LOG_DEBUG("[[update Ends]] [R-%d] [L-]", get_id());
}

// Themis
void HotStuffCore::print_all_blocks(const block_t &nblk, const block_t &blk){
    std::queue<block_t> parents_queue;
    parents_queue.push(nblk);

    // HOTSTUFF_LOG_INFO("[[print_all_blocks Start]] [R-%d] [L-] : Below:", get_id());
    while(!parents_queue.empty()){
        auto const &b = parents_queue.front();
        parents_queue.pop();

        for (auto const &pp_block: b->parents){
            HOTSTUFF_LOG_DEBUG("[[print_all_blocks]] [R-%d] [L-] hash = %.10s(%d) => %.10s(%d)", get_id(), get_hex(pp_block->get_hash()).c_str(), pp_block->get_height(), get_hex(b->get_hash()).c_str(), b->get_height());
            parents_queue.push(pp_block);
        }
        
    }
    // HOTSTUFF_LOG_INFO("[[print_all_blocks Ends]] [R-%d] [L-]", get_id());

    // HOTSTUFF_LOG_INFO("[[print_all_blocks]] [R-%d] [L-] new block = %.10s(%d), start block = %.10s(%d), b0 = %.10s(%d), b_exec = %.10s(%d)", 
    //                     get_id(), 
    //                     get_hex(nblk->get_hash()).c_str(), nblk->get_height(),
    //                     get_hex(blk->get_hash()).c_str(), blk->get_height(),
    //                     get_hex(b0->get_hash()).c_str(), b0->get_height(),
    //                     get_hex(b_exec->get_hash()).c_str(), b_exec->get_height());
}



// Themis
std::vector<uint256_t> HotStuffCore::
                        fair_finalize_old(block_t const &blk, 
                        std::vector<std::pair<uint256_t, uint256_t>> const &e_update){
    auto &graph = blk->get_graph();

    
    /** (1) For all Bi and transactions tx, tx0 in Bi that do not have an edge between them, 
     * if (tx; tx0) is in some Bj.e_update, then add that edge to Bi.G **/
    // for(auto const &edge: e_update){
    //     if(graph.count(edge.first)>0 && graph.count(edge.second)>0){
    //         /* nodes exists but edge does not exists: update these edges */
    //         blk->update_graph(edge);
    //     }
    // }

    /** (2) is graph B.G is a tournament **/
    if(!blk->is_weakly_connected()){
        /* Graph is not a tournament graph */
        return std::vector<uint256_t>();
    }

    /** (3) Compute the condensation graph of B.G, and topological sorting S **/
    CondensationGraph util_obj = CondensationGraph(blk->get_graph());
    auto const &sccs = util_obj.get_condensation_graph();
    /* Finalize a Global Order */
    /* TODO: Handle condorset cycles */
    std::vector<uint256_t> order;
    for (auto const &scc: sccs) {
        for (auto const &cmd: scc) {
            order.push_back(cmd);
        }
    }
    return order;
}

std::unordered_map<salticidae::uint256_t, std::unordered_set<salticidae::uint256_t>> HotStuffCore:: 
    fair_finalize(block_t const &blk, std::vector<std::pair<uint256_t, uint256_t>> const &e_update){
        
        auto &graph = blk->get_graph();
        auto finalize_start_ts = std::chrono::steady_clock::now();
         /** (2) is graph B.G is a tournament **/
        if(!blk->is_weakly_connected()){
            /* Graph is not a tournament graph */
            auto finalize_end_ts = std::chrono::steady_clock::now();
            double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                finalize_end_ts - finalize_start_ts).count() / 1000.0;
            HOTSTUFF_LOG_INFO("[perf] fair_finalize: graph_v=%zu result=NOT_WEAKLY_CONNECTED total_ms=%.3f",
                     graph.size(), total_ms);
            return std::unordered_map<salticidae::uint256_t, std::unordered_set<salticidae::uint256_t>>();
        }

        auto finalize_end_ts = std::chrono::steady_clock::now();
        double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            finalize_end_ts - finalize_start_ts).count() / 1000.0;
        HOTSTUFF_LOG_INFO("[perf] fair_finalize: graph_v=%zu total_ms=%.3f",
                 graph.size(), total_ms);

        return graph;
}

block_t HotStuffCore::on_propose(/* const std::vector<uint256_t> &cmds,*/               // Themis
                            const std::unordered_map<uint256_t, std::unordered_set<uint256_t>> &graph,
                            const std::vector<std::pair<uint256_t, uint256_t>> &e_missing,  // Rashnu
                            const std::vector<std::pair<uint256_t, uint256_t>> &e_update,
                            const std::vector<block_t> &parents,
                            bytearray_t &&extra) {
    auto _propose_t0 = STAGE_NOW();
    if (parents.empty())
        throw std::runtime_error("empty parents");
    for (const auto &_: parents) tails.erase(_);

    // /* Remove cmds from local order storage that are going to be proposed */
    // for(auto g: graph){
    //     storage->clear_ordered_hash_on_propose(g.first);
    // }
    // /* Store proposed commands */
    // for(auto g: graph){
    //     storage->add_to_proposed_cmds_cache(g.first);
    // }

    /* create the new block */
    block_t bnew = storage->add_blk(
        new Block(parents, /*cmds,*/ graph, e_missing, e_update,
            hqc.second->clone(), std::move(extra),
            parents[0]->height + 1,
            hqc.first,
            nullptr
        ));

// DataStream s1;
// bnew->serialize(s1);
// HOTSTUFF_LOG_WARN("[[on-propose]] Proposal size = %ld", s1.size());

// #ifdef HOTSTUFF_ENABLE_LOG_DEBUG
#ifdef NOTDEFINE
    if(bnew->get_qc_ref()==nullptr){
        HOTSTUFF_LOG_DEBUG("[[on_propose]] [R-%d] [L-]  block hqc = %.10s, Decision = %d", get_id(), "null", bnew->get_decision());
    }
    else{
        HOTSTUFF_LOG_DEBUG("[[on_propose]] [R-%d] [L-] block hqc = %.10s, Decision = %d", get_id(), get_hex(bnew->get_qc_ref()->get_hash()).c_str(), bnew->get_decision());
    }
#endif
    
    const uint256_t bnew_hash = bnew->get_hash();
    bnew->self_qc = create_quorum_cert(bnew_hash);
    on_deliver_blk(bnew);
    update(bnew);
    Proposal prop(id, bnew, nullptr);
    LOG_PROTO("propose %s", std::string(*bnew).c_str());
    if (bnew->height <= vheight)
        throw std::runtime_error("new block should be higher than vheight");
    /* self-receive the proposal (no need to send it through the network) */
    on_receive_proposal(prop);
    on_propose_(prop);
    /* boradcast to other replicas */

#ifdef NOTDEFINE
    print_block("on_propose", prop);
#endif

    do_broadcast_proposal(prop);
    STAGE_PROF("propose_gen", bnew->get_height(), 0, STAGE_US(_propose_t0));
    return bnew;
}

void HotStuffCore::print_block(std::string calling_method, const hotstuff::Proposal &prop){
#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
    for ( auto const &g: prop.blk->get_graph()) {
        HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] key = %s", calling_method.c_str(), get_id(), prop.proposer, get_hex(g.first).c_str());
        for (auto const &tx: g.second){
            HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] val = %s", calling_method.c_str(), get_id(), prop.proposer, get_hex(tx).c_str());
        }
    }
#endif

    DataStream s1;
    prop.blk->serialize(s1);
    HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] block (serialized) = %s", calling_method.c_str(), get_id(), prop.proposer, s1.get_hex().c_str());
    DataStream s2;
    s2 << *prop.blk;
    HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] block (salticidae raw) = %s", calling_method.c_str(), get_id(), prop.proposer, s2.get_hex().c_str());
    HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] block (salticidae raw hash) = %s", calling_method.c_str(), get_id(), prop.proposer, get_hex(s2.get_hash()).c_str());
    HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] block (salticidae hash) = %s", calling_method.c_str(), get_id(), prop.proposer, get_hex(salticidae::get_hash(*prop.blk)).c_str());
    HOTSTUFF_LOG_DEBUG("[[%s]] [R-%d] [L-%d] broadcasted block = %s", calling_method.c_str(), get_id(), prop.proposer, get_hex(prop.blk->get_hash()).c_str());
}

void HotStuffCore::on_receive_proposal(const Proposal &prop) {
    auto _vd_t0 = STAGE_NOW();
    LOG_PROTO("got %s", std::string(prop).c_str());
    bool self_prop = prop.proposer == get_id();
    block_t bnew = prop.blk;
    HOTSTUFF_LOG_DEBUG("[[on_receive_proposal]] [R-%d] [L-%d] broadcasted block Received = %.10s", get_id(), prop.proposer, get_hex(bnew->get_hash()).c_str());

    /* DoD: every replica records the proposal's graph vertices as proposed
     * so that (a) retransmitted LOCAL-ORDERs for already-proposed commands
     * are filtered at the leader, and (b) replicas can stop retransmitting
     * commands that have made it into any proposal. */
    for (auto const &g : bnew->get_graph())
        storage->add_to_proposed_cmds_cache(g.first);

#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
// #ifdef NOTDEFINE
    auto const &graph = bnew->get_graph();
    // for ( auto const &g: graph) {
    //     HOTSTUFF_LOG_DEBUG("[[on_receive_proposal Start]] [R-%d] [L-%d] key = %.10s", get_id(), prop.proposer, get_hex(g.first).c_str());
    //     for (auto const &tx: g.second){
    //         HOTSTUFF_LOG_DEBUG("[[on_receive_proposal Start]] [R-%d] [L-%d] val = %.10s", get_id(), prop.proposer, get_hex(tx).c_str());
    //     }
    // }
    HOTSTUFF_LOG_INFO("[[on_receive_proposal Start]] # Missing edges = %ld", bnew->get_missing_edges().size());
#endif

    if (!self_prop)
    {
        sanity_check_delivered(bnew);
        HOTSTUFF_LOG_DEBUG("[[on_receive_proposal Before Update]] [R-%d] [L-%d]", get_id(), prop.proposer);
        update(bnew);
        HOTSTUFF_LOG_DEBUG("[[on_receive_proposal After Update]] [R-%d] [L-%d]", get_id(), prop.proposer);
    }
    bool opinion = false;
    if (bnew->height > vheight)
    {
        if (bnew->qc_ref && bnew->qc_ref->height > b_lock->height)
        {
            opinion = true; // liveness condition
            vheight = bnew->height;
        }
        else
        {   // safety condition (extend the locked branch)
            block_t b;
            for (b = bnew;
                b->height > b_lock->height;
                b = b->parents[0]);
            if (b == b_lock) /* on the same branch */
            {
                opinion = true;
                vheight = bnew->height;
            }
        }
    }
    LOG_PROTO("now state: %s", std::string(*this).c_str());
    if (!self_prop && bnew->qc_ref)
        on_qc_finish(bnew->qc_ref);
    on_receive_proposal_(prop);
    if (opinion && !vote_disabled){
        HOTSTUFF_LOG_DEBUG("[[on_receive_proposal Start Vote]] [R-%d] [L-%d]", get_id(), prop.proposer);
        do_vote(prop.proposer,
            Vote(id, bnew->get_hash(),
                create_part_cert(*priv_key, bnew->get_hash()), this));
    }
    STAGE_PROF("vote_decision", prop.blk->get_height(), prop.blk->get_graph().size(), STAGE_US(_vd_t0));
        
}

void HotStuffCore::on_receive_vote(const Vote &vote) {
    LOG_PROTO("got %s", std::string(vote).c_str());
    LOG_PROTO("now state: %s", std::string(*this).c_str());
    block_t blk = get_delivered_blk(vote.blk_hash);
    assert(vote.cert);
    size_t qsize = blk->voted.size();
    if (qsize >= config.nmajority) return;
    if (!blk->voted.insert(vote.voter).second)
    {
        LOG_WARN("duplicate vote for %s from %d", get_hex10(vote.blk_hash).c_str(), vote.voter);
        return;
    }
    auto &qc = blk->self_qc;
    if (qc == nullptr)
    {
        LOG_WARN("vote for block not proposed by itself");
        qc = create_quorum_cert(blk->get_hash());
    }
    qc->add_part(vote.voter, *vote.cert);
    if (qsize + 1 == config.nmajority)
    {
        auto _vq_t0 = STAGE_NOW();
        qc->compute();
        update_hqc(blk, qc);
        on_qc_finish(blk);
        STAGE_PROF("vote_qc", blk->get_height(), 0, STAGE_US(_vq_t0));
    }
}

// Themis
void HotStuffCore::on_local_order (ReplicaID proposer, const std::vector<uint256_t> &order, bool is_reorder) {
    HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] [L-%d] START", get_id(), proposer);
    /** Add seen but Unproposed commands to the local order **/
    auto cmds = order;

    if(!is_reorder){
        // cmds = storage->get_unproposed_cmds();
        // cmds.insert(cmds.end(), order.begin(), order.end());
    }

    /** update seen edges **/
    storage->update_local_order_seen(cmds);

    /** identify previously missing and seen edges and update l_update **/
    auto const l_update = storage->get_updated_missing_edges();

#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
// #ifdef NOTDEFINE
    HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] [L-%d] l_update sent = ", get_id(), proposer);
    for(auto const &edge: l_update){
        HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] [L-%d] %.10s -> %.10s", get_id(), proposer, get_hex(edge.first).c_str(), get_hex(edge.second).c_str());
    }
#endif

    if(cmds.size()==0 && l_update.size()==0){
        /* Nothing to send to Leader */
        HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] [L-%d] Nothing to order", get_id(), proposer);
        return;
    }

    /** Creating Local order DAG **/
    LocalOrderDAG *local_order_dag = new LocalOrderDAG(cmds);
    for(auto cmd_hash: cmds){
        local_order_dag->add_dependency(cmd_hash, storage->get_cmd_dependency(cmd_hash));
    }
    auto dag = local_order_dag->create_dag();
    /** create LocalOrder struct Object **/
    LocalOrder local_order = LocalOrder(get_id(), dag, l_update, this);
    /** send local order to leader **/

#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
// #ifdef NOTDEFINE
    HOTSTUFF_LOG_DEBUG("[[on_local_order]] [R-%d] Creating Receive LocalOrder DAG", get_id());
    // for(auto const &g: local_order.ordered_dag){
    //     HOTSTUFF_LOG_DEBUG("[%.10s]", get_hex(g.first).c_str());
    //     for(auto const val: g.second){
    //         HOTSTUFF_LOG_DEBUG("%.10s", get_hex(val).c_str());
    //     }
    // }
    HOTSTUFF_LOG_DEBUG("============================");
#endif
    do_send_local_order(proposer, local_order);
}

// Themis
bool HotStuffCore::on_receive_local_order (const LocalOrder &local_order, const std::vector<block_t> &parents) {
    LOG_PROTO("got %s", std::string(local_order).c_str());
    LOG_PROTO("now state: %s", std::string(*this).c_str());
    
#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
// #ifdef NOTDEFINE
    HOTSTUFF_LOG_DEBUG("[[on_receive_local_order]] [fromR-%d] [thisL-%d] Receive LocalOrder DAG on Leade", local_order.initiator, get_id());
    // for(auto const &g: local_order.ordered_dag){
    //     HOTSTUFF_LOG_DEBUG("[%.10s]", get_hex(g.first).c_str());
    //     for(auto const val: g.second){
    //         HOTSTUFF_LOG_DEBUG("%.10s", get_hex(val).c_str());
    //     }
    // }
    HOTSTUFF_LOG_DEBUG("============================");
#endif    
    /** add new local order to the storage **/
    storage->add_local_order(local_order.initiator, local_order.ordered_dag, local_order.l_update);

    /** DoD: accumulate persistent observation weights (one count per
     *  distinct (replica, tx) / (replica, edge) pair).  This preserves the
     *  evidence of pruned-pending transactions across rounds and across
     *  replica retransmissions. */
    {
        auto &seen_tx = dod_tx_counted[local_order.initiator];
        for (auto const &g : local_order.ordered_dag)
        {
            if (seen_tx.insert(g.first).second)
                dod_tx_count[g.first]++;
            for (auto const &to : g.second)
            {
                if (seen_tx.insert(to).second)
                    dod_tx_count[to]++;
                if (dod_edge_counted.insert(std::make_pair(local_order.initiator,
                                                           std::make_pair(g.first, to))).second)
                    dod_edge_count[g.first][to]++;
            }
        }
    }

    /** Trigger FairPropose() and FairUpdate() **/
    if(storage->get_local_order_cache_size() >= config.nmajority){

        // Check if the commands in front of all the queues are proposed OR not
        std::vector<ReplicaID> replicas = storage->get_ordered_dag_replia_vector();
        for(ReplicaID replica: replicas){
            std::unordered_map<uint256_t, std::unordered_set<uint256_t>> unproposed_ordered_dag;
            auto ordered_dag = storage->get_ordered_dag(replica);
            for(auto &g: ordered_dag){
                auto cmd = g.first;
                if(!storage->is_cmd_proposed(cmd)){
                    unproposed_ordered_dag[cmd] = std::unordered_set<uint256_t>();
                }
            }
            if(unproposed_ordered_dag.size() < ordered_dag.size()){
                if(!unproposed_ordered_dag.empty()){
                    for(auto &g: ordered_dag){
                        auto from = g.first;
                        if(unproposed_ordered_dag.count(from)==0){
                            continue;
                        }
                        for(auto to: g.second){
                            if(unproposed_ordered_dag.count(to)==0){
                                /* Remove the proposed hashes (eg remove B) */
                                unproposed_ordered_dag[from].insert(ordered_dag[to].begin(), ordered_dag[to].end());
                            }
                            else{
                                /* Keep the unproposed hash linking */
                                unproposed_ordered_dag[from].insert(to);
                            }
                        }
                    }
                    storage->clear_front_ordered_dag(replica);
                    storage->add_ordered_dag_to_front(replica, unproposed_ordered_dag);
                }
                else{
                    storage->clear_front_ordered_dag(replica);
                } 
            }
        }


#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
// #ifdef NOTDEFINE
       for(auto const &replica: storage->get_ordered_dag_replia_vector()){
            HOTSTUFF_LOG_DEBUG("[[on_receive_local_order]] [fromR-%d] [thisL-%d] Global Order started", local_order.initiator, get_id());
            // for(auto const &g: storage->get_ordered_dag(replica)){
            //     HOTSTUFF_LOG_DEBUG("[%.10s]", get_hex(g.first).c_str());
            //     for(auto const val: g.second){
            //         HOTSTUFF_LOG_DEBUG("%.10s", get_hex(val).c_str());
            //     }
            // }
            HOTSTUFF_LOG_DEBUG("============================");
       }
#endif
        return storage->get_local_order_cache_size() >= config.nmajority;;
    }
    HOTSTUFF_LOG_DEBUG("[[on_receive_local_order]] [fromR-%d] [thisL-%d] No majority Found", local_order.initiator, get_id());
    return false;
}

// DoD-HS: assemble the global ordering evidence before proposing a HotStuff
// block.  The HotStuff QC authenticates the resulting graph; the graph itself
// is built from n-f distinct local-order messages exactly once per round.
std::pair<std::unordered_map<uint256_t, std::unordered_set<uint256_t>>, std::vector<std::pair<uint256_t, uint256_t>>> HotStuffCore::fair_propose() {
    HOTSTUFF_LOG_DEBUG("[[fairPropose START]] [R-%d]", get_id());
    auto fair_start_ts = std::chrono::steady_clock::now();
    auto _fair_t0 = STAGE_NOW();
    std::vector<ReplicaID> replicas = storage->get_ordered_dag_replia_vector();
    std::vector<std::unordered_map<uint256_t, std::unordered_set<uint256_t>>> local_graphs;
    local_graphs.reserve(replicas.size());
    for(ReplicaID replica: replicas){
        local_graphs.push_back(storage->get_ordered_dag(replica));
    }
    /* Retransmissions can put duplicate (already-proposed) commands back
     * into a replica's batch; strip them so each command is proposed and
     * executed exactly once.  Batched proposed-check: one lock for the
     * whole filter instead of one per node/edge. */
    {
        std::vector<uint256_t> probes;
        size_t est = 0;
        for (const auto &g : local_graphs) {
            est += g.size();
            for (const auto &row : g) est += row.second.size();
        }
        probes.reserve(est);
        for (const auto &g : local_graphs)
            for (const auto &row : g) {
                probes.push_back(row.first);
                for (const auto &to : row.second) probes.push_back(to);
            }
        const auto proposed = storage->filter_proposed_cmds(probes);
        auto is_prop = [&](const uint256_t &cmd) { return proposed.count(cmd) != 0; };
        for (auto &g : local_graphs)
        {
            for (auto it = g.begin(); it != g.end(); )
            {
                if (is_prop(it->first))
                {
                    it = g.erase(it);
                    continue;
                }
                for (auto nit = it->second.begin(); nit != it->second.end(); )
                {
                    if (is_prop(*nit))
                        nit = it->second.erase(nit);
                    else
                        ++nit;
                }
                ++it;
            }
        }
    }
    const auto topo_start_ts = std::chrono::steady_clock::now();
    /* Persistent weights remember every transaction ever observed; restrict
     * them to commands that have not been proposed yet so each proposal
     * only carries new commands and the graph stays bounded.  Batched
     * proposed-check: the cumulative edge map can hold thousands of entries
     * and a per-entry mutex round-trip dominated this path. */
    std::unordered_map<uint256_t, size_t> w_tx;
    std::unordered_map<uint256_t, std::unordered_map<uint256_t, size_t>> w_edge;
    {
        /* Probe only keys not already known proposed (monotone skip-set):
         * the cumulative maps grow forever, but proposed history is only
         * ever consulted once per command. */
        std::vector<uint256_t> probes;
        for (auto const &e : dod_tx_count)
            if (!dod_proposed_skip.count(e.first)) probes.push_back(e.first);
        for (auto const &e : dod_edge_count) {
            if (dod_proposed_skip.count(e.first)) continue;
            for (auto const &t : e.second)
                if (!dod_proposed_skip.count(t.first)) probes.push_back(t.first);
        }
        const auto proposed = storage->filter_proposed_cmds(probes);
        for (auto const &cmd : proposed) dod_proposed_skip.insert(cmd);
        /* One-time history compaction: erase proposed entries from the
         * carry-over maps. Proposed commands can never be re-proposed or
         * re-ordered (dedup is permanent), so their evidence is never
         * consulted again; erasing keeps per-round filtering proportional
         * to the live working set. Writers touch these maps only in
         * on_receive_local_order, which runs serially before fair_propose
         * on the same thread, so no concurrent modification. Each entry is
         * erased at most once (amortized O(1)). */
        for (auto it = dod_tx_count.begin(); it != dod_tx_count.end(); )
        {
            if (dod_proposed_skip.count(it->first)) it = dod_tx_count.erase(it);
            else ++it;
        }
        for (auto it = dod_edge_count.begin(); it != dod_edge_count.end(); )
        {
            if (dod_proposed_skip.count(it->first)) { it = dod_edge_count.erase(it); continue; }
            auto &inner = it->second;
            for (auto tit = inner.begin(); tit != inner.end(); )
            {
                if (dod_proposed_skip.count(tit->first)) tit = inner.erase(tit);
                else ++tit;
            }
            ++it;
        }
        for (auto const &e : dod_tx_count)
            w_tx.insert(e);
        for (auto const &e : dod_edge_count)
            for (auto const &t : e.second)
                w_edge[e.first][t.first] = t.second;
    }
    auto dod = build_dod_global_order(local_graphs,
                                      config.solid_tx_threshold,
                                      config.non_blank_tx_threshold,
                                      w_tx,
                                      w_edge);
    const auto topo_end_ts = std::chrono::steady_clock::now();

    for(ReplicaID replica: replicas){
        storage->clear_front_ordered_dag(replica);
    }

    HOTSTUFF_LOG_DEBUG("[[fairPropose END]] [R-%d]", get_id());

    auto fair_end_ts = std::chrono::steady_clock::now();
    const double topo_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            topo_end_ts - topo_start_ts).count() / 1000.0;
    const double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            fair_end_ts - fair_start_ts).count() / 1000.0;
    HOTSTUFF_LOG_INFO("[perf] dod_hs_global_order: replicas=%zu graph_v=%zu scc=%zu batches=%zu pruned=%zu missing=%zu topo_ms=%.3f total_ms=%.3f",
             replicas.size(), dod.graph.size(), dod.scc_count, dod.batches.size(),
             dod.pruned_pending, dod.missing_edges.size(), topo_ms, total_ms);
    STAGE_PROF("fairness", 0, 0, STAGE_US(_fair_t0));

    return std::make_pair(std::move(dod.graph), std::move(dod.missing_edges));
}

// Themis
std::vector<std::pair<uint256_t, uint256_t>> HotStuffCore::fair_update(){
     HOTSTUFF_LOG_DEBUG("[[fair_update START]] [R-%d]", get_id());
    auto update_start_ts = std::chrono::steady_clock::now();
    /** (1) get those replicas from which Leader has received their local order **/
    std::vector<ReplicaID> replicas = storage->get_l_update_replia_vector();

    /** (2) Create an empty e_update vector **/
    std::unordered_map<uint256_t,std::unordered_set<uint256_t>> e_update_map;
    std::vector<std::pair<uint256_t, uint256_t>> e_update;

    // TODO: Themis For all tx and tx' that are part of the same leader proposal ???
    /** (3)  **/
    std::unordered_map<uint256_t, std::unordered_map<uint256_t, uint16_t>> edge_count;
    /* Find edge count */
    
    for(auto const &replica: replicas){
        HOTSTUFF_LOG_DEBUG("[[fair_update]] [R-%d] [L-] l_update Received from replica (%d)= ", get_id(), replica);
        for (auto const &edge: storage->get_l_update_vector(replica)) {
            edge_count[edge.first][edge.second]++;
            HOTSTUFF_LOG_DEBUG("[[fair_update]] [R-%d] [L-] %.10s -> %.10s", get_id(), get_hex(edge.first).c_str(), get_hex(edge.second).c_str());
        }
    }
    /* add edges where k>=n(1-gama)+f+1 {>= tx_edge_threshold} */
    for(auto &from : edge_count){
        for(auto &to : from.second){
            uint256_t from_v = from.first;
            uint256_t to_v = to.first;
            uint16_t occurance = to.second;
            if(1.0 * occurance >= config.tx_edge_threshold
                && e_update_map[to_v].count(from_v)==0){ 
                    /* edge occurance is above threshold and no reverse edge is already present in graph */
                    e_update_map[from_v].insert(to_v);
                }
        }
    }


    /** (4) update and output e_update vector */
    for ( auto const &map: e_update_map) {
        auto const from_v = map.first;
        for (auto const to_v: map.second) {
            e_update.push_back(std::make_pair(from_v, to_v));
        }
    }

#ifdef HOTSTUFF_ENABLE_LOG_DEBUG
// #ifdef NOTDEFINE
    HOTSTUFF_LOG_DEBUG("[[fair_update]] [R-%d] [L-] e_update derived = ", get_id());
    for(auto const &edge: e_update){
        HOTSTUFF_LOG_DEBUG("[[fair_update]] [R-%d] [L-] %.10s -> %.10s", get_id(), get_hex(edge.first).c_str(), get_hex(edge.second).c_str());
    }
#endif
    
    HOTSTUFF_LOG_DEBUG("[[fair_update ENDS]] [R-%d]", get_id());

    for(auto const &replica: replicas){
        storage->clear_front_l_update(replica);
    }

    auto update_end_ts = std::chrono::steady_clock::now();
    const double update_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        update_end_ts - update_start_ts).count() / 1000.0;
    HOTSTUFF_LOG_INFO("[perf] fair_update: replicas=%zu edges=%zu total_ms=%.3f",
             replicas.size(), e_update.size(), update_ms);

    return e_update;
}

void HotStuffCore::reorder(ReplicaID proposer) {
    // TODO: Themis
    /** Get unordered cmds ??? **/

    HOTSTUFF_LOG_DEBUG("[[reorder]] [R-%d] invoked", get_id());
    /** Create Local Order **/
    on_local_order(proposer, std::vector<uint256_t>(), true);  
}

/*** end HotStuff protocol logic ***/
void HotStuffCore::on_init(uint32_t nfaulty, double fairness_parameter) {   // Themis
    config.nmajority = config.nreplicas - nfaulty;
    config.fairness_parameter = fairness_parameter;                 // Themis
    /** Do not switch below 3 statements with above 2 statements **/
    config.solid_tx_threshold = get_solid_tx_threshold();           // Themis
    config.non_blank_tx_threshold = get_non_blank_tx_threshold();   // Themis
    config.tx_edge_threshold = get_tx_edge_threshold();             // Themis
    HOTSTUFF_LOG_INFO("[[on_init]] [R-%d]  nmajority = %d, fairness_parameter = %f, solid_tx_threshold = %f, non_blank_tx_threshold = %f, tx_edge_threshold = %f", get_id(), config.nmajority, config.fairness_parameter, config.solid_tx_threshold, config.non_blank_tx_threshold, config.tx_edge_threshold);
    b0->qc = create_quorum_cert(b0->get_hash());
    b0->qc->compute();
    b0->self_qc = b0->qc->clone();
    b0->qc_ref = b0;
    hqc = std::make_pair(b0, b0->qc->clone());
}

// Themis
double HotStuffCore::get_solid_tx_threshold() {
    size_t nmajority = config.nmajority;
    size_t n = config.nreplicas;
    size_t f = n - nmajority;
    return n - 2.0*f;
}

// Themis
double HotStuffCore::get_non_blank_tx_threshold() {
    size_t nmajority = config.nmajority;
    size_t n = config.nreplicas;
    size_t f = n - nmajority;
    double gama = config.fairness_parameter;
    // DoD's pending threshold is independent of the fixed threshold.  A
    // transaction is included when it is fixed (n-2f observations) or when
    // it reaches this receive-order threshold; pending vertices are pruned by
    // the condensation-DAG reachability test in build_dod_global_order().
    return (n * (1.0-gama)) + f + 1.0;
}

// Themis
double HotStuffCore::get_tx_edge_threshold() {
    size_t nmajority = config.nmajority;
    size_t n = config.nreplicas;
    size_t f = n - nmajority;
    double gama = config.fairness_parameter;
    return (n * (1.0-gama)) + f + 1.0;
}

void HotStuffCore::prune(uint32_t staleness) {
    block_t start;
    /* skip the blocks */
    for (start = b_exec; staleness; staleness--, start = start->parents[0])
        if (!start->parents.size()) return;
    std::stack<block_t> s;
    start->qc_ref = nullptr;
    s.push(start);
    while (!s.empty())
    {
        auto &blk = s.top();
        if (blk->parents.empty())
        {
            storage->try_release_blk(blk);
            s.pop();
            continue;
        }
        blk->qc_ref = nullptr;
        s.push(blk->parents.back());
        blk->parents.pop_back();
    }
}

void HotStuffCore::add_replica(ReplicaID rid, const PeerId &peer_id,
                                pubkey_bt &&pub_key) {
    config.add_replica(rid,
            ReplicaInfo(rid, peer_id, std::move(pub_key)));
    b0->voted.insert(rid);
}

promise_t HotStuffCore::async_qc_finish(const block_t &blk) {
    if (blk->voted.size() >= config.nmajority)
        return promise_t([](promise_t &pm) {
            pm.resolve();
        });
    auto it = qc_waiting.find(blk);
    if (it == qc_waiting.end())
        it = qc_waiting.insert(std::make_pair(blk, promise_t())).first;
    return it->second;
}

void HotStuffCore::on_qc_finish(const block_t &blk) {
    auto it = qc_waiting.find(blk);
    if (it != qc_waiting.end())
    {
        it->second.resolve();
        qc_waiting.erase(it);
    }
}

promise_t HotStuffCore::async_wait_proposal() {
    return propose_waiting.then([](const Proposal &prop) {
        return prop;
    });
}

promise_t HotStuffCore::async_wait_receive_proposal() {
    return receive_proposal_waiting.then([](const Proposal &prop) {
        return prop;
    });
}

promise_t HotStuffCore::async_hqc_update() {
    return hqc_update_waiting.then([this]() {
        return hqc.first;
    });
}

void HotStuffCore::on_propose_(const Proposal &prop) {
    auto t = std::move(propose_waiting);
    propose_waiting = promise_t();
    t.resolve(prop);
}

void HotStuffCore::on_receive_proposal_(const Proposal &prop) {
    auto t = std::move(receive_proposal_waiting);
    receive_proposal_waiting = promise_t();
    t.resolve(prop);
}

void HotStuffCore::on_hqc_update() {
    auto t = std::move(hqc_update_waiting);
    hqc_update_waiting = promise_t();
    t.resolve();
}

HotStuffCore::operator std::string () const {
    DataStream s;
    s << "<hotstuff "
      << "hqc=" << get_hex10(hqc.first->get_hash()) << " "
      << "hqc.height=" << std::to_string(hqc.first->height) << " "
      << "b_lock=" << get_hex10(b_lock->get_hash()) << " "
      << "b_exec=" << get_hex10(b_exec->get_hash()) << " "
      << "vheight=" << std::to_string(vheight) << " "
      << "tails=" << std::to_string(tails.size()) << ">";
    return s;
}

}
