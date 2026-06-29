#define WIN32_LEAN_AND_MEAN

#include "pch.h"

#include <algorithm>
#include <windows.h>
#include <dinput.h>
#include <fstream>
#include <string>
#include <mutex>
#include <iostream>
#include <ostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#pragma comment(lib, "dxguid.lib")

// Global configuration variables with default fallback values
int g_Logging = 1;
int g_TargetDeviceIndex = 0;
int g_PassGaugeFillupTime = 50;
int g_ButtonXIndex = 0;
int g_ButtonOIndex = 1;
int g_ButtonL1Index = 4;
float g_ButtonL2Threshold = 0.9f;
float g_RightStickThreshold = 0.5f;
int g_ButtonL2ToggleIndex = 8;
float g_LeftStickThreshold = 0.5f;
int g_GameVersion = 6;
int g_ForceManualPass = 0;

// LOGGING SYSTEM
std::ofstream logFile;
std::mutex logMutex;

int joyPadCount = 0;
// Auto detected active device index, 0 means auto detection
int activeDeviceIndex = 0;
bool forceManualPass = false;

void Log(const std::string& message) {
    if (!g_Logging) return;

    std::lock_guard<std::mutex> lock(logMutex);

    // 1. Get current time point from the high-resolution system clock
    auto now = std::chrono::system_clock::now();

    // 2. Convert to time_t to get seconds, minutes, hours, etc.
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm timeInfo;
    localtime_s(&timeInfo, &time_t_now);

    // 3. Extract the millisecond fraction
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration - seconds);

    // 4. Use a stringstream to build the time string with milliseconds padded to 3 digits
    std::stringstream ss;
    ss << "[" << std::put_time(&timeInfo, "%Y-%m-%d %X")
        << "." << std::setfill('0') << std::setw(3) << milliseconds.count() << "] ";

    if (logFile.is_open()) {
        logFile << ss.str() << message << std::endl;
    }
}

// Helper to initialize the log file next to the game executable
void InitLogging() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    size_t pos = std::string(buffer).find_last_of("\\/");
    std::string logPath = std::string(buffer).substr(0, pos) + "\\dinput8.log.txt";

    logFile.open(logPath, std::ios::out | std::ios::trunc);
    Log("[INFO] DirectInput8 Proxy Logger Initialized.");
}

void LoadConfiguration() {
    // Looks in the current working directory (the game's directory)
    std::ifstream configFile("dinput8.cfg");
    if (!configFile.is_open()) {
        Log("[CONFIG] dinput8.cfg not found. Using default internal values.");
        return;
    }

    std::string line;
    while (std::getline(configFile, line)) {
        // Remove trailing carriage returns if the file has Windows line endings
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Clean up whitespace for robust parsing
        line.erase(std::remove_if(line.begin(), line.end(), isspace), line.end());

        // Skip empty lines or comment lines
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Find the key=value delimiter
        size_t delimiterPos = line.find('=');
        if (delimiterPos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, delimiterPos);
        std::string value = line.substr(delimiterPos + 1);

        // Assign values based on matching keys
        if (key == "Logging") {
            g_Logging = std::stoi(value);
            Log("[CONFIG] Loaded Logging = " + std::to_string(g_Logging));
        }
        if (key == "GameVersion") {
            g_GameVersion = std::stoi(value);
            Log("[CONFIG] Loaded GameVersion = " + std::to_string(g_GameVersion));
        }
        else if (key == "TargetDeviceIndex") {
            g_TargetDeviceIndex = std::stoi(value);
            Log("[CONFIG] Loaded TargetDeviceIndex = " + std::to_string(g_TargetDeviceIndex));
        }
        else if (key == "PassGaugeFillupTime") {
            g_PassGaugeFillupTime = std::stoi(value);
            Log("[CONFIG] Loaded PassGaugeFillupTime = " + std::to_string(g_PassGaugeFillupTime));
        }
        else if (key == "ButtonXIndex") {
            g_ButtonXIndex = std::stoi(value);
            Log("[CONFIG] Loaded ButtonXIndex = " + std::to_string(g_ButtonXIndex));
        }
        else if (key == "ButtonOIndex") {
            g_ButtonOIndex = std::stoi(value);
            Log("[CONFIG] Loaded ButtonOIndex = " + std::to_string(g_ButtonOIndex));
        }
        else if (key == "ButtonL1Index") {
            g_ButtonL1Index = std::stoi(value);
            Log("[CONFIG] Loaded ButtonL1Index = " + std::to_string(g_ButtonL1Index));
        }
        else if (key == "ButtonL2Threshold") {
            g_ButtonL2Threshold = std::stof(value);
            Log("[CONFIG] Loaded ButtonL2Threshold = " + std::to_string(g_ButtonL2Threshold));
        }
        else if (key == "RightStickThreshold") {
            g_RightStickThreshold = std::stof(value);
            Log("[CONFIG] Loaded RightStickThreshold = " + std::to_string(g_RightStickThreshold));
        }
        else if (key == "ForceManualPass") {
            g_ForceManualPass = std::stoi(value);
            Log("[CONFIG] Loaded ForceManualPass = " + std::to_string(g_ForceManualPass));
        }
        else if (key == "ButtonL2ToggleIndex") {
            g_ButtonL2ToggleIndex = std::stoi(value);
            Log("[CONFIG] Loaded ButtonL2ToggleIndex = " + std::to_string(g_ButtonL2ToggleIndex));
        }
        else if (key == "LeftStickThreshold") {
            g_LeftStickThreshold = std::stof(value);
            Log("[CONFIG] Loaded LeftStickThreshold = " + std::to_string(g_LeftStickThreshold));
        }
    }
    configFile.close();
    Log("[CONFIG] External configuration successfully parsed.");
}

