#pragma once

#include <vector>
#include <cstring>
#include <cstdio>

#include "../../src/installer/probe.h"

namespace edvr::installer::test {

// Contract negatives are intentionally byte buffers: the validator must reject
// malformed, truncated, and non PE data without mapping or executing it.
template <class Check>
inline void nativeContractCases(Check check) {
    const std::vector<unsigned char> empty;
    check(!validateNativePayloadBytes(empty.data(), empty.size(), NativeImageKind::Graphics),
          "empty native graphics payload rejected");
    const unsigned char mz[] = {'M', 'Z', 0, 0, 0, 0, 0, 0};
    check(!validateNativePayloadBytes(mz, sizeof(mz), NativeImageKind::Graphics),
          "truncated native graphics payload rejected");
    check(!validateNativePayloadBytes(mz, sizeof(mz), NativeImageKind::Runtime),
          "truncated native runtime payload rejected");
    check(!validateNativePayloadBytes(mz, sizeof(mz), NativeImageKind::Loader),
          "truncated OpenXR loader payload rejected");
}

template <class Check>
inline void nativeBuiltContractCases(Check check, const std::wstring& root) {
    auto bytes=[&](const wchar_t* leaf) {
        std::vector<unsigned char> out;
        FILE* file=nullptr;
        const auto path=root+L"\\build\\"+leaf;
        if(_wfopen_s(&file,path.c_str(),L"rb") || !file) return out;
        fseek(file,0,SEEK_END);const long size=ftell(file);rewind(file);
        if(size>0 && size<(64<<20)) {out.resize(size);if(fread(out.data(),1,out.size(),file)!=out.size())out.clear();}
        fclose(file);return out;
    };
    const auto graphics=bytes(L"edvr_openxr_graphics.dll");
    const auto runtime=bytes(L"edvr_openxr_runtime.dll");
    const auto loader=bytes(L"openxr_loader.dll");
    check(validateNativePayloadBytes(graphics.data(),graphics.size(),NativeImageKind::Graphics),"built native graphics marker/providers accepted");
    check(validateNativePayloadBytes(runtime.data(),runtime.size(),NativeImageKind::Runtime),"built native runtime exact export contract accepted");
    check(validateNativePayloadBytes(loader.data(),loader.size(),NativeImageKind::Loader),"bundled x64 Khronos loader accepted");
    const auto legacy=bytes(L"legacy_d3d11_fixture.dll");
    check(!legacy.empty(),"legacy rejection fixture exists");
    check(!validateNativePayloadBytes(legacy.data(),legacy.size(),NativeImageKind::Graphics),"legacy graphics fixture rejected as release payload");
    check(probeDll(root+L"\\build\\edvr_openxr_runtime.dll").kind==DllKind::Edvr,"native runtime is identified as EDVR, never as an original to preserve");
    if(graphics.empty())return;
    // Mutate the actual built PE using the export ordinal table, so a marker
    // with a high/nonalphabetic ordinal is exercised too. Never execute it.
    auto bad=graphics;
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(bad.data());
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(bad.data()+dos->e_lfanew);
    auto* sections=IMAGE_FIRST_SECTION(nt);
    auto offset=[&](DWORD rva) -> size_t {
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)
            if(rva>=sections[i].VirtualAddress && rva-sections[i].VirtualAddress<sections[i].SizeOfRawData)
                return sections[i].PointerToRawData+rva-sections[i].VirtualAddress;
        return bad.size();
    };
    auto* exp=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(bad.data()+offset(nt->OptionalHeader.DataDirectory[0].VirtualAddress));
    auto* names=reinterpret_cast<DWORD*>(bad.data()+offset(exp->AddressOfNames));
    auto* funcs=reinterpret_cast<DWORD*>(bad.data()+offset(exp->AddressOfFunctions));
    auto* ords=reinterpret_cast<WORD*>(bad.data()+offset(exp->AddressOfNameOrdinals));
    size_t marker=bad.size(),provider=bad.size();DWORD markerRva=0;
    for(DWORD i=0;i<exp->NumberOfNames;++i) {
        const auto no=offset(names[i]);const char* name=reinterpret_cast<const char*>(bad.data()+no);
        if(!strcmp(name,"edvrNativeStartupRouting")) {markerRva=funcs[ords[i]];marker=offset(markerRva);}
        if(!strcmp(name,"edvrAcquireNativeGraphics"))provider=no;
    }
    if(marker+16>bad.size() || provider==bad.size()) {check(false,"built marker/provider found for mutation tests");return;}
    bad[marker+8]=0;
    check(!validateNativePayloadBytes(bad.data(),bad.size(),NativeImageKind::Graphics),"route-disabled marker rejected");
    bad[marker+8]=1;bad[provider]='x';
    check(!validateNativePayloadBytes(bad.data(),bad.size(),NativeImageKind::Graphics),"missing graphics provider rejected");
    bad[provider]='e';
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(markerRva>=sections[i].VirtualAddress && markerRva-sections[i].VirtualAddress<sections[i].SizeOfRawData) {
        sections[i].Characteristics|=IMAGE_SCN_MEM_WRITE;
        check(!validateNativePayloadBytes(bad.data(),bad.size(),NativeImageKind::Graphics),"writable startup marker rejected");
    }
}

} // namespace edvr::installer::test
