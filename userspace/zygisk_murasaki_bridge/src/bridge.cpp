#include "bridge.hpp"

#include <android/log.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>

namespace murasaki::bridge {

static constexpr const char* LOG_TAG = "MurasakiBridge";

static constexpr jint TRANSACTION_MRSK = ('M' << 24) | ('R' << 16) | ('S' << 8) | 'K';
static constexpr jint ACTION_GET_SHIZUKU_BINDER = 1;
static constexpr jint ACTION_GET_MURASAKI_BINDER = 2;

static constexpr const char* AMS_DESCRIPTOR = "android.app.IActivityManager";

static constexpr const char* SERVICE_MURASAKI = "io.murasaki.IMurasakiService";
static constexpr const char* SERVICE_SHIZUKU = "user_service";
static constexpr const char* SERVICE_SHIZUKU_FALLBACK = "moe.shizuku.server.IShizukuService";

static constexpr const char* MURASAKI_AIDL_DESCRIPTOR = "io.murasaki.server.IMurasakiService";
static constexpr jint MURASAKI_TX_isUidGrantedRoot = 11;

static constexpr const char* SHIZUKU_API_PERMISSION_PREFIX = "moe.shizuku.manager.permission.API";
static constexpr const char* SHIZUKU_V3_META = "moe.shizuku.client.V3_SUPPORT";
static constexpr const char* MURASAKI_META = "io.murasaki.client.SUPPORT";

// Rei 优先：桥接读取白名单时先试 Rei 目录，兼容 YukiSU 旧路径
static constexpr const char* ALLOWLIST_REI = "/data/adb/rei/.murasaki_allowlist";
static constexpr const char* ALLOWLIST_KSU = "/data/adb/ksu/.murasaki_allowlist";

static ExecTransact_t g_orig_execTransact = nullptr;

static jclass g_cls_Binder = nullptr;
static jmethodID g_mid_getCallingUid = nullptr;
static jmethodID g_mid_getCallingPid = nullptr;

static jclass g_cls_Parcel = nullptr;
static jmethodID g_mid_Parcel_obtainPtr = nullptr;  // obtain(long)
static jmethodID g_mid_Parcel_obtain = nullptr;     // obtain()
static jmethodID g_mid_Parcel_recycle = nullptr;
static jmethodID g_mid_Parcel_setDataPosition = nullptr;
static jmethodID g_mid_Parcel_enforceInterface = nullptr;
static jmethodID g_mid_Parcel_readInt = nullptr;
static jmethodID g_mid_Parcel_readString = nullptr;
static jmethodID g_mid_Parcel_writeInterfaceToken = nullptr;
static jmethodID g_mid_Parcel_writeInt = nullptr;
static jmethodID g_mid_Parcel_writeNoException = nullptr;
static jmethodID g_mid_Parcel_writeStrongBinder = nullptr;
static jmethodID g_mid_Parcel_readException = nullptr;

static jclass g_cls_ServiceManager = nullptr;
static jmethodID g_mid_SM_getService = nullptr;

static jclass g_cls_IBinder = nullptr;
static jmethodID g_mid_IBinder_transact = nullptr;
static jmethodID g_mid_IBinder_pingBinder = nullptr;

static jclass g_cls_ActivityThread = nullptr;
static jmethodID g_mid_AT_currentActivityThread = nullptr;
static jmethodID g_mid_AT_getSystemContext = nullptr;

static jclass g_cls_Context = nullptr;
static jmethodID g_mid_Context_getPackageManager = nullptr;

static jclass g_cls_PackageManager = nullptr;
static jmethodID g_mid_PM_getPackagesForUid = nullptr;
static jmethodID g_mid_PM_getPackageInfo = nullptr;
static jmethodID g_mid_PM_getApplicationInfo = nullptr;
static jfieldID g_fid_PM_GET_PERMISSIONS = nullptr;
static jfieldID g_fid_PM_GET_META_DATA = nullptr;

static jclass g_cls_PackageInfo = nullptr;
static jfieldID g_fid_PackageInfo_requestedPermissions = nullptr;

static jclass g_cls_ApplicationInfo = nullptr;
static jfieldID g_fid_ApplicationInfo_metaData = nullptr;

static jclass g_cls_Bundle = nullptr;
static jmethodID g_mid_Bundle_getBoolean = nullptr;

static jclass g_cls_String = nullptr;
static jmethodID g_mid_String_startsWith = nullptr;

static inline void logd(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_DEBUG, LOG_TAG, fmt, ap);
    va_end(ap);
}

