#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace edvr {
// Bounded, optional timestamps for sampled draw work. A busy ring skips a
// measurement; it never waits or flushes the game's command stream. Poll
// once per frame, at least four frames after submission. Failure/disjoint
// counts distinguish unavailable timestamps from a measured zero cost.
template<unsigned Capacity> class GpuIntervals {
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint,begin,end;
        uint64_t frame=0;
        bool pending=false;
    } slots_[Capacity];
    int open_=-1;
    uint64_t frame_=0;
    bool failed_=false;
public:
    struct Totals { double ms=0; unsigned samples=0,skipped=0,invalid=0; } totals;
    bool begin(ID3D11DeviceContext* ctx) {
        if(open_>=0 || failed_){++totals.skipped;return false;}
        for(unsigned i=0;i<Capacity;++i) {
            auto& s=slots_[i];if(s.pending)continue;
            if(!s.disjoint) {
                Microsoft::WRL::ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
                D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0},t{D3D11_QUERY_TIMESTAMP,0};
                if(FAILED(dev->CreateQuery(&d,&s.disjoint)) || FAILED(dev->CreateQuery(&t,&s.begin)) || FAILED(dev->CreateQuery(&t,&s.end))) {
                    s={};failed_=true;++totals.invalid;return false;
                }
            }
            ctx->Begin(s.disjoint.Get());ctx->End(s.begin.Get());open_=int(i);return true;
        }
        ++totals.skipped;return false;
    }
    void end(ID3D11DeviceContext* ctx) {
        if(open_<0)return;
        auto& s=slots_[open_];ctx->End(s.end.Get());ctx->End(s.disjoint.Get());
        s.pending=true;s.frame=frame_;open_=-1;
    }
    void poll(ID3D11DeviceContext* ctx) {
        ++frame_;
        for(auto& s:slots_) {
            if(!s.pending || frame_-s.frame<4)continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 a=0,b=0;
            const auto flags=D3D11_ASYNC_GETDATA_DONOTFLUSH;
            const HRESULT hd=ctx->GetData(s.disjoint.Get(),&d,sizeof(d),flags);
            if(hd==S_FALSE)continue;
            if(FAILED(hd) || d.Disjoint || !d.Frequency){s.pending=false;++totals.invalid;continue;}
            const HRESULT ha=ctx->GetData(s.begin.Get(),&a,sizeof(a),flags),hb=ctx->GetData(s.end.Get(),&b,sizeof(b),flags);
            if(ha==S_FALSE || hb==S_FALSE)continue;
            s.pending=false;
            if(FAILED(ha)||FAILED(hb)||b<a){++totals.invalid;continue;}
            totals.ms+=double(b-a)*1000.0/double(d.Frequency);++totals.samples;
        }
    }
};
}
