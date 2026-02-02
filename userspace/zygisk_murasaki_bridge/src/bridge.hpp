#pragma once

#include <jni.h>

namespace murasaki::bridge {

using ExecTransact_t = jboolean (*)(JNIEnv*, jobject, jint, jlong, jlong, jint);

// Hook target: android.os.Binder#execTransact(IJJI)Z
// Returns JNI_TRUE if consumed (handled as bridge), otherwise calls original.
jboolean execTransact(JNIEnv* env, jobject thiz, jint code, jlong dataObj, jlong replyObj, jint flags);

// Save original function pointer (provided by Zygisk hookJniNativeMethods).
void setOriginalExecTransact(ExecTransact_t orig);

}  // namespace murasaki::bridge

