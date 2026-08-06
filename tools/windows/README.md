# Windows allocation capture shim

`uad2_alloc_capture_proxy.c` is the user-mode forwarding shim used to observe
the native plug-in allocation record immediately before the official
`CreateUAD2PlugIn2` call. The original `UAD2DriverClient.dll` must be renamed
to `UAD2DriverClient-real.dll` in the same disposable test directory. The shim
loads it, captures the bounded record chain to `F:\sdk-recovery`, and forwards
the original arguments unchanged.

Build the 64-bit DLL with MinGW-w64:

```sh
x86_64-w64-mingw32-gcc -shared -O2 -Wall -Wextra -Werror \
  tools/windows/uad2_alloc_capture_proxy.c \
  tools/windows/uad2_alloc_capture_proxy.def \
  -o UAD2DriverClient.dll
```

The build must export the same 12 names and ordinals declared in the `.def`
file. Run it only in a disposable VM with a legally installed runtime. Restore
the original DLL after capture. Do not distribute the original DLL, captured
Bill bytes, or plug-in binaries.

Decode the output on the analysis host:

```sh
python3 tools/inspect_plugin_alloc_capture.py /path/to/allocinfo.bin
```

The decoder zeroes process-local pointers in its stable record hash and emits
only resource-relative offsets plus allocation metadata.
