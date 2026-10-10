# Moonlight Nonary Extended

A [Moonlight](https://moonlight-stream.org) game streaming client for Windows and Linux, with full DualSense support.

It is based on [Nonary's moonlight-qt](https://github.com/Nonary/moonlight-qt) (`release/6.1.0-vrr18`: PyroWave, VRR pacing) and adds the DualSense and controller features from [Taveszfito/moonlight-extended](https://github.com/Taveszfito/moonlight-extended).

## Features

- DualSense speaker, headset jack and HD haptics from the host, over USB and Bluetooth
- DualSense Bluetooth microphone sent to the host
- Choose the controller type the host emulates (Automatic, Xbox 360, DualShock 4, DualSense). Hold Start + Select for 2 s to switch one pad to Xbox 360 and back.
- Map controller buttons to keyboard keys and mouse, use the gyro as a stick, and set shortcut chords
- Extended Quick Menu, clipboard sync and stream notifications
- Bitrate (10 s average and peak) in the stream statistics
- On Windows, it installs next to normal Moonlight and has its own settings. It does not change or remove your existing Moonlight.

Controller audio, microphone and emulation choice need a host with Apollo Extended controller support. With other hosts, normal streaming works.

The DualSense features use a patched SDL3 (`sdl-patches/`). The build scripts download SDL 3.4.12 and apply the patches for you.

## Linux

Build on the computer where you will use the client. Builds from a different distribution do not run, because FFmpeg and Qt library versions differ.

### 1. Install the build dependencies

Arch / CachyOS:

```bash
sudo pacman -S --needed base-devel git cmake ninja \
  qt6-base qt6-declarative qt6-svg qt6-wayland \
  ffmpeg libva libvdpau libplacebo vulkan-headers vulkan-icd-loader \
  opus sdl2-compat sdl2_ttf openssl libxkbcommon wayland wayland-protocols libdrm mesa \
  libpulse pipewire libusb systemd-libs dbus \
  libx11 libxext libxtst libxrandr libxcursor libxi libxfixes libxss
```

Ubuntu 24.04 / Debian:

```bash
sudo apt install build-essential git cmake ninja-build \
  qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-wayland \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qtqml-workerscript \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libplacebo-dev libvulkan-dev \
  libva-dev libvdpau-dev libdrm-dev libgbm-dev libegl-dev libopus-dev libssl-dev \
  libsdl2-dev libsdl2-ttf-dev libpulse-dev libasound2-dev libpipewire-0.3-dev \
  libudev-dev libusb-1.0-0-dev libdbus-1-dev libwayland-dev libxkbcommon-dev \
  libx11-dev libxext-dev libxtst-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev
```

If the build stops because a library is missing, install the matching package and run the build again.

### 2. Build

```bash
git clone --recursive https://github.com/verusferro/moonlight-nonary-extended.git
cd moonlight-nonary-extended
scripts/build-linux.sh
```

The first build needs internet access to download SDL and sdl2-compat. To rebuild, run the script again.

### 3. Give your user access to the DualSense

Do this one time on each computer. It applies to all DualSense and DualSense Edge controllers, over USB and Bluetooth.

Connect the controller. Then check if you already have access:

```bash
bash -c 'for d in /sys/class/hidraw/*; do grep -qiE "054C:0000(0CE6|0DF2)" "$d/device/uevent" 2>/dev/null || continue; n=/dev/${d##*/}; if [ -r "$n" ] && [ -w "$n" ]; then echo "$n: access OK"; else echo "$n: no access"; fi; done'
```

- `access OK`: go to step 4.
- `no access`: add the udev rule below.
- No output: the controller is not connected.

```bash
printf '%s\n' 'KERNEL=="hidraw*", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0ce6", TAG+="uaccess"' \
  'KERNEL=="hidraw*", ATTRS{idVendor}=="054c", ATTRS{idProduct}=="0df2", TAG+="uaccess"' \
  'KERNEL=="hidraw*", KERNELS=="*054C:0CE6*", TAG+="uaccess"' \
  'KERNEL=="hidraw*", KERNELS=="*054C:0DF2*", TAG+="uaccess"' \
  | sudo tee /etc/udev/rules.d/70-dualsense.rules
sudo udevadm control --reload && sudo udevadm trigger
```

Disconnect the controller, connect it again, and do the check again.

Without this access, buttons, sticks and rumble still work, but adaptive triggers, Bluetooth audio, haptics and the microphone do not.

### 4. Run

```bash
build-linux/stage/moonlight
```

Always start the client with this launcher, not `moonlight.bin`. The launcher makes the client use the patched SDL3 instead of the system SDL.

## Windows

### 1. Install the tools

- [Visual Studio 2022](https://visualstudio.microsoft.com/) (or its Build Tools) with "Desktop development with C++"
- [Qt 6](https://www.qt.io/download-qt-installer) for MSVC 2022 64-bit (tested with 6.11.1). Add its `bin` folder (with `qmake.exe`) to `PATH`.
- [Git](https://git-scm.com/), [CMake](https://cmake.org/) and [7-Zip](https://www.7-zip.org/) on `PATH`

### 2. Build

In a Command Prompt:

```bat
git clone --recursive https://github.com/verusferro/moonlight-nonary-extended.git
cd moonlight-nonary-extended
powershell -ExecutionPolicy Bypass -File setup-deps.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-sdl-windows.ps1
scripts\build-arch.bat Release x64
scripts\generate-bundle.bat Release x64-only
```

`setup-deps.ps1` downloads the stock Moonlight libraries. `build-sdl-windows.ps1` then replaces their SDL3 with the patched SDL3. Run both again if you run `setup-deps.ps1` again.

The results are in `build\`:

| File | What it is |
|---|---|
| `installer-release\MoonlightSetup-<version>.exe` | Installer |
| `build-x64-release\Moonlight.msi` | MSI package |
| `installer-x64-release\MoonlightPortable-x64-<version>.zip` | Portable version |

### Move settings from normal Moonlight

This client has its own settings, so you must pair with your host again. To copy hosts, pairing and settings from normal Moonlight, export `HKCU\Software\Moonlight Game Streaming Project` with Registry Editor to a `.reg` file, then:

```bat
powershell -ExecutionPolicy Bypass -File scripts\import-settings.ps1 -Backup moonlight.reg -Installed
```

## License

GPL-3.0, like Moonlight. See [LICENSE](LICENSE). Third-party license texts are in `packaging/licenses/`.
