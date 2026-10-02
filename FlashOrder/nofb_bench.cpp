// nofb_bench.cpp -- fallback 关闭时，什么条件触发环（SCC 聚合条件）
// 用 nofb_flashorder.h（plain Kahn + 尾部追加），Tarjan 仅检测。
// 场景 × γ × k_d 网格，报：multi>0 触发率（=本应聚合）、Kahn 卡死簇数、
// 尾部追加导致的 rank 位移（对比 fallback 版）。
#include <cstdio>
#include <cstdint>
#include <vector>
#include <map>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
#include "hotstuff/entity.h"
#include "nofb_flashorder.h"
using namespace hotstuff; using namespace FlashOrderNoFB;
static uint256_t make_txn_id(uint32_t i){bytearray_t raw(32,0);raw[0]=i&0xff;raw[1]=(i>>8)&0xff;raw[2]=(i>>16)&0xff;raw[3]=(i>>24)&0xff;return uint256_t(raw);}
static uint32_t txn_index(const uint256_t &x){bytearray_t ba=x;return (uint32_t)ba[0]|((uint32_t)ba[1]<<8)|((uint32_t)ba[2]<<16)|((uint32_t)ba[3]<<24);}
static void apply_adversarial(std::vector<int>&o,int hr,int,int lo,int W,int s){std::vector<int>w(o.begin()+lo,o.begin()+lo+W);int k=s%W;if(k<0)k+=W;if(hr%2==0)std::rotate(w.begin(),w.begin()+k,w.end());else std::rotate(w.begin(),w.end()-k,w.end());for(int i=0;i<W;i++)o[lo+i]=w[i];}

struct Stats{int calls=0,fb=0,maxs=0;long long stall_sum=0;double disp_sum=0;int disp_n=0;};

static Stats run(const std::string&sc,double gamma,double kd,int R,int n,int nrep,int f,int noise){
  Stats st;
  for(int rep=0;rep<R;rep++){
    std::mt19937 rng(1234u+rep*101);
    std::vector<int>rid(nrep);std::iota(rid.begin(),rid.end(),0);std::shuffle(rid.begin(),rid.end(),rng);
    std::vector<char>byz(nrep,0);for(int i=0;i<f;i++)byz[rid[i]]=1;
    std::vector<OrderedList> lists;int hs=0,ht=nrep-f,bidx=0;
    for(int r=0;r<nrep;r++){
      std::vector<int>o(n);std::iota(o.begin(),o.end(),0);
      if(byz[r]){
        if(sc=="heavy"){int W=std::max(4,n/4);int lo=(n-W)/2;std::vector<int>w(o.begin()+lo,o.begin()+lo+W);std::vector<int>rest;for(int i=0;i<n;i++)if(i<lo||i>=lo+W)rest.push_back(o[i]);for(int i=0;i<W;i++)o[i]=w[i];for(int i=0;i<(int)rest.size();i++)o[W+i]=rest[i];}
        else std::shuffle(o.begin(),o.end(),rng);
      }else{
        std::poisson_distribution<int>p(noise);
        int sw=p(rng);
        for(int s=0;s<sw;s++){std::uniform_int_distribution<int>u(0,n-2);int i=u(rng);std::swap(o[i],o[i+1]);}
        if(sc=="condorcet"){int W=std::max(4,n/4);int lo=(n-W)/2;apply_adversarial(o,hs++,ht,lo,W,std::max(1,W/3));}
        else if(sc=="burst"){int W=std::max(4,n/2);int lo=(n-W)/2;std::vector<int>w(o.begin()+lo,o.begin()+lo+W);std::shuffle(w.begin(),w.end(),rng);for(int i=0;i<W;i++)o[lo+i]=w[i];}
        else if(sc=="partial"){std::vector<int>keep;std::uniform_real_distribution<double>u(0,1);for(int i=0;i<n;i++)if(u(rng)>0.10)keep.push_back(i);if(keep.empty())keep.push_back(0);o=keep;}
      }
      OrderedList ol;ol.cmds.reserve(o.size());ol.timestamps.reserve(o.size());
      for(size_t i=0;i<o.size();i++){ol.cmds.push_back(make_txn_id((uint32_t)o[i]));ol.timestamps.push_back((uint64_t)i*100ULL);}
      lists.push_back(std::move(ol));
    }
    HyperGraph<10000> hg;
    auto fo=hg.finalize(lists,f,nrep,gamma,100.0,100.0,-1.0,kd);
    const auto&p=hg.get_profile();
    st.calls++;
    if(p.n_multi_sccs>0){st.fb++;st.maxs=std::max(st.maxs,p.max_scc_size);}
    st.stall_sum+=p.stalled_clusters;
    // rank displacement vs ground truth 0..n-1
    if(fo.size()) {
      std::vector<int> pos(n,-1);
      for(size_t i=0;i<fo.size();i++){uint32_t id=txn_index(fo[i]);if(id<(uint32_t)n)pos[id]=(int)i;}
      double d=0;int c=0;for(int i=0;i<n;i++)if(pos[i]>=0){d+=std::abs(pos[i]-i);c++;}
      if(c){st.disp_sum+=d/c;st.disp_n++;}
    }
  }
  return st;
}
int main(){
  int n=200,nrep=9,f=2,R=5;
  const char*scs[]={"baseline","heavy","byzshuffle","condorcet","burst","partial"};
  printf("scenario,gamma,k_d,noise,calls,scc_trigger_rate,max_scc,mean_stalled_clusters,mean_rank_disp\n");
  for(auto sc:scs)for(double g:{0.8,0.6})for(double kd:{0.0,1.0}){
    for(int noise:{5,50}){
      if(std::string(sc)=="baseline"&&noise==50)continue; // baseline 只跑 noise=5,跳过重复行
      if(std::string(sc)=="partial"&&noise==50)continue;  // partial 与 noise 无关
      Stats st=run(sc,g,kd,R,n,nrep,f,noise);
      printf("%s,%.1f,%.1f,%d,%d,%.2f,%d,%.1f,%.4f\n",sc,g,kd,noise,st.calls,
             (double)st.fb/st.calls,st.maxs,(double)st.stall_sum/st.calls,
             st.disp_n?st.disp_sum/st.disp_n/n:0.0);
    }
  }
}
