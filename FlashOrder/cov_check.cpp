// cov_check.cpp -- fallback 覆盖率核查（condorcet k_d=0 是否丢交易）
// 复刻 kd_quality_bench.cpp 的 generate(condorcet) 逻辑，检查最终序 unique 覆盖。
#include <cstdio>
#include <cstdint>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <set>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"
using namespace hotstuff; using namespace FlashOrder;
static uint256_t make_txn_id(uint32_t i){bytearray_t raw(32,0);raw[0]=i&0xff;raw[1]=(i>>8)&0xff;raw[2]=(i>>16)&0xff;raw[3]=(i>>24)&0xff;return uint256_t(raw);}
static uint32_t txn_index(const uint256_t &x){bytearray_t ba=x;return (uint32_t)ba[0]|((uint32_t)ba[1]<<8)|((uint32_t)ba[2]<<16)|((uint32_t)ba[3]<<24);}
static void apply_adversarial(std::vector<int>&o,int hr,int,int lo,int W,int s){std::vector<int>w(o.begin()+lo,o.begin()+lo+W);int k=s%W;if(k<0)k+=W;if(hr%2==0)std::rotate(w.begin(),w.begin()+k,w.end());else std::rotate(w.begin(),w.end()-k,w.end());for(int i=0;i<W;i++)o[lo+i]=w[i];}
int main(){int n=200,nrep=9,f=2;for(int rep=0;rep<3;rep++){std::mt19937 rng(1234u+rep*101);std::vector<int>rid(nrep);std::iota(rid.begin(),rid.end(),0);std::shuffle(rid.begin(),rid.end(),rng);std::vector<char>byz(nrep,0);for(int i=0;i<f;i++)byz[rid[i]]=1;int hs=0,ht=nrep-f;std::vector<OrderedList> lists;
for(int r=0;r<nrep;r++){std::vector<int>o(n);std::iota(o.begin(),o.end(),0);if(byz[r]){std::shuffle(o.begin(),o.end(),rng);}else{std::poisson_distribution<int>p(5);int sw=p(rng);for(int s=0;s<sw;s++){std::uniform_int_distribution<int>u(0,n-2);int i=u(rng);std::swap(o[i],o[i+1]);}int W=std::max(4,n/4);int lo=(n-W)/2;apply_adversarial(o,hs++,ht,lo,W,std::max(1,W/3));}
OrderedList ol;ol.cmds.reserve(n);ol.timestamps.reserve(n);for(int i=0;i<n;i++){ol.cmds.push_back(make_txn_id((uint32_t)o[i]));ol.timestamps.push_back((uint64_t)i*100ULL);}lists.push_back(std::move(ol));}
for(double kd:{0.0,0.075,1.0}){HyperGraph<10000> hg;auto fo=hg.finalize(lists,f,nrep,0.8,100.0,100.0,-1.0,kd);std::set<uint32_t> uniq;for(auto&t:fo)uniq.insert(txn_index(t));printf("rep=%d kd=%.3f order_size=%zu unique=%zu (n=%d)\n",rep,kd,fo.size(),uniq.size(),n);}}}