// DirectInput8Create Function Pointer Profile
typedef HRESULT(WINAPI* LPDEDIRECTINPUT8CREATE)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
LPDEDIRECTINPUT8CREATE RealDirectInput8Create = nullptr;
HMODULE hSystemDInput = nullptr;

// Dynamically load the real Windows dinput8.dll from SysWOW64
void LoadRealDLL() {
    if (hSystemDInput) return;

    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\dinput8.dll");

    Log("[INFO] Loading system DLL from: " + std::string(sysPath));
    hSystemDInput = LoadLibraryA(sysPath);
    if (hSystemDInput) {
        RealDirectInput8Create = (LPDEDIRECTINPUT8CREATE)GetProcAddress(hSystemDInput, "DirectInput8Create");
        Log("[INFO] Successfully hooked system DirectInput8Create address.");
    }
    else {
        Log("[ERROR] Failed to load system dinput8.dll.");
    }
}

// ============================================================================
// 1. DEVICE PROXY CLASS (Intercepts and Logs Controller Data)
// ============================================================================
class MyDirectInputDevice8 : public IDirectInputDevice8A {
private:
    IDirectInputDevice8A* RealDevice;
    bool isJoystick = false;
    int deviceIndex = -1;
    
    // Stores the exact timestamp when a button was pressed
    std::chrono::steady_clock::time_point buttonXPressTimes; // ground pass
    std::chrono::steady_clock::time_point buttonOPressTimes; // lob pass
    BYTE previousButtonXState = 0;
    BYTE previousButtonOState = 0;
    BYTE previousButtonL2ToggleState = 0;
    bool simulatingL2Press = false;

public:
    MyDirectInputDevice8(IDirectInputDevice8A* original, bool joystickCheck, int index)
        : RealDevice(original), isJoystick(joystickCheck), deviceIndex(index) {}

