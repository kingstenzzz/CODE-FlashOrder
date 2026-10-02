/**
 * Evil Client for injecting Condorcet cycles
 */

#include <cassert>
#include <random>
#include <memory>
#include <signal.h>
#include <sys/time.h>
#include <iostream>

#include "salticidae/type.h"
#include "salticidae/netaddr.h"
#include "salticidae/network.h"
#include "salticidae/util.h"

#include <numeric>
#include <fstream>
#include "hotstuff/util.h"
#include "hotstuff/type.h"
#include "hotstuff/client.h"
#include "small_bank.h"

using salticidae::Config;
using salticidae::get_hex10;

using hotstuff::ReplicaID;
using hotstuff::NetAddr;
using hotstuff::EventContext;
using hotstuff::MsgReqCmd;
using hotstuff::MsgRespCmd;
using hotstuff::CommandDummy;
using hotstuff::HotStuffError;
using hotstuff::uint256_t;
using hotstuff::opcode_t;
using hotstuff::command_t;

EventContext ec;
size_t max_async_num;
int max_iter_num;
uint32_t cid;
uint32_t cnt = 0;
uint32_t nfaulty;

struct Request {
    command_t cmd;
    size_t confirmed;
    salticidae::ElapsedTime et;
    Request(const command_t &cmd): cmd(cmd), confirmed(0) { et.start(); }
};

using Net = salticidae::MsgNetwork<opcode_t>;

std::unordered_map<ReplicaID, Net::conn_t> conns;
std::unordered_map<const uint256_t, Request> waiting;
std::vector<NetAddr> replicas;
std::vector<std::pair<struct timeval, double>> elapsed;
std::vector<double> confirm_lat_sec;
std::unique_ptr<Net> mn;
SmallBankManager *small_bank_manager;
double time_consumed_in_cmd_generation = 0.0;

int cycles_to_inject = 0;
int current_cycle = 0;
bool rot_mode_rank = false;
int cycle_len = 3; // [R2-D5] transactions per Condorcet cycle (3/10/20)
// macro (envelope) mode: spine + unequal groups, per-replica block rotation
bool macro_mode = false;
bool macro_no_rotate = false;
int macro_spine = 0;
std::vector<size_t> macro_group_sizes;
// explicit-rows mode: replica i receives an exact permutation (strict-trigger)
bool rows_mode = false;
std::vector<std::vector<size_t>> explicit_rows;

void connect_all() {
    for (size_t i = 0; i < replicas.size(); i++)
        conns.insert(std::make_pair(i, mn->connect_sync(replicas[i])));
}

