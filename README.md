# atkAudio Plugin for OBS

## PluginHost

- VST3 plugin host for OBS
- MIDI, e.g. for using MIDI keyboard and a sampler plugin as soundboard
- Sidechain support
- Optional multithreading for improved performance with multi-core CPUs
- Direct interfacing with audio and MIDI hardware devices
- AU plugins on Apple macOS
- LADSPA and LV2 plugins on Linux

## PluginHost2

- Includes all features of regular PluginHost plus:
- Use multiple plugins to create complex audio processing chains and graphs from OBS sources and audio devices
- Always internally multithreading (no extra latency penalty)
- Saving and loading of graphs as files
- Route audio and MIDI between sources, plugins and hardware (ASIO/CoreAudio included)
- Sample rate converting and drift compensating internal buffering for seamless audio between OBS sources and audio devices
- etc

PluginHost2 can interface directly with audio and MIDI hardware, OBS audio sources, and output audio as new OBS sources, allowing for complex audio processing setups. E.g. use ASIO interface as audio device, take additional audio from OBS sources, route monitoring to ASIO outputs and/or different audio drivers/hardware, use plugins and create final mix, and output the processed audio as a new OBS source for recording and streaming. Or just create a simple soundboard with a sampler plugin and a MIDI keyboard.

Develop your own audio processing plugins and integrate them into `PluginHost2` using the [JUCE framework](https://juce.com/) AudioProcessor class. See `InternalPlugins.cpp` how `GainPlugin` is loaded. See `GainPlugin.h` for implementation. Optionally include OBS headers to use the [OBS API](https://docs.obsproject.com/) for more advanced integration with [OBS Studio](https://obsproject.com/)

## DeviceIo(2)

- Send and receive audio directly into and from audio devices
- "Anything from/to anywhere" device routing
- ASIO, CoreAudio and Windows Audio devices

## Audio Source Mixer (OBS Source)

- Mix audio from OBS sources into a new OBS audio source
- Can be used as 'dummy' source to host filters, e.g. PluginHost2

## Usage examples

- Use Delay filter to manually delay/sync individual audio sources
- Source Mixer can create submixes (or one main mix) from multiple OBS audio sources
  - Mute original sources to prevent double/parallel audio
- Use DeviceIo2 to route audio directly between OBS and audio devices (e.g. ASIO)
- Put CPU intensive plugins into PluginHost and enable MT for better performance (multi-core, one buffer extra latency)
- Use a sampler plugin with a MIDI keyboard in PluginHost as a soundboard
- Do all of the above and more with PluginHost2
  - MIDI control of OBS audio source volume & mute

## Build instructions

Project is (now loosely) based on [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate) and depends on [JUCE Framework](https://github.com/juce-framework/JUCE). Install JUCE Framework [Minimum System Requirements](https://github.com/juce-framework/JUCE#minimum-system-requirements) and OBS Plugin Template [Supported Build Environment](https://github.com/obsproject/obs-plugintemplate#supported-build-environments) and follow OBS Plugin Template [Quick Start Guide](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide).

In short, after installing all dependencies (Ubuntu example):

```console
git clone https://github.com/atkaudio/pluginforobsrelease
cd pluginforobsrelease
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 
```

Find `atkaudio-pluginforobs.so` and copy it to OBS plugins directory.
See `CMakePresets.json` for Windows, macOS and other build presets.

## Donation

[![PayPal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/donate/?hosted_button_id=ERBKC76F55HZW)

If you find this project useful, please consider making [a donation](https://www.paypal.com/donate/?hosted_button_id=ERBKC76F55HZW) to support its future development and maintenance.

## Installation

- Download and install [latest release](https://github.com/atkAudio/PluginForObsRelease/releases/latest) using the appropriate installer for your OS.
- Manual/portable installations e.g. on major Linux distros: extract portable `.zip` file and copy the directory `atkaudio-pluginforobs` into `~/.config/obs-studio/plugins/`.