    // The interceptor target: PES6 calls this every frame to get input states
    STDMETHODIMP GetDeviceState(DWORD cbData, LPVOID lpvData) override {
        // Let Windows fetch the real hardware input state first
        HRESULT hr = RealDevice->GetDeviceState(cbData, lpvData);

        if (!isJoystick) return hr;

        int currentActiveDeviceIndex = activeDeviceIndex;
        // If there is only one gamepad, it will be considered active device automatically.
        if (joyPadCount == 1) {
            currentActiveDeviceIndex = deviceIndex;
            // Log("[DEBUG] Only one controller, use it as active device.");
        }
        
        // We have target device but current device isn't the desired one.
        if (deviceIndex != currentActiveDeviceIndex && currentActiveDeviceIndex > 0) {
            // Log("[DEBUG] current device " + std::to_string(deviceIndex) + " isn't the desired device " + std::to_string(currentActiveDeviceIndex));
            return hr;
        }

        if (!SUCCEEDED(hr) || lpvData == nullptr || !isJoystick) return hr;

        if (!(cbData == sizeof(DIJOYSTATE) || cbData == sizeof(DIJOYSTATE2))) return hr;

        DIJOYSTATE* joyState = static_cast<DIJOYSTATE*>(lpvData);

        // Detect active device if needed
        if (currentActiveDeviceIndex == 0) {
            // Detect if right stick has real movement from the user
            bool hasRealRStickInput = std::abs(joyState->lRx - 32767) >= 32768 * g_RightStickThreshold || std::abs(joyState->lRy - 32767) >= 32768 * g_RightStickThreshold;
            // Ignore some cases where a inactive controller reports weird left stick position
            if (joyState->lRx == 0 || joyState->lRx == 65535 || joyState->lRy == 0 || joyState->lRy == 65535 || joyState->lRx == joyState->lRy) {
                hasRealRStickInput = false;
            }
            Log("[DEBUG] device " + std::to_string(deviceIndex) + " Has real right stick input ? " + std::to_string(hasRealRStickInput) + " x = " + std::to_string(joyState->lRx) + ", y = " + std::to_string(joyState->lRy));
            if (hasRealRStickInput) {
                Log("[INFO] Detected active device " + std::to_string(deviceIndex));
                activeDeviceIndex = deviceIndex;
                currentActiveDeviceIndex = deviceIndex;
            }
        }

        if (currentActiveDeviceIndex == 0) {
            // Log("[DEBUG] No active controller can be detected.");
            return hr;
        }

        if (g_GameVersion == 6) {
            GetDeviceState6(joyState);
        }
        else if (g_GameVersion == 11) {
            GetDeviceState11(joyState);
        }

        return hr;
    }

    void GetDeviceState11(DIJOYSTATE* joyState) {
        if (!g_ForceManualPass) {
            BYTE currentButtonL2ToggleState = joyState->rgbButtons[g_ButtonL2ToggleIndex];

            bool isL2TogglePressed = currentButtonL2ToggleState & 0x80;

            if (currentButtonL2ToggleState != previousButtonL2ToggleState) {
                if (isL2TogglePressed) {
                    Log("[DEBUG] Button L2 Toggle pressed.");
                    if (forceManualPass) forceManualPass = false; else forceManualPass = true;
                    Log("[INFO] Force manual pass? " + std::to_string(forceManualPass));
                }
                else {
                    Log("[DEBUG] Button L2 Toggle released.");
                }
            }
            previousButtonL2ToggleState = currentButtonL2ToggleState;
        }
        else {
            forceManualPass = true;
        }

        // Detect if left stick has real movement from the user
        bool hasRealLeftStickInput = std::abs(joyState->lX - 32767) >= 32768 * g_LeftStickThreshold || std::abs(joyState->lY - 32767) >= 32768 * g_LeftStickThreshold;
        
        BYTE currentButtonXState = joyState->rgbButtons[g_ButtonXIndex];
        BYTE currentButtonOState = joyState->rgbButtons[g_ButtonOIndex];

        bool isButtonXBeingPressed = currentButtonXState & 0x80;
        bool isButtonOBeingPressed = currentButtonOState & 0x80;

        if (currentButtonXState != previousButtonXState) {
            if (isButtonXBeingPressed) {
                Log("[DEBUG] Button X pressed.");
                if (hasRealLeftStickInput && forceManualPass) {
                    Log("[DEBUG] Left stick moved, start simulating L2 press");
                    simulatingL2Press = true;
                    // Simulate L2 press
                    joyState->lZ = 65535;
                }
                else {
                    simulatingL2Press = false;
                }
            }
            else {
                Log("[DEBUG] Button X released.");
                if (simulatingL2Press) {
                    // Simulate L2 press
                    joyState->lZ = 65535;
                    Log("[DEBUG] Stop simulating L2 press");
                }
                simulatingL2Press = false;            
            }
        }

        if (currentButtonOState != previousButtonOState) {
            if (isButtonOBeingPressed) {
                Log("[DEBUG] Button O pressed.");
                if (hasRealLeftStickInput && forceManualPass) {
                    Log("[DEBUG] Left stick moved, start simulating L2 press");
                    simulatingL2Press = true;
                    // Simulate L2 press
                    joyState->lZ = 65535;
                }
                else {
                    simulatingL2Press = false;
                }
            }
            else {
                Log("[DEBUG] Button O released.");
                if (simulatingL2Press) {
                    // Simulate L2 press
                    joyState->lZ = 65535;
                    Log("[DEBUG] Stop simulating L2 press");
                }
                simulatingL2Press = false;
            }
        }

        if (isButtonXBeingPressed || isButtonOBeingPressed) {
            if (simulatingL2Press) {
                std::string buttonName = "X";
                if (isButtonOBeingPressed) buttonName = "O";
                Log("[DEBUG] Simulating L2 press, " + buttonName + " is being pressed");
                // Simulate L2 press
                joyState->lZ = 65535;
            }
        }

        previousButtonXState = currentButtonXState;
        previousButtonOState = currentButtonOState;
    }

