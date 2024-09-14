# SoftSA

An RF spectrum analyzer app for Android phones using only built-in hardware. The project is based on Qualcomm SoCs. Given Qualcomm's acquisition of Atheros, we think that their Wi-Fi modules share similarities with the Atheros [ath series](https://wireless.wiki.kernel.org/en/users/drivers/ath10k/spectral). This project has involved reverse engineering the QCA driver and has undergone a preliminary implementation to enable spectral scanning with Qualcomm-based smartphones.

## Requirements

This app requires an Android phone with the following:

- Android 7 or above (tested on Google Pixel 5 with Android 11 and Android 14)
- Root access (e.g. using [Magisk](https://github.com/topjohnwu/Magisk/))
- Qualcomm qcacld-3.0 driver

## Installation

Download the APK file built by GitHub Actions from the [Releases](https://github.com/zlab-pub/SoftSA/releases) page and install it using `adb install`.

Alternatively, build the app on a Linux system with JDK 17 (or above) and Android SDK 34 by running `./gradlew assembleDebug`, which produces an APK file at `app/build/outputs/apk/debug/app-debug.apk`. The following third-party dependencies will be downloaded during the build process:

- [libsu](https://github.com/topjohnwu/libsu) will be downloaded by Gradle.
- [libnl](https://github.com/thom311/libnl) will be downloaded by CMake, built from source, and included in the app as shared libraries.
- [hostapd](https://w1.fi/hostapd/) will be downloaded by CMake, though this app only uses its [qca-vendor.h](https://w1.fi/cgit/hostap/plain/src/common/qca-vendor.h) header file.

## Usage

Many Qualcomm chips' spectral scan feature can only cover a 40 MHz range centered at the frequency of the current Wi-Fi channel. Therefore, it's better to run this app with hotspot enabled and "Turn off hotspot automatically" disabled. The app will then periodically switch the channel of the hotspot to cover different frequency ranges. If enabling hotspot before launching the app doesn't work, try enabling hotspot after launching the app instead.

This app shows a spectrogram on the screen, where brighter colors indicate higher FFT magnitudes. This app also employs a simple algorithm to detect Bluetooth transmission and estimate its strength. The following is a screenshot of the app in the presence of Bluetooth transmission (click on it to view a screen recording):

<p align="center">
  <a href="https://github.com/user-attachments/assets/d81cccf7-be4d-4b00-ac9c-f4950b1802f5">
    <img
      src="assets/images/screenshot.png"
      alt="A screenshot of the app in the presence of Bluetooth transmission."
      width="50%"
    />
  </a>
</p>

A short click on the screen pauses or resumes the scanning, while a long click shows a configuration dialog:

<table>
  <tr>
    <td>
      <img
        src="assets/images/config0.png"
        alt="The configuration dialog shown on long click."
      />
    </td>
    <td>
      <img
        src="assets/images/config1.png"
        alt="The configuration dialog for AP frequencies."
      />
    </td>
  </tr>
</table>


## Contact

If you have any questions about this project, contact <zhoujq2024@shanghaitech.edu.cn> or <yangzhc@shanghaitech.edu.cn>.