bool try_send(bool check = true) {
    if ((!check || waiting.size() < max_async_num) && current_cycle < cycles_to_inject)
    {
        /* [R2-D5] generalized Condorcet cycle of cycle_len txs:
         * split replicas into 3 equal groups; group g observes the tx
         * sequence rotated by g*offset, creating a majority cycle. */
        const size_t L = (size_t)cycle_len;
        std::vector<command_t> cmds;
        std::vector<MsgReqCmd> msgs;
        for (size_t t = 0; t < L; t++) {
            auto tx = small_bank_manager->get_next_transaction_serialized();
            auto cmd = new CommandDummy(cid, cnt++, tx);
            cmds.push_back(cmd);
            msgs.emplace_back(*cmd);
        }
        const size_t n = replicas.size();
        if (rows_mode) {
            // strict-trigger workload: replica i receives explicit_rows[i] order
            const size_t m = explicit_rows[0].size();
            std::vector<command_t> cmds;
            std::vector<MsgReqCmd> msgs;
            for (size_t t = 0; t < m; t++) {
                auto tx = small_bank_manager->get_next_transaction_serialized();
                auto cmd = new CommandDummy(cid, cnt++, tx);
                cmds.push_back(cmd);
                msgs.emplace_back(*cmd);
            }
            for (size_t i = 0; i < n; i++) {
                for (size_t p = 0; p < m; p++)
                    mn->send_msg(msgs[explicit_rows[i][p]], conns[i]);
            }
            for (auto &c : cmds)
                waiting.insert(std::make_pair(c->get_hash(), Request(c)));
            current_cycle++;
            {   // debug: tx hashes in injection order (tx index = id)
                std::string hs;
                for (auto &c : cmds)
                    hs += (hs.empty() ? "" : ",") + get_hex(c->get_hash());
                HOTSTUFF_LOG_INFO("[ROWS_DBG] m=%zu txs=%s", m, hs.c_str());
            }
            return true;
        }
        if (macro_mode) {
            /* envelope workload: spine txs + L unequal groups; replica i
             * observes group blocks rotated by k = i % L (or 0 = control). */
            const size_t L = macro_group_sizes.size();
            size_t total = (size_t)macro_spine;
            for (auto g : macro_group_sizes) total += g;
            std::vector<command_t> cmds;      // injection order: spine then groups
            std::vector<MsgReqCmd> msgs;
            for (size_t t = 0; t < total; t++) {
                auto tx = small_bank_manager->get_next_transaction_serialized();
                auto cmd = new CommandDummy(cid, cnt++, tx);
                cmds.push_back(cmd);
                msgs.emplace_back(*cmd);
            }
            std::vector<std::pair<size_t,size_t>> blocks; // [begin,end) in cmds
            size_t off = (size_t)macro_spine;
            for (size_t i = 0; i < L; i++) { blocks.push_back({off, off + macro_group_sizes[i]}); off += macro_group_sizes[i]; }
            for (size_t i = 0; i < n; i++) {
                size_t k = macro_no_rotate ? 0 : i % L;
                for (size_t t = 0; t < (size_t)macro_spine; t++)
                    mn->send_msg(msgs[t], conns[i]);
                for (size_t b = 0; b < L; b++) {
                    auto &blk = blocks[(b + k) % L];
                    for (size_t t = blk.first; t < blk.second; t++)
                        mn->send_msg(msgs[t], conns[i]);
                }
            }
            for (auto &c : cmds)
                waiting.insert(std::make_pair(c->get_hash(), Request(c)));
            current_cycle++;
            {   // envelope debug: spine + group hashes in injection order.
                // chunked (15 per line) so concurrent log writes stay atomic
                auto dump = [&](const char *kind, int gi, size_t from, size_t to) {
                    const size_t cnt = to - from;
                    const size_t np = (cnt + 14) / 15;
                    for (size_t off = from, part = 0; off < to; off += 15, part++) {
                        std::string hs;
                        for (size_t t = off; t < std::min(to, off + 15); t++)
                            hs += (t == off ? "" : ",") + get_hex(cmds[t]->get_hash());
                        if (gi < 0)
                            HOTSTUFF_LOG_INFO("[ENVELOPE_DBG] spine part=%zu/%zu %s",
                                              part, np, hs.c_str());
                        else
                            HOTSTUFF_LOG_INFO("[ENVELOPE_DBG] group=%d size=%zu part=%zu/%zu %s",
                                              gi, macro_group_sizes[gi], part, np, hs.c_str());
                    }
                };
                dump("spine", -1, 0, (size_t)macro_spine);
                for (size_t i = 0; i < L; i++)
                    dump("group", (int)i, blocks[i].first, blocks[i].second);
            }
            return true;
        }
        size_t g1 = n / 3, g2 = 2 * n / 3;
        size_t step = std::max<size_t>(1, L / 3);
        for (size_t i = 0; i < n; i++) {
            /* rot-mode rank: HC-20 -- replica i rotates by i%L;
             * rot-mode group: original symmetric 3-group */
            size_t rot;
            if (rot_mode_rank) { rot = i % L; (void)g1; (void)g2; (void)step; }
            else rot = (i < g1) ? 0 : ((i < g2) ? step : 2 * step % L);
            for (size_t t = 0; t < L; t++)
                mn->send_msg(msgs[(t + rot) % L], conns[i]);
        }
        for (size_t t = 0; t < L; t++)
            waiting.insert(std::make_pair(cmds[t]->get_hash(), Request(cmds[t])));
        current_cycle++;
        if (L <= 20) {   // witness debug: tx hashes in injection order
            std::string hs;
            for (size_t t = 0; t < L; t++)
                hs += (hs.empty() ? "" : ",") + get_hex10(cmds[t]->get_hash());
            HOTSTUFF_LOG_INFO("[CYCLE_DBG] cycle=%d len=%zu txs=%s",
                              current_cycle, L, hs.c_str());
        }
        if (current_cycle % 10 == 0) {
            HOTSTUFF_LOG_INFO("Injected %d cycles...", current_cycle);
        }
        return true;
    }
    return false;
}