    void GetDeviceState6(DIJOYSTATE* joyState) {
        // --- CHECK IF L2 (Z+) IS PRESSED ---
        // E.g. Neutral center is 32767. If we use 45000 as a threshold 
        // to detect when the trigger is pushed roughly halfway or more.
        bool isL2Pressed = (joyState->lZ > 32767 + 32768 * g_ButtonL2Threshold);

        Log("[DEBUG] L2 is pressed? " + std::to_string(isL2Pressed) + ". Current value: " + std::to_string(joyState->lZ) + " threshold: " + std::to_string(32767 + 32768 * g_ButtonL2Threshold));

        
        if (!isL2Pressed) {
            return;
        }

        // From here we either simulate a ground pass or a lob pass
        BYTE currentButtonXState = joyState->rgbButtons[g_ButtonXIndex];
        BYTE currentButtonOState = joyState->rgbButtons[g_ButtonOIndex];

        bool isButtonXBeingPressed = currentButtonXState & 0x80;
        bool isButtonOBeingPressed = currentButtonOState & 0x80;

        if (currentButtonXState != previousButtonXState) {
            if (isButtonXBeingPressed) {
                Log("[DEBUG] Button X pressed.");
                buttonXPressTimes = std::chrono::steady_clock::now();
                // Reset L1 because we are simulating a ground pass
                joyState->rgbButtons[g_ButtonL1Index] = 0x00;
            }
            else {
                Log("[DEBUG] Button X released.");
                // Reset rstick
                joyState->lRx = 32767;
                joyState->lRy = 32767;
                joyState->rgbButtons[g_ButtonL1Index] = 0x00;
            }
        } 
        
        if (currentButtonOState != previousButtonOState) {
            if (isButtonOBeingPressed) {
                Log("[DEBUG] Button O pressed.");
                buttonOPressTimes = std::chrono::steady_clock::now();
                // Press L1 because we are simulating a lob pass
                joyState->rgbButtons[g_ButtonL1Index] = 0x80;
            }
            else {
                Log("[DEBUG] Button O released.");
                // Reset rstick
                joyState->lRx = 32767;
                joyState->lRy = 32767;
                joyState->rgbButtons[g_ButtonL1Index] = 0x80;
            }
        }

       if (isButtonXBeingPressed || isButtonOBeingPressed) {
            auto currentTime = std::chrono::steady_clock::now();

            // Calculate duration in milliseconds
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - buttonXPressTimes).count();
            if (isButtonOBeingPressed) {
                duration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - buttonOPressTimes).count();
            }
            if (duration > g_PassGaugeFillupTime) duration = g_PassGaugeFillupTime;
            Log("[DEBUG] Button press duration " + std::to_string(duration));

            // Set rstick position based on lstick angle and button A press duration
            
            // 1. Get current physical normalized coordinates
            float lx = (static_cast<float>(joyState->lX) - 32767.0f) / 32767.0f;
            float ly = ((static_cast<float>(joyState->lY) - 32767.0f) / 32767.0f) * -1.0f;
            // 2. Calculate current angle
            float radians = std::atan2(ly, lx);
            // 3. Target rstick radius based on button A press duration
            float targetRadius = static_cast<float>(duration) / static_cast<float>(g_PassGaugeFillupTime);
            // 4. Calculate new normalized coordinates matching the exact same angle
            float newLxNorm = targetRadius * std::cos(radians);
            float newLyNorm = targetRadius * std::sin(radians);
            // 5. Convert back to raw DirectInput values (0 to 65535)
            LONG rawNewX = static_cast<LONG>((newLxNorm * 32767.0f) + 32767.0f);
            LONG rawNewY = static_cast<LONG>((-newLyNorm * 32767.0f) + 32767.0f); // Note the negative sign for inverted Y

