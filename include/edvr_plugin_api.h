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
    // The host currently calls onFilterInput once per frame with deviceType 0 and rawInputData null. The values below are
    // reserved, not yet delivered: do not branch on them.
    uint32_t deviceType;          // 0 = Keyboard/Mouse, 1 = DirectInput Joystick
    const void* rawInputData;
    // Set to 1 to block input from the game. The host clears it to 0 before every call, and a frame in which no addon sets it
    // releases the hold at once: it is a per-frame request, not a latch, so set it on every onFilterInput call for as long as you want
    // input held.
    uint8_t swallowInput;
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

// Values (min/max/step/default, what a getter returns, what a setter receives, and what is saved) are an int64_t whose meaning
// depends on the setting type:
//   BOOL    0 or 1; saved as on/off.
//   CHOICE  an index into choiceOptions; saved as the option's text (matched case-insensitively on restore).
//   INT     the number; saved as a decimal integer.
//   FLOAT   IEEE-754 float bits in the low 4 bytes of the int64_t (memcpy a float into a zeroed int64_t; do not cast); saved
//           with 2 decimals, so finer steps do not survive a restart.
//   ACTION  the setter is called with 1 each time the row is run. A FLOAT row needs an explicit stepValue (float bits).
// Registration also calls setter once (see registerSetting below), with the restored value: the value saved in the ini
// for this addon, header and key if there is one, else defaultValue.
// Callback to read the current setting value from the plugin
typedef int64_t (EDVR_API *EdvrPluginSettingGetter)(const char* settingKey, void* userData);

// Callback to update setting value when changed in F8 Menu
typedef void (EDVR_API *EdvrPluginSettingSetter)(const char* settingKey, int64_t value, void* userData);

// Definition of a setting registered under the F8 Menu "Plugins" tab
typedef struct EdvrPluginSettingDef {
    uint32_t structSize;
    const char* header;               // Header under Plugins tab (e.g. "Cockpit MFD")
    const char* key;                  // Unique setting key (e.g. "show_all_windows"). Saved keys are lowercased, so keys
                                      // differing only in case share one saved value; callbacks and F8 keep your spelling.
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
    // Adds a row to the F8 menu's Plugins page. Returns 0 on success.
    // While registering, the host calls setting->setter once with the restored value (saved value, else defaultValue), inside
    // your onInitialize and under the host's settings lock: apply it to your state and return.
    // Call it only from within onInitialize: anywhere else (after it returns, from onUpdate, onRenderEye,
    // onFilterInput or another thread) it returns -1 and registers nothing.
    // The host removes every setting the addon registered when the addon fails initialisation (onInitialize
    // returns non-zero or faults), when it faults in a later callback and is disabled, and when it shuts down.
    // Getters and setters therefore never outlive the addon's code.
    // Addons run only while the game is in VR. In a flat session the host is dormant: it makes no callback, hides the
    // addon's rows and releases its input ownership, and calls everything again when VR resumes. Nothing is unloaded at
    // a switch, so an addon needs no handling for it.
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
    // headPose is the located head, not either eye: the OpenXR VIEW reference space located in the session's seated origin (the same local
    // reference space the eyes' eyePose values are expressed in), at the frame's predicted display time. Metres, right-handed, +Y up, -Z forward.
    // Its position lies between the eyes, so an eye is about half the interpupillary distance to either side of it and may be
    // canted. Aim a gaze ray from this pose. Called once per frame, only when the head was located with valid position and orientation.
    // dtSeconds is the host's own QPC measurement of the time since the previous onUpdate call, used when it is between 0.0001 and
    // 0.5 seconds. On the first call, or when the measurement is outside that range (a stall, a pause), the host's fallback of
    // 0.016 is passed instead, so treat it as a frame time, never as a fixed 16 ms.
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