static inline void logw(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_WARN, LOG_TAG, fmt, ap);
    va_end(ap);
}

static void clear_exc(JNIEnv* env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

// Rei 优先：若任一白名单文件存在且可读，则 calling_uid 必须在其中；无文件或不可读时返回 true（交给 daemon）
static bool allowlist_file_contains_uid(jint calling_uid) {
    for (const char* path : {ALLOWLIST_REI, ALLOWLIST_KSU}) {
        FILE* f = fopen(path, "r");
        if (!f) continue;
        bool found = false;
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            int uid = -1;
            if (sscanf(line, "%d", &uid) == 1 && uid == static_cast<int>(calling_uid)) {
                found = true;
                break;
            }
        }
        fclose(f);
        if (found) return true;
        return false;  // 该文件存在但 uid 不在列表中
    }
    return true;  // 无文件或不可读时交给 daemon
}

static bool ensure_cache(JNIEnv* env) {
    if (g_cls_Binder) {
        return true;
    }

    auto make_global = [&](jclass local) -> jclass {
        if (!local) return nullptr;
        jclass g = (jclass) env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        return g;
    };

    // android.os.Binder
    g_cls_Binder = make_global(env->FindClass("android/os/Binder"));
    if (!g_cls_Binder) return false;
    g_mid_getCallingUid = env->GetStaticMethodID(g_cls_Binder, "getCallingUid", "()I");
    g_mid_getCallingPid = env->GetStaticMethodID(g_cls_Binder, "getCallingPid", "()I");

    // android.os.Parcel
    g_cls_Parcel = make_global(env->FindClass("android/os/Parcel"));
    if (!g_cls_Parcel) return false;
    g_mid_Parcel_obtainPtr = env->GetStaticMethodID(g_cls_Parcel, "obtain", "(J)Landroid/os/Parcel;");
    g_mid_Parcel_obtain = env->GetStaticMethodID(g_cls_Parcel, "obtain", "()Landroid/os/Parcel;");
    g_mid_Parcel_recycle = env->GetMethodID(g_cls_Parcel, "recycle", "()V");
    g_mid_Parcel_setDataPosition = env->GetMethodID(g_cls_Parcel, "setDataPosition", "(I)V");
    g_mid_Parcel_enforceInterface = env->GetMethodID(g_cls_Parcel, "enforceInterface", "(Ljava/lang/String;)V");
    g_mid_Parcel_readInt = env->GetMethodID(g_cls_Parcel, "readInt", "()I");
    g_mid_Parcel_readString = env->GetMethodID(g_cls_Parcel, "readString", "()Ljava/lang/String;");
    g_mid_Parcel_writeInterfaceToken = env->GetMethodID(g_cls_Parcel, "writeInterfaceToken", "(Ljava/lang/String;)V");
    g_mid_Parcel_writeInt = env->GetMethodID(g_cls_Parcel, "writeInt", "(I)V");
    g_mid_Parcel_writeNoException = env->GetMethodID(g_cls_Parcel, "writeNoException", "()V");
    g_mid_Parcel_writeStrongBinder = env->GetMethodID(g_cls_Parcel, "writeStrongBinder", "(Landroid/os/IBinder;)V");
    g_mid_Parcel_readException = env->GetMethodID(g_cls_Parcel, "readException", "()V");

    // android.os.ServiceManager
    g_cls_ServiceManager = make_global(env->FindClass("android/os/ServiceManager"));
    if (!g_cls_ServiceManager) return false;
    g_mid_SM_getService =
        env->GetStaticMethodID(g_cls_ServiceManager, "getService", "(Ljava/lang/String;)Landroid/os/IBinder;");

    // android.os.IBinder
    g_cls_IBinder = make_global(env->FindClass("android/os/IBinder"));
    if (!g_cls_IBinder) return false;
    g_mid_IBinder_transact = env->GetMethodID(g_cls_IBinder, "transact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z");
    g_mid_IBinder_pingBinder = env->GetMethodID(g_cls_IBinder, "pingBinder", "()Z");

    // android.app.ActivityThread (hidden, but JNI can still call)
    g_cls_ActivityThread = make_global(env->FindClass("android/app/ActivityThread"));
    if (!g_cls_ActivityThread) return false;
    g_mid_AT_currentActivityThread = env->GetStaticMethodID(g_cls_ActivityThread, "currentActivityThread",
                                                           "()Landroid/app/ActivityThread;");
    g_mid_AT_getSystemContext = env->GetMethodID(g_cls_ActivityThread, "getSystemContext", "()Landroid/content/Context;");

    // android.content.Context
    g_cls_Context = make_global(env->FindClass("android/content/Context"));
    if (!g_cls_Context) return false;
    g_mid_Context_getPackageManager =
        env->GetMethodID(g_cls_Context, "getPackageManager", "()Landroid/content/pm/PackageManager;");

    // android.content.pm.PackageManager
    g_cls_PackageManager = make_global(env->FindClass("android/content/pm/PackageManager"));
    if (!g_cls_PackageManager) return false;
    g_mid_PM_getPackagesForUid = env->GetMethodID(g_cls_PackageManager, "getPackagesForUid", "(I)[Ljava/lang/String;");
    g_mid_PM_getPackageInfo = env->GetMethodID(g_cls_PackageManager, "getPackageInfo",
                                               "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    g_mid_PM_getApplicationInfo = env->GetMethodID(g_cls_PackageManager, "getApplicationInfo",
                                                   "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
    g_fid_PM_GET_PERMISSIONS = env->GetStaticFieldID(g_cls_PackageManager, "GET_PERMISSIONS", "I");
    g_fid_PM_GET_META_DATA = env->GetStaticFieldID(g_cls_PackageManager, "GET_META_DATA", "I");

    // android.content.pm.PackageInfo
    g_cls_PackageInfo = make_global(env->FindClass("android/content/pm/PackageInfo"));
    if (!g_cls_PackageInfo) return false;
    g_fid_PackageInfo_requestedPermissions = env->GetFieldID(g_cls_PackageInfo, "requestedPermissions", "[Ljava/lang/String;");

    // android.content.pm.ApplicationInfo
    g_cls_ApplicationInfo = make_global(env->FindClass("android/content/pm/ApplicationInfo"));
    if (!g_cls_ApplicationInfo) return false;
    g_fid_ApplicationInfo_metaData = env->GetFieldID(g_cls_ApplicationInfo, "metaData", "Landroid/os/Bundle;");

    // android.os.Bundle
    g_cls_Bundle = make_global(env->FindClass("android/os/Bundle"));
    if (!g_cls_Bundle) return false;
    g_mid_Bundle_getBoolean = env->GetMethodID(g_cls_Bundle, "getBoolean", "(Ljava/lang/String;Z)Z");

    // java.lang.String
    g_cls_String = make_global(env->FindClass("java/lang/String"));
    if (!g_cls_String) return false;
    g_mid_String_startsWith = env->GetMethodID(g_cls_String, "startsWith", "(Ljava/lang/String;)Z");

    clear_exc(env);
    return true;
}

static jobject parcel_from_ptr(JNIEnv* env, jlong ptr) {
    if (!ptr || !g_mid_Parcel_obtainPtr) return nullptr;
    jobject p = env->CallStaticObjectMethod(g_cls_Parcel, g_mid_Parcel_obtainPtr, ptr);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return nullptr;
    }
    return p;
}