            // 6. Clamp values strictly to hardware boundaries just to prevent overflows
            if (rawNewX < 0) rawNewX = 0;
            if (rawNewX > 65535) rawNewX = 65535;
            if (rawNewY < 0) rawNewY = 0;
            if (rawNewY > 65535) rawNewY = 65535;

            // 7. Inject the modified values for rstick
            joyState->lRx = rawNewX;
            joyState->lRy = rawNewY;
            Log("[DEBUG] Simulating Right stick: x=" + std::to_string(rawNewX) + ", y=" + std::to_string(rawNewY) + ", angle=" + std::to_string(radians));

            if (isButtonXBeingPressed) {
                // Also reset L1 because we are simulating a ground pass
                joyState->rgbButtons[g_ButtonL1Index] = 0x00;
            }
            else if (isButtonOBeingPressed) {
                // Press L1 for a lob pass
                joyState->rgbButtons[g_ButtonL1Index] = 0x80;
            }
        }

        previousButtonXState = currentButtonXState;
        previousButtonOState = currentButtonOState;

        // Reset button X and O if needed because we are simulating rstick
        if (isButtonXBeingPressed) joyState->rgbButtons[g_ButtonXIndex] = 0x00;
        if (isButtonOBeingPressed) joyState->rgbButtons[g_ButtonOIndex] = 0x00;

        // Reset L2 as well, otherwise rstick pass won't be triggered by the game.
        if (isButtonXBeingPressed || isButtonOBeingPressed) joyState->lZ = 32767;

        // If X and O are not pressed, we don't change any button state so that regular PES command still works.
        return;
    }

    // Standard COM Boilerplate: Forward everything else directly to the system device
    STDMETHODIMP QueryInterface(REFIID riid, LPVOID* ppvObj) override { return RealDevice->QueryInterface(riid, ppvObj); }
    STDMETHODIMP_(ULONG) AddRef() override { return RealDevice->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = RealDevice->Release();
        if (count == 0) { delete this; return 0; }
        return count;
    }
    STDMETHODIMP GetCapabilities(LPDIDEVCAPS lpDiCaps) override { return RealDevice->GetCapabilities(lpDiCaps); }
    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) override { return RealDevice->EnumObjects(lpCallback, pvRef, dwFlags); }
    STDMETHODIMP GetProperty(REFGUID rguidProp, LPDIPROPHEADER pdiph) override { return RealDevice->GetProperty(rguidProp, pdiph); }
    STDMETHODIMP SetProperty(REFGUID rguidProp, LPCDIPROPHEADER pdiph) override { return RealDevice->SetProperty(rguidProp, pdiph); }
    STDMETHODIMP Acquire() override { return RealDevice->Acquire(); }
    STDMETHODIMP Unacquire() override { return RealDevice->Unacquire(); }
    STDMETHODIMP GetDeviceData(DWORD cbObjectSize, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override { return RealDevice->GetDeviceData(cbObjectSize, rgdod, pdwInOut, dwFlags); }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT lpdf) override { 
        // Log the data format size to see what layout the game expects
        Log("[INFO] Game requested SetDataFormat. Structure size: " + std::to_string(lpdf->dwDataSize));
        return RealDevice->SetDataFormat(lpdf); 
    }
    STDMETHODIMP SetEventNotification(HANDLE hEvent) override { return RealDevice->SetEventNotification(hEvent); }
    STDMETHODIMP SetCooperativeLevel(HWND hwnd, DWORD dwFlags) override { return RealDevice->SetCooperativeLevel(hwnd, dwFlags); }
    STDMETHODIMP RunControlPanel(HWND hwndOwner, DWORD dwFlags) override { return RealDevice->RunControlPanel(hwndOwner, dwFlags); }
    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion, REFGUID rguid) override { return RealDevice->Initialize(hinst, dwVersion, rguid); }
    STDMETHODIMP CreateEffect(REFGUID rguid, LPCDIEFFECT lpeff, LPDIRECTINPUTEFFECT* ppdeff, LPUNKNOWN punkOuter) override { return RealDevice->CreateEffect(rguid, lpeff, ppdeff, punkOuter); }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA lpCallback, LPVOID pvRef, DWORD dwEffType) override { return RealDevice->EnumEffects(lpCallback, pvRef, dwEffType); }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA pdei, REFGUID rguid) override { return RealDevice->GetEffectInfo(pdei, rguid); }
    STDMETHODIMP GetForceFeedbackState(LPDWORD pdwOut) override { return RealDevice->GetForceFeedbackState(pdwOut); }
    STDMETHODIMP SendForceFeedbackCommand(DWORD dwFlags) override { return RealDevice->SendForceFeedbackCommand(dwFlags); }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK lpCallback, LPVOID pvRef, DWORD dwFlags) override { return RealDevice->EnumCreatedEffectObjects(lpCallback, pvRef, dwFlags); }
    STDMETHODIMP Escape(LPDIEFFESCAPE pesc) override { return RealDevice->Escape(pesc); }
    STDMETHODIMP Poll() override { return RealDevice->Poll(); }
    STDMETHODIMP SendDeviceData(DWORD cbObjectSize, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) override { return RealDevice->SendDeviceData(cbObjectSize, rgdod, pdwInOut, dwFlags); }
    STDMETHODIMP EnumEffectsInFile(LPCSTR lpszFileName, LPDIENUMEFFECTSINFILECALLBACK ecf, LPVOID pvRef, DWORD dwFlags) override { return RealDevice->EnumEffectsInFile(lpszFileName, ecf, pvRef, dwFlags); }
    STDMETHODIMP WriteEffectToFile(LPCSTR lpszFileName, DWORD dwEntries, LPDIFILEEFFECT pdien, DWORD dwFlags) override { return RealDevice->WriteEffectToFile(lpszFileName, dwEntries, pdien, dwFlags); }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA lpdiDeviceImageInfoHeader) override { return RealDevice->GetImageInfo(lpdiDeviceImageInfoHeader); }
    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA pdidoi, DWORD dwObj, DWORD dwHow) override { return RealDevice->GetObjectInfo(pdidoi, dwObj, dwHow); }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATA lpdiActionFormat, LPCSTR lpszUserName, DWORD dwFlags) override {
        return RealDevice->SetActionMap(lpdiActionFormat, lpszUserName, dwFlags);
    }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATA lpdiaf, LPCSTR lpszUserName, DWORD dwFlags) override {
        return RealDevice->BuildActionMap(lpdiaf, lpszUserName, dwFlags);
    }
    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA pdidi) override {
        return RealDevice->GetDeviceInfo(pdidi);
    }
};

