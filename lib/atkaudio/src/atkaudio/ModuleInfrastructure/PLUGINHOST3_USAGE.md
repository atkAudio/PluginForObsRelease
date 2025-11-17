# Using ModuleInfrastructure in PluginHost3

This document shows how to use the new `ModuleInfrastructure` to quickly build a PluginHost3 or any other audio processing module.

## Quick Start

### Minimal Implementation

The simplest possible implementation using `ModuleDeviceManager`:

```cpp
// PluginHost3.h
#pragma once
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>
#include <juce_audio_devices/juce_audio_devices.h>

namespace atk {

class PluginHost3 {
public:
    PluginHost3();
    ~PluginHost3();
    
    void process(float** buffer, int numChannels, int numSamples, double sampleRate);
    void getState(std::string& s);
    void setState(std::string& s);
    juce::Component* getWindowComponent();

private:
    struct Impl;
    Impl* pImpl;
};

} // namespace atk
```

```cpp
// PluginHost3.cpp
#include "PluginHost3.h"
#include "UI/MainHostWindow3.h"

struct atk::PluginHost3::Impl {
    std::unique_ptr<MainHostWindow3> mainHostWindow;
    juce::AudioDeviceManager deviceManager;
    atk::ModuleDeviceManager moduleDeviceManager;
    
    Impl()
        : mainHostWindow(new MainHostWindow3())
        , moduleDeviceManager(
            std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost3 Audio"),
            deviceManager
        )
    {
        // Initialize device management - that's it!
        if (moduleDeviceManager.initialize())
            moduleDeviceManager.openOBSDevice();
        
        mainHostWindow->setVisible(false);
    }
    
    ~Impl() {
        // Clean shutdown - ModuleDeviceManager handles everything
    }
    
    void process(float** buffer, int numChannels, int numSamples, double sampleRate) {
        // One line - realtime-safe processing
        moduleDeviceManager.processExternalAudio(buffer, numChannels, numSamples, sampleRate);
    }
    
    juce::Component* getWindowComponent() {
        return mainHostWindow.get();
    }
    
    atk::MidiClient& getMidiClient() {
        return moduleDeviceManager.getMidiClient();
    }
    
    juce::AudioDeviceManager& getDeviceManager() {
        return moduleDeviceManager.getAudioDeviceManager();
    }
};

atk::PluginHost3::PluginHost3() : pImpl(new Impl()) {}
atk::PluginHost3::~PluginHost3() { delete pImpl; }

void atk::PluginHost3::process(float** buffer, int numChannels, int numSamples, double sampleRate) {
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

// ... other methods ...
```

## What You Get For Free

By using `ModuleDeviceManager`, you automatically get:

1. **OBS Audio Device**
   - Automatic OBS channel configuration
   - OBS sample rate handling
   - Realtime-safe audio processing

2. **AudioServer Integration**
   - ASIO, CoreAudio, ALSA, Windows Audio devices
   - Professional audio interface support
   - Device enumeration and selection

3. **Device Coordination**
   - Only one device active at a time
   - Automatic device switching
   - Thread-safe device access

4. **MIDI Integration**
   - MidiClient for MIDI I/O
   - Lock-free MIDI queues
   - Device subscription management
   - Virtual MIDI injection

5. **State Management**
   - Device configuration persistence
   - MIDI subscription persistence
   - Automatic restoration

## Advanced Usage

### Custom Device Behavior

If you need custom audio processing in your OBS device:

```cpp
// Custom OBS device for PluginHost3
class PluginHost3OBSDevice : public atk::ModuleOBSAudioDevice {
public:
    PluginHost3OBSDevice(const juce::String& name, MainHostWindow3& window)
        : ModuleOBSAudioDevice(name, "PluginHost3 Audio")
        , mainWindow(window)
    {
    }
    
    void processExternalAudio(...) override {
        // Custom pre-processing
        doCustomStuff();
        
        // Call base implementation
        ModuleOBSAudioDevice::processExternalAudio(...);
        
        // Custom post-processing
        doMoreCustomStuff();
    }

private:
    MainHostWindow3& mainWindow;
};

// Custom device type that creates your custom devices
class PluginHost3DeviceType : public atk::ModuleAudioIODeviceType {
public:
    PluginHost3DeviceType(MainHostWindow3& window)
        : ModuleAudioIODeviceType("PluginHost3 Audio")
        , mainWindow(window)
    {
    }

protected:
    juce::AudioIODevice* createOBSDevice(const juce::String& deviceName) override {
        return new PluginHost3OBSDevice(deviceName, mainWindow);
    }

private:
    MainHostWindow3& mainWindow;
};

// Use in Impl constructor
Impl()
    : mainHostWindow(new MainHostWindow3())
    , moduleDeviceManager(
        std::make_unique<PluginHost3DeviceType>(*mainHostWindow),
        deviceManager
    )
{
    // ...
}
```

