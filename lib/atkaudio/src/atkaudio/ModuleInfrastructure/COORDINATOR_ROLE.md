# Coordinator Role: Quick Reference

## What the Coordinator Does

**Ensures only ONE device is active per module instance at a time.**

### Within a Single Module Instance

```
Module A (PluginHost2):
  Coordinator A:
    ✅ Can activate: OBS Audio
    ❌ Cannot also activate: ASIO Device (while OBS Audio is active)
    
  Only ONE device can be active and registered with AudioServer at a time.
```

### Across Multiple Module Instances

```
Module A (PluginHost2):
  Coordinator A:
    ✅ OBS Audio active
    ✅ Registered callback with AudioServer
    
Module B (PluginHost3):
  Coordinator B:
    ✅ ASIO Device active  
    ✅ Registered callback with AudioServer
    
Both modules process audio CONCURRENTLY via their registered callbacks.
```

## Key Points

1. **Scope**: Per-module-instance only
   - Each module has its own coordinator
   - Coordinator prevents device conflicts **within that module**
   - Does NOT coordinate across different modules

2. **AudioServer Integration**:
   - Each module registers its **active device's callback** with AudioServer
   - AudioServer can handle **multiple concurrent callbacks** (one per module)
   - All registered modules process audio simultaneously from hardware

3. **Purpose**: Prevents UI confusion
   - User can't accidentally select multiple devices in one module
   - Example: Can't have both "OBS Audio" and "ASIO Device" selected in PluginHost2
   - But PluginHost2 can use "OBS Audio" while PluginHost3 uses "ASIO Device"

## Examples

### ❌ Prevented (Same Module)

```cpp
PluginHost2 module;
module.openDevice("OBS Audio");        // ✅ Active
module.openDevice("ASIO Device");      // ❌ Blocked by coordinator - only one active
```

### ✅ Allowed (Different Modules)

```cpp
PluginHost2 host2;
host2.openDevice("OBS Audio");         // ✅ Active, registered with AudioServer

PluginHost3 host3;
host3.openDevice("ASIO Device");       // ✅ Active, registered with AudioServer

// Both process audio concurrently via AudioServer callbacks
```

## Implementation

```cpp
class ModuleAudioIODeviceType {
    // Each device type has ONE coordinator
    std::shared_ptr<ModuleDeviceCoordinator> coordinator;
    
    // All devices created by this type share the same coordinator
    ModuleOBSAudioDevice* createOBSDevice() {
        return new ModuleOBSAudioDevice(..., coordinator, ...);
    }
    
    ModuleAudioServerDevice* createAudioServerDevice() {
        return new ModuleAudioServerDevice(..., coordinator, ...);
    }
};

// Different modules = different device types = different coordinators
PluginHost2: ModuleAudioIODeviceType → Coordinator A
PluginHost3: ModuleAudioIODeviceType → Coordinator B
```

## Summary

- **One coordinator per module instance**
- **One active device per module instance**
- **Multiple modules can coexist**
- **Each module registers with AudioServer**
- **All modules process concurrently**