// ============================================================================
// 2. INTERFACE PROXY CLASS (Intercepts device provisioning requests)
// ============================================================================
class MyDirectInput8 : public IDirectInput8A {
private:
    IDirectInput8A* RealInput;

public:
    MyDirectInput8(IDirectInput8A* original) : RealInput(original) {}

    // Intercept device instantiation so we can inject our custom device layer
    STDMETHODIMP CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A* lplpDirectInputDevice, LPUNKNOWN pUnkOuter) override {
        LPDIRECTINPUTDEVICE8A originalDevice = nullptr;
        HRESULT hr = RealInput->CreateDevice(rguid, &originalDevice, pUnkOuter);

        if (SUCCEEDED(hr) && originalDevice != nullptr) {
            // Determine if the initialized hardware device is a system joystick or mouse/keyboard
            bool isJoy = (rguid != GUID_SysKeyboard && rguid != GUID_SysMouse);
            int assignedIndex = -1;

            if (isJoy) {  
                joyPadCount++;
                assignedIndex = joyPadCount;
                Log("[INFO] Game requested initialization of a Gamepad/Joystick device " + std::to_string(assignedIndex) + ".");
                // Fetch the precise hardware instance names
                DIDEVICEINSTANCEA di;
                ZeroMemory(&di, sizeof(DIDEVICEINSTANCEA));
                di.dwSize = sizeof(DIDEVICEINSTANCEA);
                if (SUCCEEDED(originalDevice->GetDeviceInfo(&di))) {
                    Log("[INFO] Name: " + std::string(di.tszInstanceName) +
                        " | Product: " + std::string(di.tszProductName));
                }
            }
            else {
                Log("[INFO] Game requested initialization of a Mouse or Keyboard device " + std::to_string(assignedIndex) + ".");
            }
            // Swap the real device pointer with our custom wrapper class
            *lplpDirectInputDevice = new MyDirectInputDevice8(originalDevice, isJoy, assignedIndex);
            return DI_OK;
        }
        return hr;
    }

    // Forward boilerplate methods to the original input interface
    STDMETHODIMP QueryInterface(REFIID riid, LPVOID* ppvObj) override { return RealInput->QueryInterface(riid, ppvObj); }
    STDMETHODIMP_(ULONG) AddRef() override { return RealInput->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = RealInput->Release();
        if (count == 0) { delete this; return 0; }
        return count;
    }
    STDMETHODIMP EnumDevices(DWORD dwDevType, LPDIENUMDEVICESCALLBACKA lpCallback, LPVOID pvRef, DWORD dwFlags) override { return RealInput->EnumDevices(dwDevType, lpCallback, pvRef, dwFlags); }
    STDMETHODIMP RunControlPanel(HWND hwndOwner, DWORD dwFlags) override { return RealInput->RunControlPanel(hwndOwner, dwFlags); }
    STDMETHODIMP Initialize(HINSTANCE hinst, DWORD dwVersion) override { return RealInput->Initialize(hinst, dwVersion); }
    STDMETHODIMP FindDevice(REFGUID rguidClass, LPCSTR ptszName, LPGUID pguidInstance) override { return RealInput->FindDevice(rguidClass, ptszName, pguidInstance); }
    STDMETHODIMP EnumDevicesBySemantics(LPCSTR ptszUserName, LPDIACTIONFORMATA rgdof, LPDIENUMDEVICESBYSEMANTICSCBA lpCallback, LPVOID pvRef, DWORD dwFlags) override { return RealInput->EnumDevicesBySemantics(ptszUserName, rgdof, lpCallback, pvRef, dwFlags); }
    STDMETHODIMP GetDeviceStatus(REFGUID rguidInstance) override { return RealInput->GetDeviceStatus(rguidInstance); }
    STDMETHODIMP ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK lpdiCallback, LPDICONFIGUREDEVICESPARAMSA lpdiParams, DWORD dwFlags, LPVOID pvRef) override { return RealInput->ConfigureDevices(lpdiCallback, lpdiParams, dwFlags, pvRef); }
};