### Direct Access to Components

If you need finer control:

```cpp
// Access the AudioDeviceManager
auto& dm = moduleDeviceManager.getAudioDeviceManager();
dm.setAudioDeviceSetup(customSetup, true);

// Access the MidiClient
auto& midi = moduleDeviceManager.getMidiClient();
atk::MidiClientState state;
state.subscribedInputDevices.add("My MIDI Device");
midi.setSubscriptions(state);

// Access the OBS device (not realtime-safe!)
if (auto* obsDevice = moduleDeviceManager.getOBSDevice()) {
    // Query device properties
    int channels = obsDevice->getActiveOutputChannels().countNumberOfSetBits();
}
```

## Migration from PluginHost2

### Before (PluginHost2 pattern)

```cpp
// Lots of boilerplate code
std::unique_ptr<Ph2AudioIODeviceType> ph2DeviceType;
std::atomic<Ph2AudioIODevice*> obsDevice{nullptr};

// Manual registration
mainHostWindow->getDeviceManager().addAudioDeviceType(
    std::unique_ptr<juce::AudioIODeviceType>(ph2DeviceType.release())
);

// Manual initialization
auto error = dm.initialise(256, 256, nullptr, true);
dm.addChangeListener(this);

// Manual device opening
juce::AudioDeviceManager::AudioDeviceSetup setup;
setup.outputDeviceName = "OBS Audio";
// ... more setup ...
error = dm.setAudioDeviceSetup(setup, true);

// Manual pointer tracking
if (auto* currentDevice = dm.getCurrentAudioDevice())
    obsDevice.store(dynamic_cast<Ph2AudioIODevice*>(currentDevice), 
                   std::memory_order_release);

// Manual cleanup in destructor
obsDevice.store(nullptr, std::memory_order_release);
auto lambda = [this] {
    window->getDeviceManager().removeChangeListener(this);
};
juce::MessageManager::callAsync(lambda);

// Manual change listener
void changeListenerCallback(juce::ChangeBroadcaster* source) override {
    auto* currentDevice = dm.getCurrentAudioDevice();
    if (currentDevice && currentDevice->getName() == "OBS Audio")
        obsDevice.store(dynamic_cast<Ph2AudioIODevice*>(currentDevice), ...);
    else
        obsDevice.store(nullptr, ...);
}
```

### After (PluginHost3 pattern)

```cpp
// Create manager - that's it!
atk::ModuleDeviceManager moduleDeviceManager(
    std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost3 Audio"),
    deviceManager
);

// Initialize and open device
moduleDeviceManager.initialize();
moduleDeviceManager.openOBSDevice();

// Use for processing
moduleDeviceManager.processExternalAudio(buffer, channels, samples, sampleRate);

// Access components
auto& midi = moduleDeviceManager.getMidiClient();
auto& dm = moduleDeviceManager.getAudioDeviceManager();

// Cleanup is automatic - just destroy the manager
```

**Result**: ~150 lines of boilerplate reduced to ~5 lines of code!

## Testing a New Module

1. Create your module class
2. Add `ModuleDeviceManager` as a member
3. Initialize it in constructor
4. Call `processExternalAudio()` in your process method
5. Access MIDI via `getMidiClient()`
6. Done!

No need to:

- Implement `ChangeListener`
- Manage atomic pointers
- Handle device coordinator
- Write cleanup code
- Implement state persistence boilerplate

## Future Extensibility

The infrastructure is designed for future enhancements:

- **Custom device types**: Override `createOBSDevice()` or `createAudioServerDevice()`
- **Custom processing**: Override `processExternalAudio()` in device classes
- **New server types**: Add new device bridges following the pattern
- **Plugin-specific features**: Extend `ModuleDeviceManager` with custom methods

All while maintaining the simple usage pattern for common cases.
