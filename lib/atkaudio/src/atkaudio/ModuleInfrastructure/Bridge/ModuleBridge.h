#pragma once

/**
 * Module Infrastructure - Bridge Components
 *
 * This module provides reusable components for bridging JUCE AudioDeviceManager
 * with OBS audio and AudioServer devices, plus MIDI integration via MidiServer.
 *
 * Main components:
 * - ModuleDeviceCoordinator: Singleton that ensures only one device is active
 * - ModuleOBSAudioDevice: Base class for OBS audio devices
 * - ModuleAudioServerDevice: Bridge for AudioServer devices (ASIO, CoreAudio, etc.)
 * - ModuleAudioIODeviceType: Device type that manages both OBS and AudioServer devices
 * - ModuleDeviceManager: High-level manager that encapsulates the entire pattern
 *
 * Usage:
 *
 * Simple usage with ModuleDeviceManager:
 * ```cpp
 * class MyModule {
 *     juce::AudioDeviceManager deviceManager;
 *     atk::ModuleDeviceManager moduleDeviceManager;
 *
 *     MyModule()
 *         : moduleDeviceManager(
 *             std::make_unique<atk::ModuleAudioIODeviceType>("MyModule Audio"),
 *             deviceManager
 *         )
 *     {
 *         moduleDeviceManager.initialize();
 *         moduleDeviceManager.openOBSDevice();
 *     }
 *
 *     void process(float** buffer, int channels, int samples, double sampleRate) {
 *         moduleDeviceManager.processExternalAudio(buffer, channels, samples, sampleRate);
 *     }
 *
 *     atk::MidiClient& getMidiClient() {
 *         return moduleDeviceManager.getMidiClient();
 *     }
 * };
 * ```
 *
 * Advanced usage with custom device types:
 * ```cpp
 * class MyCustomDeviceType : public atk::ModuleAudioIODeviceType {
 * protected:
 *     juce::AudioIODevice* createOBSDevice(const juce::String& deviceName) override {
 *         return new MyCustomOBSDevice(deviceName);
 *     }
 * };
 *
 * // Use MyCustomDeviceType instead of ModuleAudioIODeviceType
 * ```
 */

#include "ModuleAudioDevice.h"
#include "ModuleAudioServerDevice.h"
#include "ModuleAudioIODeviceType.h"
#include "ModuleDeviceManager.h"
