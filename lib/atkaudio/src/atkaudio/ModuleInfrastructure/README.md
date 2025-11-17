# Module Infrastructure

This directory contains reusable infrastructure for building audio processing modules that integrate with OBS, JUCE, AudioServer, and MidiServer.

## Directory Structure

```
ModuleInfrastructure/
├── AudioServer/         # Cross-platform audio device management
├── MidiServer/          # MIDI routing and management
├── Bridge/              # Reusable device bridge components
│   ├── ModuleAudioDevice.h            # Base OBS audio device
│   ├── ModuleAudioServerDevice.h      # AudioServer device bridge
│   ├── ModuleAudioIODeviceType.h      # Device type manager
│   ├── ModuleDeviceManager.h          # High-level device manager
│   └── ModuleBridge.h                 # Convenience header
└── README.md            # This file
```

## Components

### AudioServer

Cross-platform audio device abstraction that provides unified access to:

- ASIO (Windows)
- CoreAudio (macOS)
- ALSA (Linux)
- Windows Audio (WDM/WASAPI)

### MidiServer

MIDI routing system that provides:

- Lock-free MIDI message queues
- Device subscription management
- Realtime-safe MIDI I/O
- Virtual MIDI injection

### Bridge

Reusable components for integrating JUCE AudioDeviceManager with external audio sources:

#### ModuleDeviceCoordinator

Per-module instance that ensures only one device is active at a time **within that specific module instance**. This prevents a single module from having multiple devices (e.g., OBS Audio + ASIO) processing simultaneously. Each `ModuleAudioIODeviceType` has its own coordinator. Multiple modules can coexist independently, each registering their active device with AudioServer for concurrent callbacks.

#### ModuleOBSAudioDevice

Base class for OBS audio devices. Provides:

- OBS channel configuration
- OBS sample rate handling
- Realtime-safe audio processing
- Active channel management

#### ModuleAudioServerDevice

Bridge between AudioServer devices and JUCE AudioDeviceManager. Enables module to process audio from professional audio interfaces.

#### ModuleAudioIODeviceType

JUCE AudioIODeviceType that manages both OBS and AudioServer devices. Can be extended to create custom device types.

#### ModuleDeviceManager

High-level manager that encapsulates the entire device + MIDI integration pattern. Handles:

- Device type registration
- AudioDeviceManager initialization
- OBS device opening
- Device change tracking
- Realtime-safe external audio processing
- MIDI client lifecycle

## Usage Patterns

### Simple Usage (Recommended)

For most use cases, use `ModuleDeviceManager`:

```cpp
#include <atkaudio/ModuleInfrastructure/Bridge/ModuleBridge.h>

class MyModule {
    juce::AudioDeviceManager deviceManager;
    atk::ModuleDeviceManager moduleDeviceManager;

    MyModule()
        : moduleDeviceManager(
            std::make_unique<atk::ModuleAudioIODeviceType>("MyModule Audio"),
            deviceManager
        )
    {
        // Initialize device management
        if (moduleDeviceManager.initialize())
            moduleDeviceManager.openOBSDevice();
    }

    void process(float** buffer, int channels, int samples, double sampleRate) {
        // Process external audio (realtime-safe)
        moduleDeviceManager.processExternalAudio(buffer, channels, samples, sampleRate);
    }

    atk::MidiClient& getMidiClient() {
        return moduleDeviceManager.getMidiClient();
    }
};
```

### Custom Device Types

To customize device behavior, extend the base classes:

```cpp
// Custom OBS device with specialized processing
class MyOBSDevice : public atk::ModuleOBSAudioDevice {
public:
    MyOBSDevice(const juce::String& name)
        : ModuleOBSAudioDevice(name, "MyModule Audio") {}

    void processExternalAudio(...) override {
        // Custom processing logic
        ModuleOBSAudioDevice::processExternalAudio(...);
    }
};

// Custom device type that creates custom devices
class MyDeviceType : public atk::ModuleAudioIODeviceType {
public:
    MyDeviceType() : ModuleAudioIODeviceType("MyModule Audio") {}

protected:
    juce::AudioIODevice* createOBSDevice(const juce::String& deviceName) override {
        return new MyOBSDevice(deviceName);
    }
};

// Use custom device type
atk::ModuleDeviceManager moduleDeviceManager(
    std::make_unique<MyDeviceType>(),
    deviceManager
);
```