void client_resp_cmd_handler(MsgRespCmd &&msg, const Net::conn_t &) {
    auto &fin = msg.fin;
    const uint256_t &cmd_hash = fin.cmd_hash;
    auto it = waiting.find(cmd_hash);
    if (it == waiting.end()) return;
    auto &et = it->second.et;
    et.stop();
    if (++it->second.confirmed <= nfaulty) return; // wait for f + 1 ack
    confirm_lat_sec.push_back(it->second.et.elapsed_sec);
    waiting.erase(it);
    
    if (waiting.empty() && current_cycle >= cycles_to_inject) {
        if (!confirm_lat_sec.empty()) {
            auto v = confirm_lat_sec;
            std::sort(v.begin(), v.end());
            const double p50 = v[v.size() / 2] * 1000.0;
            const double p95 = v[(size_t)(0.95 * (v.size() - 1))] * 1000.0;
            const double avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size() * 1000.0;
            HOTSTUFF_LOG_INFO("[CLIENT_LAT] n=%zu p50_ms=%.2f p95_ms=%.2f avg_ms=%.2f",
                              v.size(), p50, p95, avg);
        }
        HOTSTUFF_LOG_INFO("All cycles confirmed. Exiting.");
        ec.stop();
    } else {
        while (try_send());
    }
}

std::pair<std::string, std::string> split_ip_port_cport(const std::string &s) {
    auto ret = salticidae::trim_all(salticidae::split(s, ";"));
    return std::make_pair(ret[0], ret[1]);
}

