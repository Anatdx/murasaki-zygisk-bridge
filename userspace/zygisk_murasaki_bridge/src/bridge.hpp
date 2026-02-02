#pragma once

#include <jni.h>

namespace murasaki::bridge {

// Install JNI CallBooleanMethodV hook to intercept Binder.execTransact
bool install(JNIEnv* env);

}  // namespace murasaki::bridge