// ============================================================================
// 3. MAIN EXPORTED DLL HOOK (The entry point called by PES6)
// ============================================================================
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    Log("[INFO] Game execution reached proxy DirectInput8Create hook.");

    if (!RealDirectInput8Create) {
        LoadRealDLL();
    }

    if (!RealDirectInput8Create) {
        Log("[ERROR] Could not assign real system DirectInput8Create pointer.");
        return DIERR_GENERIC;
    }

    LPDIRECTINPUT8A originalDirectInput = nullptr;
    HRESULT hr = RealDirectInput8Create(hinst, dwVersion, riidltf, (LPVOID*)&originalDirectInput, punkOuter);

    if (SUCCEEDED(hr) && originalDirectInput != nullptr) {
        Log("[INFO] Successfully wrapped native IDirectInput8 engine interface.");
        *ppvOut = new MyDirectInput8(originalDirectInput);
        return DI_OK;
    }

    Log("[ERROR] System DirectInput8Create initialization call failed.");
    return hr;
}

// Basic DLL Entry Point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        Log("[INFO] Attaching proxy engine. Openning log stream.");
        DisableThreadLibraryCalls(hModule);
        InitLogging();
        LoadConfiguration();
        activeDeviceIndex = g_TargetDeviceIndex;
        break;
    case DLL_PROCESS_DETACH:
        Log("[INFO] Detaching proxy engine. Closing log stream.");
        if (logFile.is_open()) {
            logFile.close();
        }
        if (hSystemDInput) {
            FreeLibrary(hSystemDInput);
        }
        break;
    }
    return TRUE;
}