int main(int argc, char **argv) {
    Config config("hotstuff.conf");

    auto opt_sb_users = Config::OptValInt::create(10);
    auto opt_sb_prob_choose_mtx = Config::OptValDouble::create(0.9);
    auto opt_sb_skew_factor = Config::OptValDouble::create(0.1);
    auto opt_fairness_parameter = Config::OptValDouble::create(1);  // Themis
    auto opt_idx = Config::OptValInt::create(0);
    auto opt_replicas = Config::OptValStrVec::create();
    auto opt_max_iter_num = Config::OptValInt::create(100);
    auto opt_max_async_num = Config::OptValInt::create(10);
    auto opt_cid = Config::OptValInt::create(-1);
    auto opt_max_cli_msg = Config::OptValInt::create(65536); // 64K by default
    auto opt_cycles = Config::OptValInt::create(100);

    auto shutdown = [&](int) { ec.stop(); };
    salticidae::SigEvent ev_sigint(ec, shutdown);
    salticidae::SigEvent ev_sigterm(ec, shutdown);
    ev_sigint.add(SIGINT);
    ev_sigterm.add(SIGTERM);

    mn = std::make_unique<Net>(ec, Net::Config().max_msg_size(opt_max_cli_msg->get()));
    mn->reg_handler(client_resp_cmd_handler);
    mn->start();

    config.add_opt("sb-users", opt_sb_users, Config::SET_VAL);
    config.add_opt("sb-prob-choose_mtx", opt_sb_prob_choose_mtx, Config::SET_VAL);
    config.add_opt("sb-skew-factor", opt_sb_skew_factor, Config::SET_VAL);
    config.add_opt("fairness-parameter", opt_fairness_parameter, Config::SET_VAL);  // Themis
    config.add_opt("idx", opt_idx, Config::SET_VAL);
    config.add_opt("cid", opt_cid, Config::SET_VAL);
    config.add_opt("replica", opt_replicas, Config::APPEND);
    config.add_opt("iter", opt_max_iter_num, Config::SET_VAL);
    config.add_opt("max-async", opt_max_async_num, Config::SET_VAL);
    config.add_opt("max-cli-msg", opt_max_cli_msg, Config::SET_VAL, 'S', "the maximum client message size");
    config.add_opt("cycles", opt_cycles, Config::SET_VAL, 'C', "number of cycles to inject");
    auto opt_cycle_len = Config::OptValInt::create(3);
    config.add_opt("cycle-len", opt_cycle_len, Config::SET_VAL, 'L', "transactions per cycle (3/10/20)");
    auto opt_rot_mode = Config::OptValStr::create("group");
    config.add_opt("rot-mode", opt_rot_mode, Config::SET_VAL, 'R', "group (symmetric 3-group) or rank (HC-20 per-replica rotation)");
    auto opt_spine = Config::OptValInt::create(-1);
    config.add_opt("spine", opt_spine, Config::SET_VAL, 0, "macro mode: spine tx count (enables macro mode)");
    auto opt_group_sizes = Config::OptValStr::create("");
    config.add_opt("group-sizes", opt_group_sizes, Config::SET_VAL, 0, "macro mode: comma list of group sizes");
    auto opt_no_rotate = Config::OptValInt::create(0);
    config.add_opt("no-rotate", opt_no_rotate, Config::SET_VAL, 0, "macro mode control arm: all replicas get rotation 0");
    auto opt_rows_file = Config::OptValStr::create("");
    config.add_opt("rows-file", opt_rows_file, Config::SET_VAL, 0,
                   "explicit-rows mode: file with one permutation per replica");
    config.parse(argc, argv);
    
    auto idx = opt_idx->get();
    max_iter_num = opt_max_iter_num->get();
    max_async_num = opt_max_async_num->get();
    cycles_to_inject = opt_cycles->get();
    cycle_len = std::max(2, opt_cycle_len->get());
    rot_mode_rank = (opt_rot_mode->get() == "rank");
    if (opt_spine->get() >= 0 && !opt_group_sizes->get().empty()) {
        macro_mode = true;
        macro_spine = opt_spine->get();
        macro_no_rotate = opt_no_rotate->get() != 0;
        for (const auto &s : salticidae::split(opt_group_sizes->get(), ","))
            macro_group_sizes.push_back(std::stoul(salticidae::trim(s)));
        cycles_to_inject = 1;
    }
    if (!opt_rows_file->get().empty()) {
        rows_mode = true;
        cycles_to_inject = 1;
        std::ifstream fin(opt_rows_file->get());
        std::string line;
        while (std::getline(fin, line)) {
            std::vector<size_t> row;
            for (const auto &s : salticidae::split(line, " "))
                if (!salticidae::trim(s).empty())
                    row.push_back(std::stoul(salticidae::trim(s)));
            if (!row.empty())
                explicit_rows.push_back(row);
        }
    }
    
    std::vector<std::string> raw;
    for (const auto &s: opt_replicas->get())
    {
        auto res = salticidae::trim_all(salticidae::split(s, ","));
        if (res.size() < 1)
            throw HotStuffError("format error");
        raw.push_back(res[0]);
    }

    if (!(0 <= idx && (size_t)idx < raw.size() && raw.size() > 0))
        throw std::invalid_argument("out of range");
    cid = opt_cid->get() != -1 ? opt_cid->get() : idx;
    for (const auto &p: raw)
    {
        auto _p = split_ip_port_cport(p);
        size_t _;
        replicas.push_back(NetAddr(NetAddr(_p.first).ip, htons(stoi(_p.second, &_))));
    }

    double fairness_parameter = opt_fairness_parameter->get();
    nfaulty = (replicas.size() * ((2*fairness_parameter) -1))/4;
    HOTSTUFF_LOG_INFO("nfaulty = %zu, injecting %d cycles", nfaulty, cycles_to_inject);

    small_bank_manager = new SmallBankManager(opt_sb_users->get(), opt_sb_prob_choose_mtx->get(), opt_sb_skew_factor->get());

    connect_all();
    while (try_send());
    ec.dispatch();

    return 0;
}
