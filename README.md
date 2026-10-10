# Moonlight Nonary Extended

A [Moonlight](https://moonlight-stream.org) game streaming client for Windows and Linux, with full DualSense support.

It is based on [Nonary's moonlight-qt](https://github.com/Nonary/moonlight-qt) (`release/6.1.0-vrr18`: PyroWave, VRR pacing) and adds the DualSense and controller features from [Taveszfito/moonlight-extended](https://github.com/Taveszfito/moonlight-extended). It also carries fixes from upstream Moonlight 6.2.0: the CVE-2026-41210 security fix, full-range colour, frame-loss recovery fixes and the newer Windows libraries (FFmpeg 9.0.2).

## Features

- DualSense speaker, headset jack and HD haptics from the host, over USB and Bluetooth
- DualSense Bluetooth microphone sent to the host
- Choose the controller type the host emulates (Automatic, Xbox 360, DualShock 4, DualSense). Press Share/Back + L3 + R3 on a pad to switch it to Xbox 360 emulation and back. Change or turn off this button combo in Settings → Quick Menu shortcuts → Toggle Xbox 360 emulation.
- Map controller buttons to keyboard keys and mouse, use the gyro as a stick, and set shortcut chords
- Extended Quick Menu, clipboard sync and stream notifications
- Bitrate (10 s average and peak) in the stream statistics
- On Windows, it installs next to normal Moonlight and has its own settings. It does not change or remove your existing Moonlight.

Controller audio, microphone and emulation choice need a host with Apollo Extended controller support, for example [Vibepollo DualSense](https://github.com/verusferro/vibepollo-dualsense). With other hosts, normal streaming works.

The DualSense features use a patched SDL3 (`sdl-patches/`). The build scripts download SDL 3.4.18 and apply the patches for you.

## Download

Get the latest version from the [Releases page](https://github.com/verusferro/moonlight-nonary-extended/releases):

| File | For |
|---|---|
| `MoonlightSetup-<version>.exe` | Windows installer |
| `MoonlightPortable-x64-<version>.zip` | Windows, portable (no install) |
| `Moonlight_Nonary_Extended-<version>-x86_64.AppImage` | Linux. Run `chmod +x` on it, then start it. |

The AppImage contains the patched SDL3, Qt and FFmpeg 9. It uses your system's VA-API library when it can, so hardware decoding works with current GPU drivers. It needs glibc 2.39 or newer (Ubuntu 24.04+, Fedora 40+, Arch, CachyOS). GPU drivers, Vulkan and Wayland come from your system. On Linux, also do [DualSense access](#3-give-your-user-access-to-the-dualsense).

To build the AppImage yourself (Ubuntu 24.04): install the Ubuntu packages from [Build on Linux](#1-install-the-build-dependencies) plus `libx11-xcb-dev libxcb-dri3-dev libshaderc-dev`, put [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy/releases) and [linuxdeploy-plugin-qt](https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases) in `PATH`, then run `scripts/build-linux.sh` and `scripts/build-linux-appimage.sh`. The second script builds FFmpeg 9.0.2, libplacebo, dav1d and libva into `build-linux/appimage-deps` first.

## Build on Linux

Build on the computer where you will use the client. Builds from a different distribution do not run, because FFmpeg and Qt library versions differ.

### 1. Install the build dependencies

Arch / CachyOS: nothing to install first. `scripts/build-linux.sh` checks the packages and, if some are missing, prints the `sudo pacman -S --needed ...` command to run.

Ubuntu 24.04 / Debian:

```bash
sudo apt install build-essential git cmake ninja-build \
  qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-wayland \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qtqml-workerscript \
  qml6-module-qtquick-dialogs \
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

If you have Steam installed, you can skip this step: its `/usr/lib/udev/rules.d/60-steam-input.rules` already gives your user access.

Without this access, buttons, sticks and rumble still work, but adaptive triggers, Bluetooth audio, haptics and the microphone do not.

### 4. Run

```bash
build-linux/stage/moonlight
```

Always start the client with this launcher, not `moonlight.bin`. The launcher makes the client use the patched SDL3 instead of the system SDL.

To add it to your start menu (optional, only for your user):

```bash
scripts/install-linux-desktop.sh
```

### Move settings from normal Moonlight

This client has its own settings, so you must pair with your host again. To copy hosts, pairing and settings (same file format) from normal Moonlight, close both and run:

```bash
cd ~/.config && mkdir -p "Moonlight Nonary Extended" && cp "Moonlight Game Streaming Project/Moonlight.conf" "Moonlight Nonary Extended/Moonlight Nonary Extended.conf"
```

## Build on Windows

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
