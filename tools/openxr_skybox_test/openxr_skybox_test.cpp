#include <d3d11.h>

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <wrl/client.h>

#include "../../src/openxr/skybox_capture.h"

using Microsoft::WRL::ComPtr;
using edvr::openxr::SkyboxCapture;

namespace {

struct Checks {
  unsigned total = 0;
  unsigned failed = 0;

  void expect(bool condition, const char* name) {
    ++total;
    if (!condition) {
      ++failed;
      std::printf("FAIL: %s\n", name);
    }
  }
};

DXGI_FORMAT SourceFormat(unsigned face) {
  switch (face % 6) {
    case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 1: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case 2: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 3: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case 4: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    default: return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
}

ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* device, unsigned width,
                                    unsigned height, DXGI_FORMAT format,
                                    const std::vector<unsigned char>* bytes = nullptr,
                                    unsigned mips = 1, unsigned arraySize = 1,
                                    unsigned samples = 1) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = mips;
  desc.ArraySize = arraySize;
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  D3D11_SUBRESOURCE_DATA data{};
  if (bytes && mips == 1 && arraySize == 1 && samples == 1) {
    data.pSysMem = bytes->data();
    data.SysMemPitch = width * 4;
  }
  ComPtr<ID3D11Texture2D> result;
  if (FAILED(device->CreateTexture2D(&desc, bytes ? &data : nullptr, &result))) {
    return {};
  }
  return result;
}

bool ReadPixels(ID3D11Device* device, ID3D11Texture2D* source,
                std::vector<unsigned char>* output) {
  if (!source || !output) return false;
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);
  if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
    return false;
  }
  D3D11_TEXTURE2D_DESC staging = desc;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> copy;
  if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy))) return false;
  context->CopyResource(copy.Get(), source);
  context->Flush();
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  output->assign(static_cast<size_t>(desc.Width) * desc.Height * 4, 0);
  for (unsigned row = 0; row < desc.Height; ++row) {
    std::memcpy(output->data() + row * desc.Width * 4,
                static_cast<const unsigned char*>(mapped.pData) + row * mapped.RowPitch,
                desc.Width * 4);
  }
  context->Unmap(copy.Get(), 0);
  return true;
}

std::array<vr::Texture_t, 6> MakeInputs(const std::array<ComPtr<ID3D11Texture2D>, 6>& textures,
                                        vr::EColorSpace color = vr::ColorSpace_Auto) {
  std::array<vr::Texture_t, 6> result{};
  for (unsigned face = 0; face < 6; ++face) {
    result[face] = {textures[face].Get(), vr::API_DirectX,
                    face & 1 ? vr::ColorSpace_Linear : color};
  }
  return result;
}

