#pragma once

#include <stdint.h>
#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_PLUGIN_API_VERSION 1

typedef struct EdvrVector3f {
    float x;
    float y;
    float z;
} EdvrVector3f;

typedef struct EdvrQuaternionf {
    float x;
    float y;
    float z;
    float w;
} EdvrQuaternionf;

typedef struct EdvrPosef {
    EdvrQuaternionf orientation;
    EdvrVector3f position;
} EdvrPosef;

typedef struct EdvrFovf {
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
} EdvrFovf;

// Context passed to plugins during eye rendering
typedef struct EdvrEyeRenderContext {
    uint32_t structSize;
    uint32_t eyeIndex;             // 0 = Left, 1 = Right
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11RenderTargetView* rtv;
    EdvrPosef eyePose;
    EdvrFovf eyeFov;
    uint32_t viewportWidth;
    uint32_t viewportHeight;
} EdvrEyeRenderContext;

// Context passed to plugins for input routing and filtering
typedef struct EdvrInputContext {
    uint32_t structSize;
    uint32_t deviceType;          // 0 = Keyboard/Mouse, 1 = DirectInput Joystick
    const void* rawInputData;
    uint8_t swallowInput;         // Plugin sets to 1 if input should be blocked from game
} EdvrInputContext;

// Plugin lifecycle and hook callbacks table
typedef struct EdvrPluginCallbacks {
    uint32_t structSize;
    const char* pluginName;
    const char* pluginVersion;

    int (*onInitialize)(void* hostReserved);
    void (*onShutdown)(void);
    void (*onUpdate)(const EdvrPosef* headPose, float dtSeconds);
    void (*onRenderEye)(const EdvrEyeRenderContext* eyeCtx);
    void (*onFilterInput)(EdvrInputContext* inputCtx);
} EdvrPluginCallbacks;

// Exported entry point every EDVR addon must provide
// Export name: "EdvrPluginRegister"
typedef int (*EdvrPluginRegisterFunc)(uint32_t hostApiVersion, EdvrPluginCallbacks* outCallbacks);

#ifdef __cplusplus
}
#endif
