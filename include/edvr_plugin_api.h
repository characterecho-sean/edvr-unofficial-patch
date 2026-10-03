#pragma once

#include <stdint.h>
#include <d3d11.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_PLUGIN_API_VERSION 2

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

// Plugin setting types for F8 Menu under the "Plugins" tab
typedef enum EdvrPluginSettingType {
    EDVR_PLUGIN_SETTING_BOOL = 0,     // Toggle switch (On/Off)
    EDVR_PLUGIN_SETTING_CHOICE = 1,   // String choice cycle (e.g. presets)
    EDVR_PLUGIN_SETTING_INT = 2,      // Integer value with min/max/step
    EDVR_PLUGIN_SETTING_FLOAT = 3,    // Float value with min/max/step
    EDVR_PLUGIN_SETTING_ACTION = 4    // Action button ("run")
} EdvrPluginSettingType;

#ifndef EDVR_API
#if defined(_WIN32)
#define EDVR_API __stdcall
#else
#define EDVR_API
#endif
#endif

// Callback to read the current setting value from the plugin
typedef int64_t (EDVR_API *EdvrPluginSettingGetter)(const char* settingKey, void* userData);

// Callback to update setting value when changed in F8 Menu
typedef void (EDVR_API *EdvrPluginSettingSetter)(const char* settingKey, int64_t value, void* userData);

// Definition of a setting registered under the F8 Menu "Plugins" tab
typedef struct EdvrPluginSettingDef {
    uint32_t structSize;
    const char* header;               // Header under Plugins tab (e.g. "Cockpit MFD")
    const char* key;                  // Unique setting key (e.g. "show_all_windows")
    const char* label;                // Label shown in menu (e.g. "Show all windows")
    const char* hint;                 // Hint / tooltip shown when highlighted
    EdvrPluginSettingType type;       // Type of setting control
    int64_t minValue;                 // Minimum value for INT (or 0)
    int64_t maxValue;                 // Maximum value for INT (or choiceCount - 1)
    int64_t stepValue;                // Step increment for INT/FLOAT
    int64_t defaultValue;             // Default initial value
    const char** choiceOptions;       // Array of string options for CHOICE
    uint32_t choiceCount;             // Number of options in choiceOptions
    EdvrPluginSettingGetter getter;   // Function to query live value
    EdvrPluginSettingSetter setter;   // Function to apply new value
    void* userData;                   // Custom context pointer passed to getter/setter
} EdvrPluginSettingDef;

// Host services provided to plugins during initialization
typedef struct EdvrHostServices {
    uint32_t structSize;
    int (EDVR_API *registerSetting)(const EdvrPluginSettingDef* setting);
    void (EDVR_API *logNote)(const char* message);
} EdvrHostServices;

// Plugin lifecycle and hook callbacks table
typedef struct EdvrPluginCallbacks {
    uint32_t structSize;
    const char* pluginName;
    const char* pluginVersion;

    int (EDVR_API *onInitialize)(const EdvrHostServices* host);
    void (EDVR_API *onShutdown)(void);
    void (EDVR_API *onUpdate)(const EdvrPosef* headPose, float dtSeconds);
    void (EDVR_API *onRenderEye)(const EdvrEyeRenderContext* eyeCtx);
    void (EDVR_API *onFilterInput)(EdvrInputContext* inputCtx);
} EdvrPluginCallbacks;

// Exported entry point every EDVR addon must provide
// Export name: "EdvrPluginRegister"
typedef int (EDVR_API *EdvrPluginRegisterFunc)(uint32_t hostApiVersion, EdvrPluginCallbacks* outCallbacks);

#ifdef __cplusplus
}
#endif
