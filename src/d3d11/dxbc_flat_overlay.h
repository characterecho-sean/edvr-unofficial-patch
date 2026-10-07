#pragma once

// Add one private raster-coverage export to a pure SM5 pixel shader. The
// original instructions and outputs are copied byte for byte. A fragment
// discarded by the original shader never reaches the added MOV before RET.
#include "dxbc_container.h"
#include "flat_shader_classifier.h" // shared bounds-checked DXBC operand decoder
#include <cstdint>
#include <string>
#include <vector>

namespace edvr {
constexpr uint32_t kFlatOverlayTarget = 7;

inline bool flatOverlayPatchPs(const void* bytecode, size_t bytes,
                               std::vector<BYTE>& patched, std::string& reason) {
    using namespace dxbc_container;
    patched.clear();
    reason.clear();
    try {
        auto chunks = parseContainer(bytecode, bytes, 0x00000050u);
        bool outputSignature = false, program = false;
        bool emptyOutput = false, forceEarlyDepthStencil = false;
        for (auto& chunk : chunks) {
            if (chunk.tag == 0x4e47534fu) { // OSGN
                if (outputSignature) throw std::runtime_error("duplicate output signature");
                outputSignature = true;
                auto elements = parseSignature(chunk.bytes);
                emptyOutput = elements.empty();
                uint32_t targetSystemValue = 0;
                for (const auto& e : elements) {
                    if (!equalName(e.name, "SV_TARGET"))
                        throw std::runtime_error("non-colour PS output (depth/coverage/stencil)");
                    if (e.semanticIndex >= kFlatOverlayTarget || e.registerIndex >= kFlatOverlayTarget)
                        throw std::runtime_error("private MRT7 occupied");
                    if (e.componentType != 3) throw std::runtime_error("non-float colour output");
                    targetSystemValue = e.systemValue;
                }
                // Empty alpha/depth-only passes can still mark surviving fragments.
                SignatureElement coverage;
                coverage.name = "SV_TARGET";
                coverage.semanticIndex = kFlatOverlayTarget;
                coverage.systemValue = targetSystemValue;
                coverage.componentType = 3;
                coverage.registerIndex = kFlatOverlayTarget;
                coverage.masks = 0x0e01u; // x written, yzw unused
                elements.push_back(std::move(coverage));
                chunk.bytes = makeSignature(elements);
            } else if (chunk.tag == 0x58454853u || chunk.tag == 0x52444853u) { // SHEX / SHDR
                if (program) throw std::runtime_error("duplicate program");
                program = true;
                if (chunk.bytes.size() & 3u) throw std::runtime_error("program alignment");
                std::vector<uint32_t> in(chunk.bytes.size() / 4);
                std::memcpy(in.data(), chunk.bytes.data(), chunk.bytes.size());
                std::vector<uint32_t> out;
                out.reserve(in.size() + 16);
                out.push_back(in[0]);
                out.push_back(0);
                bool declared = false, returned = false, executable = false;
                // A read-only structured/raw SRV may feed conditional colour
                // math. Keep that control flow intact and add the coverage
                // write only at one unconditional, top-level terminal RET.
                // Structured loop branches stay inside their enclosing loop;
                // every path that terminates still reaches this same tail.
                struct Control { bool loop=false, elseSeen=false; };
                std::vector<Control> control;
                for (size_t at = 2; at < in.size();) {
                    const uint32_t op = in[at] & 0x7ffu;
                    const uint32_t length = instructionLength(in, at);
                    if (op == 106 && (in[at] & 0x2000u))
                        forceEarlyDepthStencil = true;
                    if (returned) throw std::runtime_error("instructions after terminal PS return");
                    // Calls, switches and conditional returns are outside
                    // this terminal-tail proof and retain their refusal.
                    if ((op >= 4 && op <= 6) || op == 9 || (op >= 19 && op <= 20) ||
                        op == 23 || op == 58 ||
                        op == 63 || op == 76 || op == 120)
                        throw std::runtime_error("unsupported PS control flow or declaration");
                    const bool conditionalControl=op==31 || op==3 || op==8;
                    const bool structuralControl=op==18 || op==21 || op==48 || op==22 || op==2 || op==7;
                    if(conditionalControl) {
                        size_t cursor=at+1;
                        flat_shader_classifier_detail::Operand condition;
                        if((in[at]&0x80000000u) ||
                           !flat_shader_classifier_detail::parseOperand(in,cursor,condition) || cursor!=at+length)
                            throw std::runtime_error("PS control condition operand bounds");
                    } else if(structuralControl && (length!=1 || (in[at]&0x80000000u)))
                        throw std::runtime_error("PS structural control instruction bounds");
                    if (op == 31 || op==48) {
                        if (control.size() >= 64) throw std::runtime_error("PS control nesting limit");
                        control.push_back({op==48,false});
                    } else if (op == 18) {
                        if (control.empty() || control.back().loop || control.back().elseSeen)
                            throw std::runtime_error("unbalanced PS ELSE");
                        control.back().elseSeen = true;
                    } else if (op == 21) {
                        if (control.empty() || control.back().loop) throw std::runtime_error("unbalanced PS ENDIF");
                        control.pop_back();
                    } else if(op==22) {
                        if(control.empty() || !control.back().loop) throw std::runtime_error("unbalanced PS ENDLOOP");
                        control.pop_back();
                    } else if(op==2 || op==3 || op==7 || op==8) {
                        bool loop=false;for(const auto& frame:control)loop|=frame.loop;
                        if(!loop)throw std::runtime_error("PS loop branch outside LOOP");
                    }
                    // Shader Model 5: DCL_RESOURCE_RAW/STRUCTURED are SRV
                    // declarations; LD_RAW/STRUCTURED read them. All UAV
                    // declarations, stores, atomics and other SM5 opcodes
                    // retain the original fail-closed path.
                    const bool readOnlySm5 = op == 161 || op == 162 || op == 165 || op == 167;
                    if (op >= 143 && !readOnlySm5)
                        throw std::runtime_error("unsupported PS UAV/structured operation");
                    if (op == 165 || op == 167) {
                        // Both LD_RAW and LD_STRUCTURED can name an SRV (t#)
                        // or a UAV (u#). Inspect the actual final resource
                        // operand, including extended opcode/operand tokens
                        // and relative-index payloads, before admitting it.
                        size_t operandAt = at + 1;
                        uint32_t extended = in[at];
                        while (extended & 0x80000000u) {
                            if (operandAt >= at + length)
                                throw std::runtime_error("structured load opcode extension");
                            extended = in[operandAt++];
                        }
                        const unsigned operandCount = op == 165 ? 3u : 4u;
                        flat_shader_classifier_detail::Operand resource;
                        for (unsigned i = 0; i < operandCount; ++i) {
                            flat_shader_classifier_detail::Operand parsed;
                            if (!flat_shader_classifier_detail::parseOperand(in, operandAt, parsed) ||
                                operandAt > at + length)
                                throw std::runtime_error("structured load operand bounds");
                            if (i + 1 == operandCount) resource = parsed;
                        }
                        if (operandAt != at + length ||
                            resource.type != flat_shader_classifier_detail::kOperandResource)
                            throw std::runtime_error("structured load is not SRV read");
                    }
                    const bool declaration = (op >= 88 && op <= 106) || op == 53 || op == 161 || op == 162;
                    if (!declaration) executable = true;
                    if (op >= 101 && op <= 103) {
                        if (length < 3) throw std::runtime_error("output declaration");
                        const uint32_t type = (in[at + 1] >> 12) & 255u;
                        if (type != 2) throw std::runtime_error("depth/coverage output declaration");
                        if (in[at + 2] >= kFlatOverlayTarget)
                            throw std::runtime_error("private MRT7 declared by original PS");
                    }
                    if (!declared && executable) {
                        const uint32_t decl[] = {0x03000065u, 0x00102012u, kFlatOverlayTarget};
                        out.insert(out.end(), decl, decl + 3);
                        declared = true;
                    }
                    if (op == 62) { // RET: only surviving fragments execute this tail.
                        if (!control.empty() || at + length != in.size() || length!=1)
                            throw std::runtime_error("PS return not terminal and top-level");
                        const uint32_t mark[] = {0x05000036u, 0x00102012u, kFlatOverlayTarget,
                                                 0x00004001u, 0x3f800000u};
                        out.insert(out.end(), mark, mark + 5);
                        returned = true;
                    }
                    out.insert(out.end(), in.begin() + at, in.begin() + at + length);
                    at += length;
                }
                if (!declared || !returned || !control.empty())
                    throw std::runtime_error("PS has no terminal unconditional return");
                out[1] = static_cast<uint32_t>(out.size());
                chunk.bytes.resize(out.size() * 4);
                std::memcpy(chunk.bytes.data(), out.data(), chunk.bytes.size());
            }
        }
        if (!outputSignature || !program) throw std::runtime_error("missing PS signature or program");
        // Check after parsing the program so either DXBC chunk order is handled.
        // Forced early depth can write depth for fragments later discarded before the mark.
        if (emptyOutput && forceEarlyDepthStencil)
            throw std::runtime_error("empty output with forced early depth/stencil");
        patched = makeContainer(chunks);
        return true;
    } catch (const std::exception& e) {
        reason = e.what();
        patched.clear();
        return false;
    }
}
// Derive private RT0 coverage from the lifetime cache's already-qualified
// MRT7 variant. Original colour outputs become fresh temps, including reads
// of those outputs; their calculations/discards and early-depth flag remain.
// No original shader output or game blend is changed during its real draw.
inline bool flatOverlayCoverageFromPatchedPs(const void* data,size_t size,
                                            std::vector<BYTE>& result,std::string& why) {
    result.clear();why.clear();
    using namespace dxbc_container;
    using namespace flat_shader_classifier_detail;
    try {
        auto chunks=parseContainer(data,size,kPs50);
        bool signature=false,program=false;
        for(auto& chunk:chunks) {
            if(chunk.tag==kTagOsgn) {
                if(signature)throw std::runtime_error("duplicate replay output signature");
                signature=true;
                auto sig=parseSignature(chunk.bytes);SignatureElement coverage;unsigned found=0;
                for(const auto& e:sig) {
                    if(!equalName(e.name,"SV_TARGET") || e.componentType!=3 ||
                       e.semanticIndex>7 || e.registerIndex>7)
                        throw std::runtime_error("replay output signature");
                    if(e.semanticIndex==7 || e.registerIndex==7) {
                        if(e.semanticIndex!=7 || e.registerIndex!=7 || e.masks!=0x0e01u)
                            throw std::runtime_error("unexpected qualified coverage signature");
                        coverage=e;++found;
                    }
                }
                if(found!=1)throw std::runtime_error("missing or duplicate qualified coverage signature");
                coverage.semanticIndex=coverage.registerIndex=0;
                chunk.bytes=makeSignature({coverage});
            } else if(chunk.tag==kTagShex || chunk.tag==kTagShdr) {
                if(program || chunk.bytes.size()<8 || (chunk.bytes.size()&3u))throw std::runtime_error("replay program");
                program=true;
                std::vector<uint32_t> in(chunk.bytes.size()/4);
                std::memcpy(in.data(),chunk.bytes.data(),chunk.bytes.size());
                uint32_t temps=0;unsigned coverageDeclarations=0,coverageWrites=0;
                for(size_t at=2;at<in.size();) {
                    const uint32_t op=in[at]&0x7ffu;const size_t len=instructionLength(in,at);
                    if(op==kOpDclTemps) {
                        if(len!=2)throw std::runtime_error("replay temps declaration");
                        temps=(std::max)(temps,in[at+1]);
                    }
                    if(op>=101 && op<=103) {
                        if(len<3)throw std::runtime_error("short replay output declaration");
                        if(in[at+2]==7) {
                            if(op!=101 || len!=3 || in[at+1]!=0x00102012u)
                                throw std::runtime_error("unexpected coverage declaration");
                            ++coverageDeclarations;
                        }
                    }
                    if(!isDeclaration(op)) {
                        if(op==54 && (in[at]&0x80000000u))
                            throw std::runtime_error("unexpected MOV opcode extension");
                        const int count=operandCount(op);
                        if(count<0)throw std::runtime_error("unknown replay operand layout");
                        size_t cursor=at+1;
                        uint32_t extended=in[at];
                        while(extended&0x80000000u) {
                            if(cursor>=at+len)throw std::runtime_error("replay opcode extension");
                            extended=in[cursor++];
                            if((extended&0x3fu)>3u)throw std::runtime_error("unknown replay opcode extension");
                        }
                        for(int i=0;i<count;++i) {
                            Operand o;
                            if(!parseOperand(in,cursor,o) || cursor>at+len)
                                throw std::runtime_error("replay operand parse");
                            if(o.type==kOperandTemp) {
                                if(o.reg>=4096)throw std::runtime_error("replay temp operand budget");
                                temps=(std::max)(temps,o.reg+1);
                            }
                            if(o.type==kOperandOutput && o.reg==7) {
                                if(i!=0 || op!=54 || len!=5 || in[at]!=0x05000036u ||
                                   in[at+1]!=0x00102012u || in[at+2]!=7 ||
                                   in[at+3]!=0x00004001u || in[at+4]!=0x3f800000u ||
                                   at+len+1!=in.size() || in[at+len]!=0x0100003eu)
                                    throw std::runtime_error("unexpected executable MRT7 operand");
                                ++coverageWrites;
                            }
                        }
                        if(cursor!=at+len)throw std::runtime_error("replay trailing operands");
                    }
                    at+=len;
                }
                if(temps>4089)throw std::runtime_error("replay temp budget");
                if(coverageDeclarations!=1 || coverageWrites!=1)
                    throw std::runtime_error("missing or duplicate coverage injection");
                std::vector<uint32_t> out{in[0],0};bool declared=false;
                for(size_t at=2;at<in.size();) {
                    const uint32_t op=in[at]&0x7ffu;const size_t len=instructionLength(in,at);
                    if(op==kOpDclTemps || (op>=101 && op<=103)) { at+=len;continue; }
                    if(!isDeclaration(op)) {
                        if(!declared) {
                            const uint32_t decl[]={0x02000068u,temps+7,0x03000065u,0x00102012u,0};
                            out.insert(out.end(),decl,decl+5);declared=true;
                        }
                        size_t cursor=at+1;uint32_t extended=in[at];
                        while(extended&0x80000000u) {
                            if(cursor>=at+len)throw std::runtime_error("replay rewrite extension");
                            extended=in[cursor++];
                        }
                        for(int i=0;i<operandCount(op);++i) {
                            const size_t start=cursor;Operand o;
                            if(!parseOperand(in,cursor,o))throw std::runtime_error("replay rewrite parse");
                            if(o.type!=kOperandOutput)continue;
                            const uint32_t tok=in[start];
                            if(o.relative || ((tok>>20)&3u)!=1u || ((tok>>22)&7u)!=0u || o.reg>7)
                                throw std::runtime_error("unbounded replay output index");
                            size_t index=start+1;uint32_t operandExtended=tok;
                            while(operandExtended&0x80000000u) {
                                if(index>=cursor)throw std::runtime_error("replay operand extension");
                                operandExtended=in[index++];
                            }
                            if(index>=cursor)throw std::runtime_error("missing replay output index");
                            if(o.reg==7)in[index]=0;
                            else { in[start]=tok&~(255u<<12);in[index]=temps+o.reg; }
                        }
                    }
                    out.insert(out.end(),in.begin()+at,in.begin()+at+len);at+=len;
                }
                if(!declared)throw std::runtime_error("missing replay coverage declaration");
                out[1]=static_cast<uint32_t>(out.size());chunk.bytes.resize(out.size()*4);
                std::memcpy(chunk.bytes.data(),out.data(),chunk.bytes.size());
            }
        }
        if(!signature || !program)throw std::runtime_error("missing replay signature or program");
        result=makeContainer(chunks);return true;
    } catch(const std::exception& e) { why=e.what();result.clear();return false; }
}
} // namespace edvr
