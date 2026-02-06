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

        // Hook android.os.Binder#execTransact(IJJI)Z in system_server only (same as Sui's Binder hook).
        JNINativeMethod m[] = {
            {"execTransact", "(IJJI)Z", (void*)murasaki::bridge::execTransact},
        };
        api_->hookJniNativeMethods(env_, "android/os/Binder", m, 1);
        // Zygisk fills m[0].fnPtr with the original native method pointer after hook.
        void* orig = m[0].fnPtr;
        if (orig) {
            murasaki::bridge::setOriginalExecTransact(
                reinterpret_cast<murasaki::bridge::ExecTransact_t>(orig));
        }
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs* args) override {
        (void)args;
        // 启动时拉起 reid daemon（reid services / apd services / ksud services），供桥接向声明了 Murasaki/Shizuku 的 app 注入 Binder
        murasaki::bridge::startReidDaemonIfNeeded();
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(MurasakiBridgeModule)