static jobject parcel_obtain(JNIEnv* env) {
    if (!g_mid_Parcel_obtain) return nullptr;
    jobject p = env->CallStaticObjectMethod(g_cls_Parcel, g_mid_Parcel_obtain);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return nullptr;
    }
    return p;
}

static jobject sm_get_service(JNIEnv* env, const char* name) {
    jstring jname = env->NewStringUTF(name);
    jobject b = env->CallStaticObjectMethod(g_cls_ServiceManager, g_mid_SM_getService, jname);
    env->DeleteLocalRef(jname);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return nullptr;
    }
    if (!b) return nullptr;
    jboolean ok = env->CallBooleanMethod(b, g_mid_IBinder_pingBinder);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return nullptr;
    }
    if (!ok) return nullptr;
    return b;
}

static bool is_declared_client(JNIEnv* env, jint uid) {
    // requestedPermissions prefix OR meta-data flags.
    jobject at = env->CallStaticObjectMethod(g_cls_ActivityThread, g_mid_AT_currentActivityThread);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return false;
    }
    if (!at) return false;

    jobject ctx = env->CallObjectMethod(at, g_mid_AT_getSystemContext);
    env->DeleteLocalRef(at);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return false;
    }
    if (!ctx) return false;

    jobject pm = env->CallObjectMethod(ctx, g_mid_Context_getPackageManager);
    env->DeleteLocalRef(ctx);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        return false;
    }
    if (!pm) return false;

    jobjectArray pkgs = (jobjectArray) env->CallObjectMethod(pm, g_mid_PM_getPackagesForUid, uid);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        env->DeleteLocalRef(pm);
        return false;
    }
    if (!pkgs) {
        env->DeleteLocalRef(pm);
        return false;
    }

    jint n = env->GetArrayLength(pkgs);
    jstring jperm_prefix = env->NewStringUTF(SHIZUKU_API_PERMISSION_PREFIX);
    jstring jmeta_shizuku = env->NewStringUTF(SHIZUKU_V3_META);
    jstring jmeta_murasaki = env->NewStringUTF(MURASAKI_META);

    jint flag_perm = 0;
    jint flag_meta = 0;
    if (g_fid_PM_GET_PERMISSIONS) {
        flag_perm = env->GetStaticIntField(g_cls_PackageManager, g_fid_PM_GET_PERMISSIONS);
    }
    if (g_fid_PM_GET_META_DATA) {
        flag_meta = env->GetStaticIntField(g_cls_PackageManager, g_fid_PM_GET_META_DATA);
    }

    bool declared = false;

    for (jint i = 0; i < n && !declared; ++i) {
        jstring pkg = (jstring) env->GetObjectArrayElement(pkgs, i);
        if (!pkg) continue;

        // requestedPermissions check
        jobject pi = env->CallObjectMethod(pm, g_mid_PM_getPackageInfo, pkg, flag_perm);
        if (env->ExceptionCheck()) {
            clear_exc(env);
        } else if (pi && g_fid_PackageInfo_requestedPermissions) {
            jobjectArray reqPerms = (jobjectArray) env->GetObjectField(pi, g_fid_PackageInfo_requestedPermissions);
            if (reqPerms) {
                jint pn = env->GetArrayLength(reqPerms);
                for (jint j = 0; j < pn; ++j) {
                    jstring p = (jstring) env->GetObjectArrayElement(reqPerms, j);
                    if (!p) continue;
                    jboolean sw = env->CallBooleanMethod(p, g_mid_String_startsWith, jperm_prefix);
                    env->DeleteLocalRef(p);
                    if (env->ExceptionCheck()) {
                        clear_exc(env);
                        continue;
                    }
                    if (sw) {
                        declared = true;
                        break;
                    }
                }
                env->DeleteLocalRef(reqPerms);
            }
        }
        if (pi) env->DeleteLocalRef(pi);

        // meta-data check
        if (!declared) {
            jobject ai = env->CallObjectMethod(pm, g_mid_PM_getApplicationInfo, pkg, flag_meta);
            if (env->ExceptionCheck()) {
                clear_exc(env);
            } else if (ai && g_fid_ApplicationInfo_metaData) {
                jobject bundle = env->GetObjectField(ai, g_fid_ApplicationInfo_metaData);
                if (bundle) {
                    jboolean b1 = env->CallBooleanMethod(bundle, g_mid_Bundle_getBoolean, jmeta_shizuku, JNI_FALSE);
                    if (env->ExceptionCheck()) clear_exc(env);
                    jboolean b2 = env->CallBooleanMethod(bundle, g_mid_Bundle_getBoolean, jmeta_murasaki, JNI_FALSE);
                    if (env->ExceptionCheck()) clear_exc(env);
                    if (b1 || b2) declared = true;
                    env->DeleteLocalRef(bundle);
                }
            }
            if (ai) env->DeleteLocalRef(ai);
        }

        env->DeleteLocalRef(pkg);
    }

    env->DeleteLocalRef(jperm_prefix);
    env->DeleteLocalRef(jmeta_shizuku);
    env->DeleteLocalRef(jmeta_murasaki);
    env->DeleteLocalRef(pkgs);
    env->DeleteLocalRef(pm);
    return declared;
}

