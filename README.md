# atkAudio Plugin for OBS

## Plugin Host
- VST3 plugin host for OBS
- Up to 8 channels
- Sidechain support
- AU plugins on Apple macOS
- LADSPA and LV2 plugins on Linux

## Plugin Host2
- Build filter graphs (plugin chains) with multiple plugin instances
- Saving and loading of filter graphs as files
- MIDI support (e.g. for using MIDI keyboard and a sampler plugin as soundboard)
- etc

Plugin Host2 can interface directly with audio and MIDI hardware, OBS audio sources, and output audio as new OBS sources, allowing for complex audio processing setups. E.g. use ASIO interface as audio device, take additional audio from OBS sources, route monitoring to ASIO outputs and/or different audio drivers/hardware, use plugins and create final mix, and output the processed audio as a new OBS source for recording and streaming. Or just create a simple soundboard with a sampler plugin and a MIDI keyboard.

## Device I/O
- Send and receive audio directly into and from audio devices
- "Anything from/to anywhere" device routing
- ASIO, CoreAudio and Windows Audio devices
- Resampling and drift correction

## Audio Source Mixer (OBS Source)
- Mix audio from up to 8 OBS sources into a new OBS audio source
- E.g. allows creating new submixes
- Can be used as 'dummy' source to host Device IO 

## Installation
- Download and install [latest release](https://github.com/atkAudio/PluginForObsRelease/releases/latest)
- Manual/portable installations: extract `.dll|.dylib|.so` from installer and copy it to OBS plugins directory. For example on Linux, extract `atkaudio-pluginforobs.so` from `.deb` file and copy it to `~/.config/obs-studio/plugins/atkaudio-pluginforobs/bin/64bit/`

## Build instructions
Project is based on [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate) and depends on [JUCE Framework](https://github.com/juce-framework/JUCE). Install JUCE Framework [Minimum System Requirements](https://github.com/juce-framework/JUCE#minimum-system-requirements) and OBS Plugin Template [Supported Build Environment](https://github.com/obsproject/obs-plugintemplate#supported-build-environments) and follow OBS Plugin Template [Quick Start Guide](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide).

In short, after installing all dependencies (Ubuntu example):
```
git clone https://github.com/atkaudio/pluginforobsrelease
cd pluginforobsrelease
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 
```
Find `atkaudio-pluginforobs.so` and copy it to OBS plugins directory. 
See `CMakePresets.json` for Windows, macOS and other build presets.
