#pragma once
#include <algorithm>
#include <cstdint>

namespace tcard_sync {
struct ConnectionTiming {
    uint64_t idle=60000,reply=15000,retry=30000,maximum=300000,dead=600000,legacy=60000;
};
// Monotonic reachability only; no endpoint, key, operation or deletion expires here.
struct ConnectionRetry {
    uint64_t next=0,since=0;unsigned failures=0;bool disconnected=false,dead=false;
    void Reachable(){next=since=0;failures=0;disconnected=dead=false;}
    void Failed(uint64_t now,const ConnectionTiming& timing,uint32_t jitter=0){
        if(!disconnected){since=now;disconnected=true;}failures=(std::min)(failures+1,16u);
        const uint64_t base=(std::min)(timing.maximum,timing.retry*(uint64_t{1}<<(std::min)(failures-1,10u)));
        next=now+(std::min)(timing.maximum,base+jitter);
    }
    bool Dead(uint64_t now,const ConnectionTiming& timing){
        return dead=disconnected&&failures&&now>=since&&now-since>=timing.dead;
    }
};
}
