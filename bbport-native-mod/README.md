# Hunter's Atlas native module for bbport

> Showcase Mod

https://github.com/user-attachments/assets/61d03361-83ed-4ee8-9310-9b676145be3a

https://github.com/user-attachments/assets/5ac895c8-6697-4cd0-800c-497cfd2cc421

This target adapts Hunter's Atlas 3.1 to bbport's versioned native-mod ABI. It
builds only the Atlas module; the bbport runtime and game files are never placed
inside the mod package.

Build from an MSYS2 CLANG64 shell after installing the packages listed by
bbport's `packaging/windows/README.md`:

```sh
cmake -S source/bbport-native-mod -B out/native-mod -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
ninja -C out/native-mod hunters_atlas
```

The runtime accepts this module only through an enabled `bbport-native-mod.json`
manifest. Atlas still validates the supported executable before position, flag,
or travel services become available. Press `F1` while the map is open for its
shortcut settings.