static bool murasaki_is_uid_allowed(JNIEnv* env, jobject murasaki_binder, jint uid) {
    // Call IMurasakiService.isUidGrantedRoot(uid) via raw transact
    if (!murasaki_binder) return false;

    jobject data = parcel_obtain(env);
    jobject reply = parcel_obtain(env);
    if (!data || !reply) {
        if (data) env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    jstring desc = env->NewStringUTF(MURASAKI_AIDL_DESCRIPTOR);
    env->CallVoidMethod(data, g_mid_Parcel_writeInterfaceToken, desc);
    env->DeleteLocalRef(desc);
    env->CallVoidMethod(data, g_mid_Parcel_writeInt, uid);

    jboolean ok = env->CallBooleanMethod(murasaki_binder, g_mid_IBinder_transact,
                                         MURASAKI_TX_isUidGrantedRoot, data, reply, 0);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        ok = JNI_FALSE;
    }

    bool allowed = false;
    if (ok) {
        env->CallVoidMethod(reply, g_mid_Parcel_readException);
        if (env->ExceptionCheck()) {
            clear_exc(env);
        } else {
            jint v = env->CallIntMethod(reply, g_mid_Parcel_readInt);
            if (env->ExceptionCheck()) {
                clear_exc(env);
            } else {
                allowed = (v != 0);
            }
        }
    }

    env->CallVoidMethod(data, g_mid_Parcel_recycle);
    env->CallVoidMethod(reply, g_mid_Parcel_recycle);
    env->DeleteLocalRef(data);
    env->DeleteLocalRef(reply);
    return allowed;
}