bool SameSnapshot(ID3D11Device* device, SkyboxCapture& capture,
                  const std::array<ID3D11Texture2D*, 6>& pointers,
                  const std::array<std::vector<unsigned char>, 6>& pixels,
                  Checks* checks, const char* prefix) {
  bool result = true;
  for (unsigned face = 0; face < 6; ++face) {
    result &= capture.texture(face) == pointers[face];
    std::vector<unsigned char> actual;
    result &= ReadPixels(device, capture.texture(face), &actual) && actual == pixels[face];
  }
  checks->expect(result, prefix);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || (std::strcmp(argv[1], "--self-test") != 0 &&
                    std::strcmp(argv[1], "--dry-run") != 0)) {
    std::puts("usage: --self-test | --dry-run");
    return 2;
  }
  if (std::strcmp(argv[1], "--dry-run") == 0) {
    std::puts("Would run WARP skybox self-test; no D3D11 device created.");
    return 0;
  }

  Checks checks;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level{};
  HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                      nullptr, 0, D3D11_SDK_VERSION, &device,
                                      &level, &context);
  checks.expect(SUCCEEDED(created) && device && context, "WARP device");
  if (!device) return 1;

  SkyboxCapture capture;
  checks.expect(SUCCEEDED(capture.initialize(device.Get())) && !capture.ready(),
                "initialize starts empty");

  std::array<ComPtr<ID3D11Texture2D>, 6> sources;
  std::array<std::vector<unsigned char>, 6> expected;
  for (unsigned face = 0; face < 6; ++face) {
    const unsigned width = face == 0 ? 1 : face + 1;
    const unsigned height = face == 0 ? 1 : face + 2;
    expected[face].resize(width * height * 4);
    for (size_t byte = 0; byte < expected[face].size(); ++byte) {
      expected[face][byte] = static_cast<unsigned char>(17 * face + byte + 3);
    }
    const DXGI_FORMAT format = SourceFormat(face);
    sources[face] = MakeTexture(device.Get(), width, height, format, &expected[face]);
    checks.expect(!!sources[face], "source texture including 1x1");
  }
  auto inputs = MakeInputs(sources);
  checks.expect(capture.set(inputs.data(), 6) == vr::VRCompositorError_None && capture.ready(),
                "six faces publish atomically");

  std::array<ID3D11Texture2D*, 6> oldPointers{};
  std::array<std::vector<unsigned char>, 6> oldPixels;
  for (unsigned face = 0; face < 6; ++face) {
    oldPointers[face] = capture.texture(face);
    checks.expect(oldPointers[face] != nullptr, "private face exists");
    checks.expect(capture.colorSpace(face) == inputs[face].eColorSpace, "color metadata");
    checks.expect(ReadPixels(device.Get(), oldPointers[face], &oldPixels[face]) &&
                      oldPixels[face] == expected[face], "private pixels copied");
  }

  // The caller may overwrite and release its inputs after set returns.
  for (auto& source : sources) {
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    std::vector<unsigned char> overwrite(desc.Width*desc.Height*4,0);
    context->UpdateSubresource(source.Get(),0,nullptr,overwrite.data(),desc.Width*4,0);
    source.Reset();
  }
  SameSnapshot(device.Get(), capture, oldPointers, oldPixels, &checks,
               "source overwrite and release leaves private snapshot");

  std::array<ComPtr<ID3D11Texture2D>, 6> replacement;
  std::array<std::vector<unsigned char>, 6> replacementPixels;
  for (unsigned face = 0; face < 6; ++face) {
    replacementPixels[face].assign((face + 1) * (face + 1) * 4,
                                   static_cast<unsigned char>(200 + face));
    replacement[face] = MakeTexture(device.Get(), face + 1, face + 1,
                                    face & 1 ? DXGI_FORMAT_B8G8R8A8_UNORM
                                              : DXGI_FORMAT_R8G8B8A8_UNORM,
                                    &replacementPixels[face]);
  }
  auto replacementInputs = MakeInputs(replacement, vr::ColorSpace_Gamma);
  checks.expect(capture.set(replacementInputs.data(), 6) == vr::VRCompositorError_None,
                "replacement publishes");
  for (unsigned face = 0; face < 6; ++face) {
    checks.expect(capture.texture(face) != oldPointers[face], "replacement uses fresh private texture");
    std::vector<unsigned char> actual;
    checks.expect(ReadPixels(device.Get(), capture.texture(face), &actual) &&
                      actual == replacementPixels[face], "replacement pixels copied");
  }

  const std::vector<unsigned char> onePixel={3,71,121,255};
  ComPtr<ID3D11Texture2D> typeless =
      MakeTexture(device.Get(), 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS,&onePixel);
  if (typeless) {
    auto typelessInputs = replacementInputs;
    typelessInputs[5].handle = typeless.Get();
    checks.expect(capture.set(typelessInputs.data(), 6) == vr::VRCompositorError_None,
                  "typeless RGBA source accepted");
  } else {
    checks.expect(false, "typeless RGBA source creation");
  }

  std::array<ID3D11Texture2D*, 6> replacementPointers{};
  std::array<std::vector<unsigned char>, 6> snapshotPixels;
  for (unsigned face = 0; face < 6; ++face) {
    replacementPointers[face] = capture.texture(face);
    checks.expect(ReadPixels(device.Get(), replacementPointers[face], &snapshotPixels[face]),"snapshot read succeeds");
  }
  auto invalid = replacementInputs;
  invalid[5].eType = vr::API_OpenGL;
  checks.expect(capture.set(invalid.data(), 6) == vr::VRCompositorError_InvalidTexture,
                "late invalid face rejected");
  SameSnapshot(device.Get(), capture, replacementPointers, snapshotPixels, &checks,
               "late invalid face leaves all six unchanged");
  for (unsigned count : {0u, 1u, 2u, 5u, 7u}) {
    checks.expect(capture.set(replacementInputs.data(), count) == vr::VRCompositorError_InvalidTexture,
                  "unsupported face count rejected");
  }
  checks.expect(capture.set(nullptr, 6) == vr::VRCompositorError_InvalidTexture,
                "null array rejected");
  invalid = replacementInputs;invalid[5].handle=nullptr;
  checks.expect(capture.set(invalid.data(),6)==vr::VRCompositorError_InvalidTexture,"late null face rejected");
  invalid = replacementInputs;
  invalid[0].eColorSpace = static_cast<vr::EColorSpace>(99);
  checks.expect(capture.set(invalid.data(), 6) == vr::VRCompositorError_InvalidTexture,
                "invalid color rejected");

  ComPtr<ID3D11Device> secondDevice;
  ComPtr<ID3D11DeviceContext> secondContext;
  D3D_FEATURE_LEVEL secondLevel{};
  D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                    D3D11_SDK_VERSION, &secondDevice, &secondLevel, &secondContext);
  checks.expect(!!secondDevice,"foreign WARP device created");
  if (secondDevice) {
    auto foreign = MakeTexture(secondDevice.Get(), 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               &replacementPixels[0]);
    auto wrongDevice = replacementInputs;
    wrongDevice[5].handle = foreign.Get();
    checks.expect(capture.set(wrongDevice.data(), 6) == vr::VRCompositorError_TextureIsOnWrongDevice,
                  "wrong device rejected");
  }

  D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=16;bufferDesc.Usage=D3D11_USAGE_DEFAULT;
  bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;ComPtr<ID3D11Buffer> buffer;
  checks.expect(SUCCEEDED(device->CreateBuffer(&bufferDesc,nullptr,&buffer)),"non-texture COM source");
  invalid=replacementInputs;invalid[5].handle=buffer.Get();
  checks.expect(capture.set(invalid.data(),6)==vr::VRCompositorError_InvalidTexture,"buffer QI failure distinct from wrong device");

  auto unsupported = replacementInputs;
  ComPtr<ID3D11Texture2D> unsupportedTexture =
      MakeTexture(device.Get(), 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT);
  unsupported[5].handle = unsupportedTexture.Get();
  checks.expect(capture.set(unsupported.data(), 6) == vr::VRCompositorError_TextureUsesUnsupportedFormat,
                "unsupported format rejected");

  for (unsigned variant=0;variant<3;++variant) {
    auto bad=MakeTexture(device.Get(),4,4,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,
                         variant==0?2:1,variant==1?2:1,variant==2?2:1);
    checks.expect(!!bad,"mip/array/MSAA fixture created");
    if(bad){auto candidate=replacementInputs;candidate[5].handle=bad.Get();
      checks.expect(capture.set(candidate.data(),6)==vr::VRCompositorError_InvalidTexture,"mip/array/MSAA source rejected");}
  }
  SameSnapshot(device.Get(),capture,replacementPointers,snapshotPixels,&checks,"all failed updates preserve every private face and pixel");

  checks.expect(capture.texture(6) == nullptr && capture.colorSpace(6) == vr::ColorSpace_Auto,
                "face bounds are safe");
  capture.clear();
  bool empty=!capture.ready();for(unsigned face=0;face<6;++face)empty=empty&&!capture.texture(face);
  checks.expect(empty, "clear retires all six faces");
  checks.expect(capture.set(replacementInputs.data(),6)==vr::VRCompositorError_None,"clear permits replacement");
  capture.shutdown();
  checks.expect(!capture.ready()&&capture.set(replacementInputs.data(),6)==vr::VRCompositorError_InvalidTexture, "shutdown rejects new copies");
  std::printf("openxr skybox self-test: %u checks, %u failures\n", checks.total, checks.failed);
  return checks.failed == 0 ? 0 : 1;
}
