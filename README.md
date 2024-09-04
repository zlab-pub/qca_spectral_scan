# Spectral Plot

A wireless spectrum analyzer app for Android phones.

## Requirements

This app requires an Android phone with the following:

- Android 7 or above (tested on Android 11 and 14)
- Root access (e.g. using [Magisk](https://github.com/topjohnwu/Magisk/))
- Qualcomm qcacld-3.0 driver

## Installation

Download the APK file built by GitHub Actions from the [Releases](https://github.com/zlab-pub/spectral-plot/releases) page and install it using `adb install`.

Alternatively, build the app on a Linux system with JDK 17 (or above) and Android SDK 34 by running `./gradlew assembleDebug`, which produces an APK file at `app/build/outputs/apk/debug/app-debug.apk`.

## Usage

Many Qualcomm chips' spectral scan feature can only cover a 40 MHz range centered at the frequency of the current Wi-Fi channel. Therefore, it's better to run this app with hotspot enabled and "Turn off hotspot automatically" disabled. The app will then periodically switch the channel of the hotspot to cover different frequency ranges.

The app will show a spectrogram on the screen. A short click on the screen will pause or resume the scanning, while a long click will show a configuration dialog.
