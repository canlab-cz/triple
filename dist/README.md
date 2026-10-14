# Distribution packages

Build scripts for the release packages. They write into `dist/out/`
(not in git); `dist/VERSION` holds the version.

| Package | For | Contents |
|---|---|---|
| `usb2cansocketcan-dkms_<v>-1_all.deb` | Ubuntu 22.04+, Debian 12+ | Driver source built by DKMS for every kernel, udev rule and systemd unit (ldattach on plug-in), `triple-attach` / `triple-detach` |
| `usb2can-triple-tools_<v>-1_amd64.deb` | Ubuntu 22.04+, Debian 12+ | `canre_cli`, `triple-dump`, `canopen-sim`, `re-demo` |
| `savvycan-triple_<v>-1_amd64.deb` | Ubuntu 22.04+, Debian 12+ | SavvyCAN with the Triple connection and tools (`savvycan-triple`), uses the system's Qt 5 |
| `usb2can-triple-<v>-linux-x86_64.tar.gz` | any x86-64 Linux with glibc 2.35+ (Ubuntu 22.04, Debian 12, Fedora 36 or later) | Driver source with `install.sh` (DKMS or plain make), udev / systemd files, SavvyCAN with Qt 5 bundled, tools, documentation |
| `usb2can-triple-<v>-windows-x64-setup.exe` | Windows 10 / 11, 64 bit | Installer: SavvyCAN with Qt 6, tools, documentation, Start menu entries, optional PATH entry for `canre_cli` |
| `usb2can-triple-<v>-windows-x64.zip` | Windows 10 / 11, 64 bit | The same, portable: unzip and run `SavvyCAN\SavvyCAN.exe` |

## Installing

### Debian, Ubuntu

```sh
sudo apt install ./usb2cansocketcan-dkms_2.1.0-1_all.deb ./usb2can-triple-tools_2.1.0-1_amd64.deb \
                 ./savvycan-triple_2.1.0-1_amd64.deb
```

apt pulls in DKMS and the Qt 5 libraries. The kernel headers of the
running kernel must be installed (`linux-headers-generic` on Ubuntu,
`linux-headers-amd64` on Debian) for DKMS to build the module. After
that, plugging in an adapter creates `can0` .. `can2` (bitrates are set
with `ip link`, see [linux-driver.md](../doc/linux-driver.md)), or run
`sudo triple-attach ttyACM0` for the default bitrates. Start the analyzer
with `savvycan-triple` or from the applications menu.

### Other Linux distributions

```sh
tar xzf usb2can-triple-2.1.0-linux-x86_64.tar.gz
cd usb2can-triple-2.1.0-linux-x86_64
sudo ./install.sh              # driver, udev rule, SavvyCAN and tools in /opt/usb2can-triple
sudo ./install.sh --no-driver  # without the driver
sudo ./install.sh --uninstall
```

The driver needs `make`, `gcc` and the kernel headers (`kernel-devel` on
Fedora); with `dkms` installed it is rebuilt after kernel updates. SavvyCAN
runs from the unpacked folder without installing as well:
`savvycan/savvycan`.

### Windows

Run the setup, or unzip the portable package anywhere. The adapter uses
Windows' built-in USB serial driver, nothing else to install. The Python
tools need Python 3 and `pip install pyserial`.

## Building

| Package | Build on | Command |
|---|---|---|
| Debian packages | Ubuntu 22.04 (native or WSL2) | `bash dist/debian/build.sh` |
| Linux tarball | Ubuntu 22.04 (native or WSL2) | `bash dist/linux/build.sh` |
| Windows zip and setup | Windows with Qt 6.8 (MinGW), CMake, Ninja, git, Python; Inno Setup 6 or 7 for the setup | `powershell -ExecutionPolicy Bypass -File dist\windows\build.ps1` |

Linux build requirements:

```sh
sudo apt install build-essential git python3 dpkg-dev qtbase5-dev qt5-qmake libqt5serialbus5-dev \
     libqt5serialport5-dev qtdeclarative5-dev qttools5-dev libqt5websockets5-dev libqt5opengl5-dev
```

SavvyCAN is cloned from GitHub at the upstream commits the changes are
tested on (`master` at `e439670` for Qt 5, `QT6WIP` at `3288f1f` for Qt 6)
and patched with `gui/savvycan/apply.py`. To use a local clone instead:
`SAVVYCAN_GIT=~/SavvyCAN bash dist/debian/build.sh`, or
`build.ps1 -SavvyCanGit C:\path\to\SavvyCAN`. The Linux scripts build in
`~/.cache/usb2can-triple-dist` (override with `WORK=`), the Windows script
in `dist\work\`. `build.ps1` finds Inno Setup in its usual folders or
takes `-Iscc <path to ISCC.exe>`.

## Testing

Both Linux test scripts need Docker and install into clean containers:

```sh
bash dist/debian/test-debs.sh ubuntu:24.04    # apt install, DKMS build, tools, SavvyCAN offscreen, removal
bash dist/linux/test-tarball.sh fedora:41     # bundled libraries, SavvyCAN offscreen, install.sh, driver build
```

Tested for 2.1.0: the .deb packages on Ubuntu 22.04, Ubuntu 24.04 and
Debian 12 (module built by DKMS for 5.15, 6.8 and 6.1); the tarball on
Ubuntu 24.04, Debian 12 and Fedora 41 (driver built for 6.8, 6.1, 6.17);
the Windows zip with a clean PATH and the setup (per-user install, start,
silent uninstall) on Windows 11, with SavvyCAN connected to an adapter.

## Releasing a new version

1. Set the version in `dist/VERSION`, `driver/dkms.conf`
   (`PACKAGE_VERSION`) and `driver/triple.c` (`MODULE_VERSION`).
2. Run the three builds and the tests.
3. Attach the files from `dist/out/` to the GitHub release.