### Manual Integration

For maximum control, use the components directly:

```cpp
class MyModule : public juce::ChangeListener {
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<atk::ModuleAudioIODeviceType> deviceType;
    std::atomic<atk::ModuleOBSAudioDevice*> obsDevice{nullptr};
    atk::MidiClient midiClient;

    MyModule() {
        // Create and register device type
        deviceType = std::make_unique<atk::ModuleAudioIODeviceType>("MyModule Audio");
        deviceManager.addAudioDeviceType(
            std::unique_ptr<juce::AudioIODeviceType>(deviceType.release())
        );

        // Initialize
        deviceManager.initialise(256, 256, nullptr, true);
        deviceManager.addChangeListener(this);

        // Open OBS device
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = "OBS Audio";
        setup.inputDeviceName = "OBS Audio";
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        deviceManager.setAudioDeviceSetup(setup, true);

        // Track device
        if (auto* device = deviceManager.getCurrentAudioDevice())
            obsDevice.store(dynamic_cast<atk::ModuleOBSAudioDevice*>(device),
                          std::memory_order_release);
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override {
        // Update device pointer on device changes
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device && device->getName() == "OBS Audio")
            obsDevice.store(dynamic_cast<atk::ModuleOBSAudioDevice*>(device),
                          std::memory_order_release);
        else
            obsDevice.store(nullptr, std::memory_order_release);
    }

    void process(float** buffer, int channels, int samples, double sampleRate) {
        if (auto* device = obsDevice.load(std::memory_order_acquire))
            device->processExternalAudio(buffer, channels, samples, sampleRate);
    }
};
```

## MIDI Integration

All usage patterns automatically include MIDI integration via `MidiClient`:

```cpp
// In your audio graph processing
void processAudio(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    // Get MIDI input
    moduleDeviceManager.getMidiClient().getPendingMidi(
        midi, 
        buffer.getNumSamples(), 
        getSampleRate()
    );

    // ... process audio and MIDI ...

    // Send MIDI output
    moduleDeviceManager.getMidiClient().sendMidi(midi);
}

// Virtual MIDI injection (e.g., from virtual keyboard)
void injectMIDI(const juce::MidiBuffer& virtualMidi) {
    moduleDeviceManager.getMidiClient().injectMidi(virtualMidi);
}

// Manage subscriptions
atk::MidiClientState state;
state.subscribedInputDevices.add("MIDI Keyboard");
state.subscribedOutputDevices.add("Virtual Synth");
moduleDeviceManager.getMidiClient().setSubscriptions(state);
```

## Thread Safety

- **Audio Thread**: `processExternalAudio()` and MIDI client methods are realtime-safe
- **Message Thread**: Device changes, subscriptions, and manager lifecycle
- **Atomic Device Pointer**: Safe access from any thread via `std::atomic`

## Future Modules

This infrastructure is designed to be reusable:

- PluginHost2 (current)
- PluginHost3 (planned)
- Any custom audio processing module that needs OBS/AudioServer integration

Simply instantiate `ModuleDeviceManager` with your custom device type and you get the full integration for free.

### Multiple Module Coexistence

Each `ModuleAudioIODeviceType` has its own `ModuleDeviceCoordinator` instance, which means:

- ✅ Multiple modules can coexist (e.g., PluginHost2 and PluginHost3 running simultaneously)
- ✅ Each module independently registers its active device callback with AudioServer
- ✅ Each module's coordinator ensures only one device is active **per module** (not globally)
- ✅ Multiple modules can process audio concurrently via their AudioServer callbacks
- ✅ No global state or singletons
- ✅ Modules don't interfere with each other

Example: PluginHost2 uses OBS Audio, PluginHost3 uses ASIO - both process audio simultaneously.

This allows for flexible module composition and testing.
