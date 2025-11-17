# Multiple Module Support

## Overview

The ModuleInfrastructure has been designed to support multiple independent module instances coexisting simultaneously, such as PluginHost2 and PluginHost3 running at the same time.

## Architecture

### No Global Singleton

The `ModuleDeviceCoordinator` is **NOT** a global singleton. Instead:

- Each `ModuleAudioIODeviceType` instance creates its own `ModuleDeviceCoordinator`
- The coordinator is stored as a `shared_ptr` and passed to all devices created by that type
- Devices within a module share the same coordinator instance
- Different modules have completely independent coordinators

### Coordinator Lifetime

```cpp
ModuleAudioIODeviceType
    └── coordinator (shared_ptr<ModuleDeviceCoordinator>)
        ├── passed to ModuleOBSAudioDevice
        └── passed to ModuleAudioServerDevice(s)
```

When the `ModuleAudioIODeviceType` is destroyed, all devices it created will hold references to the coordinator, keeping it alive until all devices are destroyed.

## Example: Multiple Modules

```cpp
// Module 1: PluginHost2
class PluginHost2::Impl {
    juce::AudioDeviceManager deviceManager1;
    atk::ModuleDeviceManager moduleDeviceManager1;
    
    Impl()
        : moduleDeviceManager1(
            std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost2 Audio"),
            deviceManager1
        )
    {
        moduleDeviceManager1.initialize();
        moduleDeviceManager1.openOBSDevice();
    }
};

// Module 2: PluginHost3  
class PluginHost3::Impl {
    juce::AudioDeviceManager deviceManager2;
    atk::ModuleDeviceManager moduleDeviceManager2;
    
    Impl()
        : moduleDeviceManager2(
            std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost3 Audio"),
            deviceManager2
        )
    {
        moduleDeviceManager2.initialize();
        moduleDeviceManager2.openOBSDevice();
    }
};

// Both can coexist and operate independently!
PluginHost2 host2;
PluginHost3 host3;

// Each has its own:
// - AudioDeviceManager
// - ModuleDeviceCoordinator
// - Set of devices
// - MidiClient
// - Independent state
```

## Benefits

### Independent Operation

- Module A switching devices doesn't affect Module B
- Each module can have different active devices
- No contention between modules

### Concurrent AudioServer Access

- Multiple modules can register callbacks with AudioServer simultaneously
- Each module registers its **active device's** callback with AudioServer
- AudioServer routes hardware audio to **all registered callbacks** concurrently
- Each module processes audio independently from the same hardware source

### Clean Separation

- No shared mutable state between modules
- Each module is self-contained
- Easy to reason about lifecycle

### Testing

- Can instantiate multiple modules in tests
- Can test module interactions
- Can verify isolation

## Coordinator Scope

The coordinator only coordinates **within a single module instance**:

- ✅ Prevents multiple devices **in the same module instance** from being active simultaneously
  - Example: Module A can't have both "OBS Audio" AND "ASIO Device" active at once
- ✅ Ensures only one device is active **per module instance**
- ✅ Each module's active device can register its callback with AudioServer
- ❌ Does NOT prevent different modules from processing simultaneously
  - Example: Module A with "OBS Audio" and Module B with "ASIO Device" both process concurrently
- ❌ Does NOT coordinate across module boundaries
- ❌ Does NOT limit AudioServer to one callback

This is the correct behavior because:

1. AudioServer is designed to handle multiple concurrent registered callbacks (one per module)
2. Each module registers its active device's callback with AudioServer independently
3. OBS audio processing is per-filter-instance
4. Each module represents a separate audio processing unit that should operate independently

## Thread Safety

### Within a Module Instance

- The coordinator uses a `CriticalSection` for thread-safe device activation
- Devices check `coordinator->isActive(this)` in realtime code
- Only one device per module instance is active and registered with AudioServer at a time
- Example: Module A cannot have both OBS Audio AND ASIO active simultaneously

### Across Module Instances

- No coordination needed between different module coordinators
- Each module registers its active device's callback with AudioServer independently
- AudioServer calls all registered callbacks concurrently
- Example: Module A (OBS Audio callback) and Module B (ASIO callback) both process simultaneously
- No shared locks or contention between modules

## Memory Management

The `shared_ptr<ModuleDeviceCoordinator>` ensures:

- Coordinator lives as long as any device references it
- Automatic cleanup when last device is destroyed
- No manual lifetime management needed
- No dangling pointers

## Migration from Singleton

### Old (Incorrect)

```cpp
class ModuleDeviceCoordinator {
public:
    static ModuleDeviceCoordinator& getInstance() {
        static ModuleDeviceCoordinator instance;
        return instance;
    }
};

// In devices:
ModuleDeviceCoordinator::getInstance().tryActivate(this);
```

**Problem**: All modules share one global coordinator, preventing coexistence.

### New (Correct)

```cpp
class ModuleDeviceCoordinator {
    // Regular class, not singleton
};

class ModuleAudioIODeviceType {
    std::shared_ptr<ModuleDeviceCoordinator> coordinator;
};

// In devices:
coordinator->tryActivate(this);  // Uses instance coordinator
```

**Benefit**: Each module has its own coordinator, enabling coexistence.

## Use Cases

### Development

- Run old and new versions side-by-side
- Compare behavior during migration
- A/B testing of different implementations

### Production

- Multiple audio processing chains
- Different processing for different sources
- Specialized modules for specific tasks

### Testing

- Unit tests can create multiple modules
- Integration tests with multiple modules
- Isolation verification

## Summary

The ModuleInfrastructure supports multiple independent module instances through:

- Per-module coordinator instances (not global singleton)
- Shared pointer lifetime management
- Independent device management per module
- No cross-module coordination

This design enables flexible module composition while maintaining clean separation and thread safety within each module.
