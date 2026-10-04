#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <string>

#define LOG_TAG "MXP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 嵌入的 dex 数据（由构建脚本从 classes.dex 生成）
#include "overlay_dex.h"

static JavaVM *g_jvm = nullptr;
static bool g_overlayStarted = false;
static pthread_mutex_t g_overlayMutex = PTHREAD_MUTEX_INITIALIZER;

// JNI_GetCreatedJavaVMs 不在 NDK 标准 API 里，通过 dlopen libnativehelper.so 获取
typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);
static JNI_GetCreatedJavaVMs_t getCreatedJavaVMs = nullptr;

static JavaVM *getJavaVM()
{
    if (g_jvm)
        return g_jvm;
    if (!getCreatedJavaVMs)
    {
        void *handle = dlopen("libnativehelper.so", RTLD_NOW);
        if (!handle)
        {
            LOGE("OverlayLoader: dlopen libnativehelper.so failed: %s", dlerror());
            return nullptr;
        }
        getCreatedJavaVMs = (JNI_GetCreatedJavaVMs_t)dlsym(handle, "JNI_GetCreatedJavaVMs");
        if (!getCreatedJavaVMs)
        {
            LOGE("OverlayLoader: dlsym JNI_GetCreatedJavaVMs failed: %s", dlerror());
            return nullptr;
        }
    }
    JavaVM *vms[4] = {nullptr};
    jsize count = 0;
    if (getCreatedJavaVMs(vms, 4, &count) == JNI_OK && count > 0)
        g_jvm = vms[0];
    return g_jvm;
}

// 在当前线程获取 JNIEnv（需要时 attach）
static JNIEnv *getEnv(bool *attached)
{
    *attached = false;
    JavaVM *vm = getJavaVM();
    if (!vm)
        return nullptr;
    JNIEnv *env = nullptr;
    jint ret = vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (ret == JNI_EDETACHED)
    {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
            *attached = true;
        else
            return nullptr;
    }
    else if (ret != JNI_OK)
        return nullptr;
    return env;
}

static void detachEnv(bool attached)
{
    if (attached && g_jvm)
        g_jvm->DetachCurrentThread();
}

// 检查并清除 JNI 异常
static void checkException(JNIEnv *env, const char *where)
{
    if (env->ExceptionCheck())
    {
        LOGE("JNI exception in %s", where);
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

bool OverlayLoader_start()
{
    pthread_mutex_lock(&g_overlayMutex);
    if (g_overlayStarted)
    {
        pthread_mutex_unlock(&g_overlayMutex);
        return true;
    }

    bool attached = false;
    JNIEnv *env = getEnv(&attached);
    if (!env)
    {
        LOGE("OverlayLoader: failed to get JNIEnv");
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    LOGI("OverlayLoader: starting, dex size=%zu", (size_t)overlay_dex_len);

    // 1. 创建 ByteBuffer 包裹 dex 数据
    jobject dexBuffer = env->NewDirectByteBuffer((void *)overlay_dex, (jlong)overlay_dex_len);
    if (!dexBuffer)
    {
        LOGE("OverlayLoader: NewDirectByteBuffer failed");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    // 2. 创建 ByteBuffer 数组
    jobjectArray dexArray = env->NewObjectArray(1, env->FindClass("java/nio/ByteBuffer"), dexBuffer);
    if (!dexArray)
    {
        LOGE("OverlayLoader: NewObjectArray failed");
        checkException(env, "NewObjectArray");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    // 3. 创建 InMemoryDexClassLoader
    jclass loaderClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!loaderClass)
    {
        LOGE("OverlayLoader: FindClass InMemoryDexClassLoader failed");
        checkException(env, "FindClass loader");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    jmethodID loaderCtor = env->GetMethodID(loaderClass, "<init>",
                                            "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!loaderCtor)
    {
        LOGE("OverlayLoader: GetMethodID <init> failed");
        checkException(env, "GetMethodID ctor");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    jobject classLoader = env->NewObject(loaderClass, loaderCtor, dexArray, nullptr);
    if (!classLoader)
    {
        LOGE("OverlayLoader: NewObject InMemoryDexClassLoader failed");
        checkException(env, "NewObject loader");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }
    checkException(env, "create loader");

    // 4. 通过 classLoader.loadClass() 加载 OverlayHelper
    jmethodID loadClassMethod = env->GetMethodID(loaderClass, "loadClass",
                                                 "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClassMethod)
    {
        LOGE("OverlayLoader: GetMethodID loadClass failed");
        checkException(env, "GetMethodID loadClass");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    jstring className = env->NewStringUTF("imgui.il2cpp.tool.OverlayHelper");
    jclass helperClass = (jclass)env->CallObjectMethod(classLoader, loadClassMethod, className);
    env->DeleteLocalRef(className);
    checkException(env, "loadClass OverlayHelper");

    if (!helperClass)
    {
        LOGE("OverlayLoader: loadClass returned null");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    // 5. 调用 OverlayHelper.start() 静态方法
    jmethodID startMethod = env->GetStaticMethodID(helperClass, "start", "()V");
    if (!startMethod)
    {
        LOGE("OverlayLoader: GetStaticMethodID start failed");
        checkException(env, "GetStaticMethodID start");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    env->CallStaticVoidMethod(helperClass, startMethod);
    checkException(env, "CallStaticVoidMethod start");

    LOGI("OverlayLoader: OverlayHelper.start() called");

    // 清理局部引用
    env->DeleteLocalRef(dexBuffer);
    env->DeleteLocalRef(dexArray);
    env->DeleteLocalRef(loaderClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(helperClass);

    g_overlayStarted = true;
    detachEnv(attached);
    pthread_mutex_unlock(&g_overlayMutex);
    return true;
}
