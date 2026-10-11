# installbuilder-unpacker

Unpack BitRock InstallBuilder installers without running them. Works with macOS `.app`, Linux `.run`, and Windows `.exe` installers, including password-protected and split payloads.

[Download a release](https://github.com/ygzo/installbuilder-unpacker/releases) for macOS, Linux, or Windows, on arm64 or x86_64.

## Usage

Open a terminal in the folder containing the executable (`bin` in a downloaded release, or `build` when built from source):

```sh
./ib-unpack /path/to/installer.app
```

The output goes next to the installer, in `installer.app.unpacked`. Payload files are under `components/`, alongside any installer metadata and images.

To choose an output folder, replace existing output, or supply a password:

```sh
./ib-unpack /path/to/installer.run extracted
./ib-unpack -y /path/to/installer.exe extracted
./ib-unpack -p 'PASSWORD' /path/to/installer.app
```

A progress bar appears during unpacking. Use `--no-progress` to hide it. Failed extraction leaves existing output intact.

On Windows, use `.\ib-unpack.exe` instead of `./ib-unpack`.

## Build

You'll need CMake 3.20+, a C++17 compiler, zlib, and liblzma. On macOS, install Xcode Command Line Tools and CMake. On Debian or Ubuntu:

```sh
sudo apt-get install build-essential cmake zlib1g-dev liblzma-dev
```

From the repository folder:

```sh
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
./ib-unpack /path/to/installer.app
```

For Windows, install Visual Studio's C++ tools and [vcpkg](https://github.com/microsoft/vcpkg). In PowerShell, from the repository folder:

```powershell
mkdir build
cd build
cmake .. -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build . --config Release
cd Release
.\ib-unpack.exe C:\path\to\installer.exe
```

For Windows arm64, use `-A ARM64` and `-DVCPKG_TARGET_TRIPLET=arm64-windows-static` in a separate build folder.

## Compatibility

Supports ZIP, LZMA, and LZHAM compression. Preserves timestamps, permissions, and symlinks where the operating system allows. Windows symlinks require Developer Mode or administrator rights. Nonempty Ultra payloads still need verification.

## License

[MIT](LICENSE). Bundled libraries keep their own [license notices](third_party/README.md).
