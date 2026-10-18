#pragma once
#include <chrono>
#include <stdexcept>

// Authenticated request/reply preparation only; frame and chunk/object budgets remain separate.
namespace tcard_prepare {
using Clock=std::chrono::steady_clock;
struct Budget {
    Clock::time_point expires{};unsigned turns=0;bool active=false,waiting=false;
    void Check(Clock::time_point now,bool stopped)const
    {
        if(stopped||now>=expires)throw std::runtime_error("Authenticated preparation cancelled or timed out");
    }
    void Begin(Clock::time_point now,bool stopped)
    {
        if(stopped)throw std::runtime_error("Authenticated preparation cancelled or timed out");
        if(active||turns>=2)throw std::runtime_error("Unexpected authenticated preparation boundary");
        expires=now+std::chrono::seconds(60);active=true;waiting=false;++turns;
    }
    Clock::time_point Wait(Clock::time_point now,bool stopped)
    {
        if(!active)throw std::runtime_error("Unexpected authenticated preparation boundary");
        Check(now,stopped);waiting=true;return expires;
    }
    bool FirstRead(Clock::time_point now,bool stopped,Clock::time_point& frame)
    {
        if(!waiting)return false;Check(now,stopped);active=false;waiting=false;frame=now+std::chrono::seconds(10);return true;
    }
    bool Send(Clock::time_point now,bool stopped,Clock::time_point& frame)
    {
        if(!active)return false;Check(now,stopped);active=false;waiting=false;frame=now+std::chrono::seconds(10);return true;
    }
};
}
