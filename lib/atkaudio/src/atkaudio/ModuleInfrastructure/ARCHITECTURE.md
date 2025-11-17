# ModuleInfrastructure Architecture

This document describes the architecture of the reusable module infrastructure that was extracted from PluginHost2.

## Problem

PluginHost2 had hard-coded patterns for:

- Integrating with OBS Audio
- Integrating with AudioServer devices
- Integrating with MidiServer
- Managing device lifecycle and thread safety

This code was specific to PluginHost2 but the pattern is useful for any audio processing module.

## Solution

Extract the patterns into reusable, generalized components in a common infrastructure that can be used by PluginHost2, PluginHost3, and any future modules.

## Self-Contained Composite Pattern

**ModuleDeviceManager is designed as a fully self-contained composite by default:**

- ✅ **Internal MidiClient**: Creates and owns its own MidiClient automatically
- ✅ **No external dependencies**: Works out-of-the-box without requiring external components
- ✅ **Optional external MidiClient**: Can optionally accept an external MidiClient pointer for integration with existing code

This design makes ModuleInfrastructure easy to use in new code while remaining flexible for complex integration scenarios.

## Architecture

### Directory Structure

```
lib/atkaudio/src/atkaudio/
└── ModuleInfrastructure/           # Common infrastructure root
    ├── AudioServer/                # Audio device abstraction (moved)
    ├── MidiServer/                 # MIDI routing (moved)
    └── Bridge/                     # Device bridge components (new)
        ├── ModuleAudioDevice.h           # Base OBS audio device
        ├── ModuleAudioServerDevice.h     # AudioServer device bridge  
        ├── ModuleAudioIODeviceType.h     # Device type manager
        ├── ModuleDeviceManager.h         # High-level manager
        └── ModuleBridge.h                # Convenience header
```

### Component Hierarchy

```
ModuleDeviceManager (high-level API)
    ├── ModuleAudioIODeviceType (device enumeration)
    │   ├── ModuleOBSAudioDevice (OBS audio bridge)
    │   └── ModuleAudioServerDevice (AudioServer bridge)
    ├── ModuleDeviceCoordinator (singleton, device arbitration)
    ├── juce::AudioDeviceManager (JUCE device management)
    └── atk::MidiClient (MIDI I/O)
```

### Design Patterns

#### 1. Bridge Pattern

- `ModuleOBSAudioDevice`: Bridges OBS audio → JUCE AudioIODevice
- `ModuleAudioServerDevice`: Bridges AudioServer → JUCE AudioIODevice

#### 2. Coordinator Pattern

- `ModuleDeviceCoordinator`: Per-module-instance coordinator ensures only one device is active **within that module**
- Thread-safe activation/deactivation
- Prevents a single module from having multiple devices (e.g., OBS + ASIO) active simultaneously
- Multiple modules can coexist, each with their own coordinator, each registering hardware callbacks with AudioServer
- Does NOT prevent different modules from processing concurrently

#### 3. Manager Pattern

- `ModuleDeviceManager`: Encapsulates the entire device + MIDI setup
- Handles initialization, lifecycle, and cleanup
- Provides high-level API for common operations

#### 4. Template Method Pattern

- `ModuleAudioIODeviceType::createOBSDevice()`: Override to customize
- `ModuleAudioIODeviceType::createAudioServerDevice()`: Override to customize
- Allows specialization while maintaining common structure

## Key Features

### 1. Realtime Safety

- Atomic device pointers for lock-free reads
- Lock-free MIDI queues
- No allocations in audio thread

### 2. Thread Safety

- Coordinator uses JUCE CriticalSection
- ChangeListener runs on message thread
- Atomic operations for device pointer updates

### 3. Lifecycle Management

- Automatic cleanup via destructors
- Async cleanup for JUCE components
- Proper resource deallocation order

### 4. Extensibility

- Virtual methods for customization
- Template method pattern for device creation
- Base classes for custom behavior

### 5. Simplicity

- High-level API hides complexity
- One-line initialization
- Automatic state management

## Usage Levels

### Level 1: Simple (Recommended)

Use `ModuleDeviceManager` - everything is automatic:

```cpp
ModuleDeviceManager mgr(...);
mgr.initialize();
mgr.openOBSDevice();
mgr.processExternalAudio(...);
```

### Level 2: Customized

Extend base classes for custom behavior:

```cpp
class MyOBSDevice : public ModuleOBSAudioDevice { ... };
class MyDeviceType : public ModuleAudioIODeviceType { ... };
ModuleDeviceManager mgr(std::make_unique<MyDeviceType>(), ...);
```

### Level 3: Manual

Use components directly for maximum control:

```cpp
ModuleAudioIODeviceType deviceType;
audioDeviceManager.addAudioDeviceType(...);
// Manual initialization and management
```

## Benefits

### Code Reduction

- PluginHost2 specific code: ~150 lines
- Reusable infrastructure: ~10 lines per module
- **90%+ reduction in boilerplate**

### Maintainability

- Single source of truth for patterns
- Fix bugs in one place
- Consistent behavior across modules

### Testability

- Components can be tested independently
- Mock-friendly interfaces
- Clear separation of concerns

### Reusability

- Drop-in solution for new modules
- Consistent API across modules
- Shared infrastructure reduces duplication

## Migration Path

### Step 1: PluginHost2 (Current)

- Uses new infrastructure via adapted types
- Maintains backward compatibility
- Proves infrastructure works

### Step 2: PluginHost3 (Future)

- Built from ground up with new infrastructure
- Cleaner implementation
- Example for future modules

### Step 3: Other Modules (Future)

- Any audio processing module can use it
- Consistent patterns across codebase
- Reduced development time

## Performance Characteristics

### Zero-Cost Abstraction

- Virtual calls only on device creation (rare)
- Inlined audio processing path
- No runtime overhead in audio thread

### Memory Efficiency

- Shared coordinator (singleton)
- Atomic pointers (no locks)
- Pre-allocated buffers

### Latency

- Direct callback path
- No additional buffering
- Same latency as hand-rolled code

## Future Enhancements

### Potential Additions

1. Device hotplug support
2. Multi-device aggregation
3. Device-specific optimizations
4. Enhanced error reporting
5. Metrics and monitoring

### Extensibility Points

- Custom device types (already supported)
- Custom processing pipelines (already supported)
- New server types (follow existing pattern)
- Plugin-specific features (extend ModuleDeviceManager)

## Related Documentation

- `README.md`: Component overview and usage
- `PLUGINHOST3_USAGE.md`: Detailed usage guide for new modules
- Individual header files: Inline documentation

## Conclusion

The ModuleInfrastructure provides a production-ready, reusable solution for integrating JUCE-based audio processing modules with OBS, AudioServer, and MidiServer. It reduces boilerplate, improves maintainability, and enables rapid development of new modules while maintaining high performance and thread safety.
