# Murasaki Bridge (Zygisk)

An independent **Zygisk module** that runs inside `system_server` and implements the `MRSK` bridge transaction:
\( ('M' \ll 24) | ('R' \ll 16) | ('S' \ll 8) | 'K' \).

It feeds **Murasaki** / **Shizuku** binders on-demand to apps that:

- declare Shizuku/Murasaki usage in their manifest, and
- are authorized by your `ksud`/Manager policy.

## How it works (high level)

- Hooks `android.os.Binder.execTransact` in `system_server`
- Intercepts the `MRSK` transaction sent to ActivityManager
- Validates the caller:
  - **Declared client**: checks Shizuku permission prefix or `meta-data` flags
  - **Authorized client**: calls `io.murasaki.IMurasakiService.isUidGrantedRoot(uid)` and fails closed if denied
- Returns the requested binder from `ServiceManager`:
  - `io.murasaki.IMurasakiService` (Murasaki)
  - `user_service` / `moe.shizuku.server.IShizukuService` (Shizuku)

## Build

Prerequisite: `ANDROID_NDK_HOME`.

```bash
./scripts/build.sh
```

Output: `murasaki_bridge_zygisk.zip` (install with Magisk).

## Credits

- **topjohnwu**: Magisk & Zygisk public API (`zygisk.hpp`) and the Zygisk module model.
- **Rikka / Sui contributors**: inspiration for the `system_server` bridge pattern.
