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
