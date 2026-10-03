#include "zygisk.hpp"
#include "Tool/Targets.h"
#include "Includes/Logger.h"
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <jni.h>

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

extern void *hack_thread(void *);

static bool g_enableHack = false;
static std::vector<std::string> g_targets;

static std::string jstringToStd(JNIEnv *env, jstring s)
{
    if (!s)
        return "";
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return out;
}

static std::string parsePackageFromDataDir(const std::string &dir)
{
    // /data/user/0/com.foo.bar  or  /data/data/com.foo.bar
    auto pos = dir.rfind('/');
    if (pos == std::string::npos || pos + 1 >= dir.size())
        return dir;
    return dir.substr(pos + 1);
}

class Il2cppToolModule : public zygisk::ModuleBase
{
  public:
    void onLoad(Api *api, JNIEnv *env) override
    {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override
    {
        if (!args)
            return;
        // Read allow-list while still unsandboxed.
        g_targets = Targets::Load();
        std::string pkg;
        if (args->nice_name)
            pkg = jstringToStd(env_, args->nice_name);
        if (pkg.empty() && args->app_data_dir)
            pkg = parsePackageFromDataDir(jstringToStd(env_, args->app_data_dir));
        auto colon = pkg.find(':');
        if (colon != std::string::npos)
            pkg.resize(colon);

        if (g_targets.empty())
        {
            // No WebUI selection yet — do not inject into every app.
            g_enableHack = false;
            // 区分「无配置文件」与「文件存在但不可读」，便于排查时序/权限问题
            if (Targets::ConfigExists())
                LOGI("il2cpp_tool: target list unreadable, skip %s", pkg.c_str());
            else
                LOGI("il2cpp_tool: no target list, skip %s", pkg.c_str());
        }
        else
        {
            for (auto &t : g_targets)
            {
                if (t == pkg)
                {
                    g_enableHack = true;
                    break;
                }
            }
            LOGI("il2cpp_tool: %zu targets loaded, %s %s", g_targets.size(), pkg.c_str(),
                 g_enableHack ? "INJECT" : "skip");
        }

        if (!g_enableHack && api_)
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const AppSpecializeArgs *) override
    {
        if (!g_enableHack)
            return;
        pthread_t tid;
        if (pthread_create(&tid, nullptr, hack_thread, nullptr) != 0)
        {
            LOGE("il2cpp_tool: failed to start hack_thread");
        }
        else
        {
            pthread_detach(tid);
        }
    }

  private:
    Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(Il2cppToolModule)
