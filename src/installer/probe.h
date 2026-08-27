// What a DLL on disk actually IS -- answered by reading the file.
//
// The installer has to make decisions about files other people's installers
// wrote: is this d3d11.dll ours, EDHM's, ReShade's, or the game's own? The one
// answer it must never get by LoadLibrary is "ours" -- loading a stranger's
// d3d11.dll runs that stranger's DllMain inside the installer, and the file we
// are least sure about is exactly the one we would be executing. So the PE is
// mapped read-only and its export table is parsed by hand.
//
// The discriminator for our own proxies is the same one gen_exports.py uses to
// refuse building a proxy of a proxy: an EDVR build exports edvr_* symbols and
// nothing else in this world does.
#pragma once

#include <windows.h>

#include <string>

namespace edvr::installer {

enum class DllKind {
    Absent,          // nothing at that path
    Unreadable,      // present, but not a PE this can parse (or locked)
    Edvr,            // ours: exports edvr_*
    OpenVrRuntime,   // a real openvr_api.dll: exports VR_InitInternal
    D3d11Provider,   // exports D3D11CreateDevice: the system copy, or another mod's proxy
    Foreign          // a PE, but none of the above
};

struct DllInfo {
    DllKind      kind = DllKind::Absent;
    std::wstring path;
    bool         is64 = false;
    unsigned long long size = 0;

    bool hasEdvrExports  = false;
    bool hasVrInit       = false;
    bool hasD3d11Create  = false;

    // VERSIONINFO, when there is any. Used only to give a foreign file a NAME
    // in the report and a sensible filename when we rename it: "EDHM" reads
    // very differently from "some other d3d11.dll" to somebody deciding
    // whether to let an installer touch their game.
    std::wstring product;
    std::wstring description;
    std::wstring company;
    std::wstring fileVersion;

    std::string sha256;   // lowercase hex, empty if the file could not be read
};

DllInfo probeDll(const std::wstring& path);

std::string sha256File(const std::wstring& path);
std::string sha256Bytes(const void* data, size_t bytes);

// "EDHM", "ReShade", "DXVK", "OpenComposite" or empty when nothing identifies
// it. Recognised from VERSIONINFO strings, which every one of these ships.
std::wstring modNameOf(const DllInfo& info);

// The filename we give a foreign d3d11.dll when we take its place:
// d3d11_edhm.dll, d3d11_reshade.dll, d3d11_other.dll. Lowercase, no spaces --
// it goes into edvr.ini as advanced.real_dll and is read by a parser that
// trims at whitespace-then-comment.
std::wstring chainNameFor(const DllInfo& info);

// One line for the report pane: "EDHM 3.5.1 (3Dmigoto d3d11.dll, 2.1 MB)".
std::wstring describeDll(const DllInfo& info);

}  // namespace edvr::installer
