#pragma once
// Restricted SM5 colour-output fanout. Original arithmetic and texture reads
// are retained; the final colour is exported to the game, clean colour and
// scalar UI-influence targets. Unsupported programs decline without mutation.
// Token definitions: Microsoft DirectXShaderCompiler d3d12TokenizedProgramFormat.hpp.
#include <windows.h>
#include <vector>
#include <cstring>
#include <string>
#include <stdexcept>
void ComputeHashRetail(const BYTE*,UINT,BYTE*);
namespace edvr { namespace dxbc_fanout_detail {
static UINT word(const std::vector<BYTE>& b,size_t off){if(off>b.size()||b.size()-off<4)throw std::runtime_error("bounds");UINT n;memcpy(&n,b.data()+off,4);return n;}
static void put(std::vector<BYTE>& b,UINT v){auto p=(BYTE*)&v;b.insert(b.end(),p,p+4);}
static UINT instructionLength(const std::vector<UINT>& t,size_t p) {
    UINT len=(t[p]>>24)&127;
    // Immediate constant buffers carry their length in the next word,
    // followed by literal float4 data, not executable instructions.
    if((t[p]&0x7ff)==53) {
        if((t[p]>>11)!=3 || p+1>=t.size())throw std::runtime_error("unsupported custom data");
        len=t[p+1];
        if(len<2 || (len-2)%4)throw std::runtime_error("constant buffer length");
    }
    if(!len || len>t.size()-p)throw std::runtime_error("instruction length");
    return len;
}
static std::vector<BYTE> patch(const std::vector<BYTE>& in){
 if(in.size()>1024*1024||in.size()<32||word(in,28)>128||memcmp(in.data(),"DXBC",4)||word(in,24)!=in.size())throw std::runtime_error("container");
 BYTE digest[16];ComputeHashRetail(in.data()+20,UINT(in.size()-20),digest);if(memcmp(digest,in.data()+4,16))throw std::runtime_error("input checksum");
 std::vector<std::pair<UINT,std::vector<BYTE>>> chunks;
 bool sig=false,program=false;
 for(UINT i=0,n=word(in,28);i<n;++i){
   UINT off=word(in,32+4*i),tag=word(in,off),size=word(in,off+4);
   if(off+8>in.size()||size>in.size()-(off+8))throw std::runtime_error("chunk bounds");
   std::vector<BYTE> b(in.begin()+off+8,in.begin()+off+8+size);
   if(tag==0x4e47534f){
     if(sig||word(b,0)!=1||word(b,4)!=8||word(b,12)!=0||word(b,20)!=3||word(b,24)!=0||(word(b,28)&0xffff)!=15)throw std::runtime_error("output signature");
     UINT name=word(b,8);if(name>=b.size()||b.size()-name<10||_strnicmp((char*)b.data()+name,"SV_TARGET",9)||b[name+9])throw std::runtime_error("output name");
     std::vector<BYTE> s;put(s,3);put(s,8);
     for(UINT j=0;j<3;++j){put(s,80);put(s,j);put(s,word(b,16));put(s,3);put(s,j);put(s,word(b,28));}
     const char semantic[]="SV_TARGET";s.insert(s.end(),semantic,semantic+sizeof(semantic));while(s.size()%4)s.push_back(0xab);
     b=std::move(s);sig=true;
   } else if(tag==0x58454853||tag==0x52444853){
     if(program||b.size()%4||word(b,0)!=0x50||word(b,4)!=b.size()/4)throw std::runtime_error("program header");
     std::vector<UINT> t(b.size()/4);memcpy(t.data(),b.data(),b.size());UINT temp=~0u,decls=0,returns=0;
     for(size_t p=2;p<t.size();){UINT op=t[p]&0x7ff,len=instructionLength(t,p);
       if(op==104){if(temp!=~0u||len!=2||t[p+1]>=4096)throw std::runtime_error("temps");temp=t[p+1];}
       if(op==101){if(len!=3||t[p+1]!=0x001020f2||t[p+2]!=0)throw std::runtime_error("output declaration");++decls;}
       if(op==62){if(len!=1||p+1!=t.size())throw std::runtime_error("early return");++returns;}
       const bool supportedNew=(op>=108&&op<=111)||(op>=121&&op<=141)||op==161||op==162||op==165||op==167;
       if(op==4||op==5||op==44||op==63||op==102||op==103||op==38||op==77||op==78||op==81||op==132||op==133||(op>=109&&!supportedNew))throw std::runtime_error("unsupported opcode "+std::to_string(op));
       p+=len;
     }
     if(decls!=1||returns!=1)throw std::runtime_error("missing declarations");
     const bool addTemps=temp==~0u;if(addTemps)temp=0;
     std::vector<UINT> out{t[0],0};UINT mask=0;
     for(size_t p=2;p<t.size();){UINT op=t[p]&0x7ff,len=instructionLength(t,p);
       std::vector<UINT> inst(t.begin()+p,t.begin()+p+len);
       if(op==104)inst[1]=temp+1;
       if((op<88&&op!=62&&op!=53)||(op>=108&&op<=141)||op==165||op==167){
         size_t dest=1;while(dest<len&&(inst[dest-1]&0x80000000u))++dest;
         if(dest<len&&((inst[dest]>>12)&255)==2){
           if(op==38||op==77||op==78||op==81||dest+1>=len||(inst[dest]&~0xf0u)!=0x00102002u||inst[dest+1]!=0)throw std::runtime_error("output operand");
           mask|=(inst[dest]>>4)&15;inst[dest]&=~0x2000u;inst[dest+1]=temp;
         }
       }
       if(op==62){for(UINT r=0;r<3;++r){UINT mov[]={0x05000036,0x001020f2,r,r==2?0x00100ff6u:0x00100e46u,temp};out.insert(out.end(),mov,mov+5);}}
       out.insert(out.end(),inst.begin(),inst.end());
       if(op==101){for(UINT r=1;r<3;++r){UINT decl[]={0x03000065,0x001020f2,r};out.insert(out.end(),decl,decl+3);}if(addTemps){out.push_back(0x02000068);out.push_back(1);}}
       p+=len;
     }
     if(mask!=15)throw std::runtime_error("incomplete output");out[1]=UINT(out.size());b.resize(out.size()*4);memcpy(b.data(),out.data(),b.size());program=true;
   }
   chunks.push_back({tag,std::move(b)});
 }
 if(!sig||!program)throw std::runtime_error("missing chunks");
 std::vector<BYTE> out(in.begin(),in.begin()+32);out.resize(32+chunks.size()*4);UINT i=0;
 for(auto& c:chunks){UINT off=UINT(out.size());memcpy(out.data()+32+4*i++,&off,4);put(out,c.first);put(out,UINT(c.second.size()));out.insert(out.end(),c.second.begin(),c.second.end());}
 UINT sz=UINT(out.size());memcpy(out.data()+24,&sz,4);ComputeHashRetail(out.data()+20,sz-20,out.data()+4);return out;
}
} // namespace dxbc_fanout_detail
inline bool uiColourFanout(const void* data,size_t bytes,std::vector<BYTE>& output,std::string& reason) {
    output.clear();reason.clear();
    if(!data || bytes<32 || bytes>1024*1024){reason="bytecode size";return false;}
    try {
        const auto* p=static_cast<const BYTE*>(data);
        output=dxbc_fanout_detail::patch(std::vector<BYTE>(p,p+bytes));return true;
    } catch(const std::exception& e){reason=e.what();return false;}
}
} // namespace edvr
