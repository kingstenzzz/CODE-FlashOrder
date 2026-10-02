// scenario_bench.cpp -- 还有哪些负载/场景会产生 multi-SCC（环 → SCC 聚合）
// 场景：
//   byzcyclic : 拜占庭副本发送"固定循环移位"序（i*(n/f)），非随机洗牌——
//               最小化作恶成本构造非传递多数
//   partial   : 诚实副本各自随机丢失 10% 交易（网络丢包/尾部延迟，部分可见性）
//   burst     : 诚实副本 ground truth 相同但热点窗口内本地到达序随机
//               （并发突发提交，时间戳几乎相同的真实 e2e 场景代理）
//   baseline  : 全诚实 + 小噪声（阴性对照）
// 每个 scenario × gamma{0.8,0.6} × k_d{0,1} 跑 R 次，报 multi>0 比例与 max_scc。
#include <cstdio>
#include <cstdint>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
#include <set>
#include "hotstuff/entity.h"
#include "hotstuff/flashorder.h"
using namespace hotstuff; using namespace FlashOrder;
static uint256_t make_txn_id(uint32_t i){bytearray_t raw(32,0);raw[0]=i&0xff;raw[1]=(i>>8)&0xff;raw[2]=(i>>16)&0xff;raw[3]=(i>>24)&0xff;return uint256_t(raw);}
struct Stats{int calls=0,fb=0,maxs=0;};

static void run(const std::string&sc,double gamma,double kd,int R,int n,int nrep,int f,Stats&st){
  for(int rep=0;rep<R;rep++){
    std::mt19937 rng(777u+rep*13);
    std::vector<int>rid(nrep);std::iota(rid.begin(),rid.end(),0);std::shuffle(rid.begin(),rid.end(),rng);
    std::vector<char>byz(nrep,0);for(int i=0;i<f;i++)byz[rid[i]]=1;
    std::vector<OrderedList> lists;int bidx=0;
    for(int r=0;r<nrep;r++){
      std::vector<int>o(n);std::iota(o.begin(),o.end(),0);
      if(byz[r]){
        if(sc=="byzcyclic"){int k=(bidx++)*(n/f);std::rotate(o.begin(),o.begin()+k%n,o.end());}
        else std::shuffle(o.begin(),o.end(),rng);
      }else{
        if(sc=="partial"){ // 随机丢 10%
          std::vector<int>keep;std::uniform_real_distribution<double>u(0,1);
          for(int i=0;i<n;i++)if(u(rng)>0.10)keep.push_back(i);if(keep.empty())keep.push_back(0);o=keep;
        }else if(sc=="burst"){ // 热点窗口 [lo,lo+W) 内随机排列（并发同时到达）
          int W=std::max(4,n/2);int lo=(n-W)/2;
          std::vector<int>w(o.begin()+lo,o.begin()+lo+W);std::shuffle(w.begin(),w.end(),rng);
          for(int i=0;i<W;i++)o[lo+i]=w[i];
        }else{ // baseline: 2 次相邻交换
          for(int s=0;s<2;s++){std::uniform_int_distribution<int>ud(0,n-2);int i=ud(rng);std::swap(o[i],o[i+1]);}
        }
      }
      OrderedList ol;ol.cmds.reserve(o.size());ol.timestamps.reserve(o.size());
      for(size_t i=0;i<o.size();i++){ol.cmds.push_back(make_txn_id((uint32_t)o[i]));ol.timestamps.push_back((uint64_t)i*100ULL);}
      lists.push_back(std::move(ol));
    }
    HyperGraph<10000> hg;
    auto fo=hg.finalize(lists,f,nrep,gamma,100.0,100.0,-1.0,kd);
    st.calls++;
    // 从 profile 拿 SCC 统计
    const auto&p=hg.get_profile();
    if(p.n_multi_sccs>0){st.fb++;st.maxs=std::max(st.maxs,p.max_scc_size);}
  }
}
int main(){
  int n=200,nrep=9,f=2,R=5;
  const char*scs[]={"baseline","byzcyclic","partial","burst"};
  printf("scenario,gamma,k_d,calls,fallback_calls,fallback_rate,max_scc_size\n");
  for(auto sc:scs)for(double g:{0.8,0.6})for(double kd:{0.0,1.0}){
    Stats st;run(sc,g,kd,R,n,nrep,f,st);
    printf("%s,%.1f,%.1f,%d,%d,%.2f,%d\n",sc,g,kd,st.calls,st.fb,(double)st.fb/st.calls,st.maxs);
  }
}
