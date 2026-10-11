# Vendored dependencies

Only the sources needed by the native extractor are included. These sources are built locally. Windows builds use vcpkg to download zlib and liblzma.

| Component | Upstream revision | Included license | Use |
| --- | --- | --- | --- |
| [Metakit](https://github.com/jnorthrup/metakit) | `ab38c5340d8bb6f88fd0339b65cf33a66c4b9400` | `metakit/license.terms` (MIT-style) | Embedded filesystem database reading |
| [LZHAM](https://github.com/richgel999/lzham_codec) | `d379b1f9121e2197881c61cfc4713c78848bdfe7` | `lzham/LICENSE` (public domain) | Payload decoding |
| [LibTomCrypt](https://github.com/libtom/libtomcrypt) | v1.18.2, `7e7eb695d581782f04b24dc444cbfde86af59853` | `libtomcrypt/LICENSE` (public domain or WTFPL) | Twofish primitive only |
| [PicoSHA2](https://github.com/okdshin/PicoSHA2) | `161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29` | `picosha2/LICENSE` (MIT) | SHA-256 |

Local integration changes:

- Metakit uses a generated configuration header and `q4_LONG64=1` where C++ `long` is 64 bits. It is compiled as C++11.
- LZHAM's Apple POD trait uses the compiler's `__is_pod` intrinsic. The macOS debugger trap uses `__builtin_trap()` instead of x86 inline assembly. `LZHAM_ANSI_CPLUSPLUS` selects portable loads on ARM; only decoder sources are compiled, with strict aliasing disabled on GCC/Clang. The portable configuration includes `<cstdint>` explicitly for modern compilers, and its informational threading warning is removed. Windows portable builds use the CRT allocation-size function; MinGW builds use CRT secure-print functions instead of redefining them.
- LibTomCrypt compiles only `twofish.c`, with `LTC_NOTHING`, `LTC_TWOFISH`, `LTC_NO_TEST`, `LTC_NO_ASM`, and `ARGTYPE=4`. MSVC ARM macros are recognized in byte-order and word-size detection; portable loads remain enabled. The application handles CBC framing and password derivation.
- PicoSHA2 is used without source modifications.

zlib and liblzma are external build dependencies. macOS builds link the system zlib, liblzma, C++ runtime, and libSystem.

Preserve the bundled license notices when redistributing vendored sources or the executable.