static bool handle_bridge(JNIEnv* env, jint code, jlong dataObj, jlong replyObj) {
    if (code != TRANSACTION_MRSK) {
        return false;
    }
    if (!ensure_cache(env)) {
        return false;
    }

    jobject data = parcel_from_ptr(env, dataObj);
    jobject reply = parcel_from_ptr(env, replyObj);
    if (!data) {
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    // enforce interface
    jstring ams_desc = env->NewStringUTF(AMS_DESCRIPTOR);
    env->CallVoidMethod(data, g_mid_Parcel_enforceInterface, ams_desc);
    env->DeleteLocalRef(ams_desc);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    jint action = env->CallIntMethod(data, g_mid_Parcel_readInt);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    jint callingUid = env->CallStaticIntMethod(g_cls_Binder, g_mid_getCallingUid);
    (void)env->CallStaticIntMethod(g_cls_Binder, g_mid_getCallingPid);
    if (env->ExceptionCheck()) {
        clear_exc(env);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    // Fail closed unless declared
    if (!is_declared_client(env, callingUid)) {
        env->CallVoidMethod(data, g_mid_Parcel_setDataPosition, 0);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_setDataPosition, 0);
        env->CallVoidMethod(data, g_mid_Parcel_recycle);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_recycle);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    // Rei 优先：白名单文件存在时，若 UID 不在 /data/adb/rei/.murasaki_allowlist（或 ksu 路径）则直接拒绝
    if (!allowlist_file_contains_uid(callingUid)) {
        env->CallVoidMethod(data, g_mid_Parcel_setDataPosition, 0);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_setDataPosition, 0);
        env->CallVoidMethod(data, g_mid_Parcel_recycle);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_recycle);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    // Daemon may start slightly after system_server; retry a few times.
    jobject murasaki = nullptr;
    for (int attempt = 0; attempt <= 3 && !murasaki; ++attempt) {
        if (attempt > 0) {
            usleep(250000);  // 250ms
        }
        murasaki = sm_get_service(env, SERVICE_MURASAKI);
    }
    if (!murasaki) {
        logw("murasaki binder not found in ServiceManager. Ensure reid/apd 'services' runs at boot (e.g. module service.sh) and /data/adb/rei/.murasaki_allowlist exists.");
        env->CallVoidMethod(data, g_mid_Parcel_setDataPosition, 0);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_setDataPosition, 0);
        env->CallVoidMethod(data, g_mid_Parcel_recycle);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_recycle);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    // Ask ksud whether this uid is allowed (manager-controlled)
    if (!murasaki_is_uid_allowed(env, murasaki, callingUid)) {
        env->DeleteLocalRef(murasaki);
        env->CallVoidMethod(data, g_mid_Parcel_setDataPosition, 0);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_setDataPosition, 0);
        env->CallVoidMethod(data, g_mid_Parcel_recycle);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_recycle);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    jobject out_binder = nullptr;
    if (action == ACTION_GET_MURASAKI_BINDER) {
        out_binder = murasaki;  // already a local ref
    } else if (action == ACTION_GET_SHIZUKU_BINDER) {
        jobject shizuku = sm_get_service(env, SERVICE_SHIZUKU);
        if (!shizuku) {
            shizuku = sm_get_service(env, SERVICE_SHIZUKU_FALLBACK);
        }
        out_binder = shizuku;  // may be null
        env->DeleteLocalRef(murasaki);
    } else {
        env->DeleteLocalRef(murasaki);
        env->CallVoidMethod(data, g_mid_Parcel_setDataPosition, 0);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_setDataPosition, 0);
        env->CallVoidMethod(data, g_mid_Parcel_recycle);
        if (reply) env->CallVoidMethod(reply, g_mid_Parcel_recycle);
        env->DeleteLocalRef(data);
        if (reply) env->DeleteLocalRef(reply);
        return false;
    }

    if (reply) {
        env->CallVoidMethod(reply, g_mid_Parcel_writeNoException);
        env->CallVoidMethod(reply, g_mid_Parcel_writeStrongBinder, out_binder);
        clear_exc(env);
    }

    // Reset position + recycle only when consumed
    env->CallVoidMethod(data, g_mid_Parcel_setDataPosition, 0);
    if (reply) env->CallVoidMethod(reply, g_mid_Parcel_setDataPosition, 0);
    env->CallVoidMethod(data, g_mid_Parcel_recycle);
    if (reply) env->CallVoidMethod(reply, g_mid_Parcel_recycle);

    env->DeleteLocalRef(data);
    if (reply) env->DeleteLocalRef(reply);
    if (out_binder && out_binder != murasaki) {
        env->DeleteLocalRef(out_binder);
    } else if (action == ACTION_GET_MURASAKI_BINDER) {
        env->DeleteLocalRef(out_binder);
    }

    logd("bridge ok: uid=%d action=%d", callingUid, action);
    return true;
}

void setOriginalExecTransact(ExecTransact_t orig) {
    g_orig_execTransact = orig;
}

jboolean execTransact(JNIEnv* env, jobject thiz, jint code, jlong dataObj, jlong replyObj, jint flags) {
    (void)flags;

    // Handle only our bridge transaction. Everything else falls back to original.
    if (code == TRANSACTION_MRSK) {
        bool consumed = handle_bridge(env, code, dataObj, replyObj);
        if (consumed) {
            return JNI_TRUE;
        }
        // For MRSK not handled, return false to match Sui behavior (client will fall back).
        return JNI_FALSE;
    }

    if (g_orig_execTransact) {
        return g_orig_execTransact(env, thiz, code, dataObj, replyObj, flags);
    }
    // Should never happen, but fail open by letting Binder treat it as unhandled.
    return JNI_FALSE;
}

}  // namespace murasaki::bridge

