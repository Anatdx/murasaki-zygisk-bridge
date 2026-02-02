# Murasaki Bridge (Zygisk)

This folder is the **Magisk module template**.

## Layout

```text
murasaki_bridge/
├── module.prop
└── zygisk/
    ├── arm64-v8a.so
    ├── armeabi-v7a.so
    ├── x86.so
    └── x86_64.so
```

The `zygisk/*.so` files are built from `userspace/zygisk_murasaki_bridge`.
