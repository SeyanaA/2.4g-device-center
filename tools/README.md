# Bundled build toolchain

The `*-buildkit-win64.zip` release asset contains the official, unmodified
[w64devkit](https://github.com/skeeto/w64devkit) v2.10.1 x64 self-extracting archive:

```text
w64devkit-x64-2.10.1.7z.exe
SHA-256: ADD1EDF984CE4D473CFEB7C52E0AB38DB73A855BBA6C7F564D5E16648EB2896A
```

`build.ps1` (or `build.cmd`) finds `tools\w64devkit*.7z.exe`, silently extracts it to
`tools\w64devkit`, and uses its `g++.exe` and `windres.exe`. The first build takes longer because
of the one-time extraction (~740 MB on disk).

w64devkit bundles GCC, binutils, MinGW-w64 and other components under their respective licenses;
the archive is redistributed unmodified with its license files. If you already have a
MinGW-w64/w64devkit toolchain, set `W64DEVKIT` or pass `-Compiler <g++.exe>` and the bundled
archive is not needed.

The raw installer is intentionally not committed to the source repository. Download the
`*-buildkit-win64.zip` asset from the project's GitHub Releases.
