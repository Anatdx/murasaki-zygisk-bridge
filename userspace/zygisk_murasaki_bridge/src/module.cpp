#include <jni.h>

#include "zygisk.hpp"

#include "bridge.hpp"

namespace {

class MurasakiBridgeModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // We only need system_server. Keep footprint minimal in apps.
        if (api_) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs* args) override {
        (void)args;
        if (!api_ || !env_) {
            return;
        }

        // Hook android.os.Binder#execTransact(IJJI)Z in system_server only.
        // This avoids risky JNI table patching and is much more stable.
        JNINativeMethod m[] = {
            {"execTransact", "(IJJI)Z", (void*)murasaki::bridge::execTransact},
        };
        api_->hookJniNativeMethods(env_, "android/os/Binder", m, 1);
        if (m[0].fnPtr) {
            murasaki::bridge::setOriginalExecTransact(
                (murasaki::bridge::ExecTransact_t)m[0].fnPtr);
        }
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(MurasakiBridgeModule)

