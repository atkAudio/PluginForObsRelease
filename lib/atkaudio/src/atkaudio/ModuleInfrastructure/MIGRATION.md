# Migration Guide: Updating Include Paths

After reorganizing the code into `ModuleInfrastructure`, several files need updated include paths.

## Files That Need Updates

### 1. PluginHost2 Files

Files in `lib/atkaudio/src/atkaudio/PluginHost2/` that include AudioServer or MidiServer:

**Before:**

```cpp
#include "../AudioServer/AudioServer.h"
#include "../MidiServer/MidiServer.h"
```

**After:**

```cpp
#include <atkaudio/ModuleInfrastructure/AudioServer/AudioServer.h>
#include <atkaudio/ModuleInfrastructure/MidiServer/MidiServer.h>
```

#### Files to Update

- `PluginHost2/UI/MainHostWindow.h`
- `PluginHost2/UI/MainHostWindow.cpp`
- `PluginHost2/Core/PluginGraph.h`
- `PluginHost2/Core/PluginGraph.cpp`
- `PluginHost2/Core/Ph2AudioIODevice.h` (also needs to migrate to Bridge)
- Any other files that include AudioServer or MidiServer

### 2. Other Module Files

Any files outside PluginHost2 that use AudioServer or MidiServer:

**Search for:**

```bash
# In PowerShell
Get-ChildItem -Path "lib\atkaudio\src" -Recurse -File | 
  Select-String -Pattern "#include.*AudioServer\.h|#include.*MidiServer\.h"
```

Update all matches to use new paths.

### 3. CMakeLists.txt Updates

The `lib/atkaudio/CMakeLists.txt` may need updates if it explicitly references AudioServer or MidiServer directories:

**Before:**

```cmake
# AudioServer sources
set(AUDIOSERVER_SOURCES
    src/atkaudio/AudioServer/AudioServer.cpp
    src/atkaudio/AudioServer/AudioServerSettingsComponent.cpp
)
```

**After:**

```cmake
# ModuleInfrastructure sources
set(MODULE_INFRASTRUCTURE_SOURCES
    src/atkaudio/ModuleInfrastructure/AudioServer/AudioServer.cpp
    src/atkaudio/ModuleInfrastructure/AudioServer/AudioServerSettingsComponent.cpp
    src/atkaudio/ModuleInfrastructure/MidiServer/MidiServer.cpp
    src/atkaudio/ModuleInfrastructure/MidiServer/MidiServerSettingsComponent.cpp
)
```

## Recommended Migration Steps

### Step 1: Update Include Paths (Non-Breaking)

1. Update all `#include` statements to use new paths
2. This maintains backward compatibility
3. Old code continues to work

### Step 2: Optionally Migrate PluginHost2 to Use Bridge (Breaking)

If you want PluginHost2 to use the new infrastructure:

1. Replace `Ph2AudioIODevice.h` includes with `ModuleBridge.h`
2. Update `PluginHost2::Impl` to use `ModuleDeviceManager`
3. Simplify the implementation (~150 lines → ~10 lines)

**Before (PluginHost2::Impl):**

```cpp
struct atk::PluginHost2::Impl
    : public juce::Timer
    , public juce::ChangeListener
{
    std::unique_ptr<MainHostWindow> mainHostWindow;
    std::unique_ptr<Ph2AudioIODeviceType> ph2DeviceType;
    std::atomic<Ph2AudioIODevice*> obsDevice{nullptr};
    
    // Lots of manual setup and tracking...
};
```

**After (using ModuleDeviceManager):**

```cpp
struct atk::PluginHost2::Impl
{
    std::unique_ptr<MainHostWindow> mainHostWindow;
    atk::ModuleDeviceManager moduleDeviceManager;
    
    Impl()
        : mainHostWindow(new MainHostWindow())
        , moduleDeviceManager(
            std::make_unique<atk::ModuleAudioIODeviceType>("PluginHost2 Audio"),
            mainHostWindow->getDeviceManager()
        )
    {
        moduleDeviceManager.initialize();
        moduleDeviceManager.openOBSDevice();
    }
};
```

### Step 3: Test

1. Build the project
2. Test OBS Audio functionality
3. Test AudioServer devices
4. Test MIDI I/O
5. Verify state persistence

## Automated Search and Replace

### PowerShell Script

```powershell
# Update AudioServer includes
Get-ChildItem -Path "lib\atkaudio\src" -Recurse -Include "*.h","*.cpp" |
  ForEach-Object {
    $content = Get-Content $_.FullName -Raw
    $updated = $content -replace '#include "../AudioServer/', '#include <atkaudio/ModuleInfrastructure/AudioServer/'
    $updated = $updated -replace '#include "../../AudioServer/', '#include <atkaudio/ModuleInfrastructure/AudioServer/'
    $updated = $updated -replace '#include "../MidiServer/', '#include <atkaudio/ModuleInfrastructure/MidiServer/'
    $updated = $updated -replace '#include "../../MidiServer/', '#include <atkaudio/ModuleInfrastructure/MidiServer/'
    Set-Content $_.FullName $updated
  }
```

### Manual Verification

After running the script, verify:

1. No broken includes remain
2. All files compile
3. No functionality is broken

## Testing Checklist

- [ ] Project builds without errors
- [ ] PluginHost2 window opens
- [ ] OBS Audio device is available
- [ ] AudioServer devices are listed
- [ ] Audio processing works
- [ ] MIDI input works
- [ ] MIDI output works
- [ ] State save/restore works
- [ ] Device switching works
- [ ] No crashes on shutdown

## Rollback Plan

If issues arise:

1. Revert file moves:

   ```powershell
   Move-Item "ModuleInfrastructure\AudioServer" "AudioServer"
   Move-Item "ModuleInfrastructure\MidiServer" "MidiServer"
   ```

2. Revert include path changes (use git):

   ```bash
   git checkout -- lib/atkaudio/src/
   ```

3. Remove Bridge directory:

   ```powershell
   Remove-Item "ModuleInfrastructure\Bridge" -Recurse
   ```

## Benefits After Migration

- ✅ Cleaner directory structure
- ✅ Related components grouped together
- ✅ Reusable infrastructure for PluginHost3
- ✅ Reduced code duplication
- ✅ Better maintainability
- ✅ Consistent patterns across modules

## Questions?

See:

- `ModuleInfrastructure/README.md` - Component overview
- `ModuleInfrastructure/ARCHITECTURE.md` - Design details
- `ModuleInfrastructure/PLUGINHOST3_USAGE.md` - Usage guide
