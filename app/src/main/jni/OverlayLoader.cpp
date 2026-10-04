#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <string>
#include <cstdio>

#define LOG_TAG "MXP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 嵌入的 dex 数据（由构建脚本从 classes.dex 生成）
#include "overlay_dex.h"

// Main.cpp 里定义的 native 方法，Zygisk dlopen 注入后 JVM 不知道，需手动 RegisterNatives
extern "C" {
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_NativeMethods_onDrawFrame(JNIEnv *, jclass);
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_NativeMethods_onSurfaceChanged(JNIEnv *, jclass, jint, jint);
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_NativeMethods_onSurfaceCreate(JNIEnv *, jclass);
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_OverlayHelper_nativeOnTouch(JNIEnv *, jclass, jint, jfloat, jfloat, jint, jlong);
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_OverlayHelper_nativeOnSurfaceDestroyed(JNIEnv *, jclass);
}

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

// 获取应用 cache 目录（用于 DexClassLoader 回退时写 dex 文件）
static std::string getCacheDir(JNIEnv *env)
{
    try
    {
        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (!activityThreadClass)
        {
            checkException(env, "FindClass ActivityThread");
            return "";
        }
        jmethodID currentApplicationMethod = env->GetStaticMethodID(activityThreadClass, "currentApplication",
                                                                     "()Landroid/app/Application;");
        if (!currentApplicationMethod)
        {
            checkException(env, "GetStaticMethodID currentApplication");
            env->DeleteLocalRef(activityThreadClass);
            return "";
        }
        jobject app = env->CallStaticObjectMethod(activityThreadClass, currentApplicationMethod);
        env->DeleteLocalRef(activityThreadClass);
        if (!app)
        {
            checkException(env, "CallStaticObjectMethod currentApplication");
            return "";
        }
        jclass contextClass = env->FindClass("android/content/Context");
        jmethodID getCacheDirMethod = env->GetMethodID(contextClass, "getCacheDir", "()Ljava/io/File;");
        jobject cacheDir = env->CallObjectMethod(app, getCacheDirMethod);
        env->DeleteLocalRef(contextClass);
        env->DeleteLocalRef(app);
        if (!cacheDir)
        {
            checkException(env, "CallObjectMethod getCacheDir");
            return "";
        }
        jclass fileClass = env->FindClass("java/io/File");
        jmethodID getPathMethod = env->GetMethodID(fileClass, "getPath", "()Ljava/lang/String;");
        jstring pathStr = (jstring)env->CallObjectMethod(cacheDir, getPathMethod);
        env->DeleteLocalRef(fileClass);
        env->DeleteLocalRef(cacheDir);
        if (!pathStr)
        {
            checkException(env, "CallObjectMethod getPath");
            return "";
        }
        const char *path = env->GetStringUTFChars(pathStr, nullptr);
        std::string result(path);
        env->ReleaseStringUTFChars(pathStr, path);
        env->DeleteLocalRef(pathStr);
        return result;
    }
    catch (...)
    {
        LOGE("OverlayLoader: getCacheDir exception");
        return "";
    }
}

// 把 dex 数据写到 cache 目录，返回文件路径
static std::string writeDexToCache(JNIEnv *env)
{
    std::string cacheDir = getCacheDir(env);
    if (cacheDir.empty())
    {
        LOGE("OverlayLoader: getCacheDir failed");
        return "";
    }
    std::string dexPath = cacheDir + "/mcp_overlay.dex";
    FILE *f = fopen(dexPath.c_str(), "wb");
    if (!f)
    {
        LOGE("OverlayLoader: fopen %s failed", dexPath.c_str());
        return "";
    }
    size_t written = fwrite(overlay_dex, 1, overlay_dex_len, f);
    fclose(f);
    if (written != overlay_dex_len)
    {
        LOGE("OverlayLoader: fwrite only %zu/%zu bytes", written, (size_t)overlay_dex_len);
        return "";
    }
    LOGI("OverlayLoader: dex written to %s (%zu bytes)", dexPath.c_str(), (size_t)overlay_dex_len);
    return dexPath;
}

// 创建 ClassLoader：优先 InMemoryDexClassLoader（API 26+），失败回退 DexClassLoader
// 返回 classLoader 对象，失败返回 nullptr
static jobject createClassLoader(JNIEnv *env)
{
    // 路径 1：InMemoryDexClassLoader（内存加载，无需文件权限）
    jclass imdcClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (imdcClass)
    {
        jmethodID imdcCtor = env->GetMethodID(imdcClass, "<init>",
                                               "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (imdcCtor)
        {
            jobject dexBuffer = env->NewDirectByteBuffer((void *)overlay_dex, (jlong)overlay_dex_len);
            if (dexBuffer)
            {
                jclass byteBufferClass = env->FindClass("java/nio/ByteBuffer");
                jobjectArray dexArray = env->NewObjectArray(1, byteBufferClass, dexBuffer);
                env->DeleteLocalRef(byteBufferClass);
                if (dexArray)
                {
                    jobject loader = env->NewObject(imdcClass, imdcCtor, dexArray, nullptr);
                    checkException(env, "NewObject InMemoryDexClassLoader");
                    env->DeleteLocalRef(dexArray);
                    env->DeleteLocalRef(dexBuffer);
                    env->DeleteLocalRef(imdcClass);
                    if (loader)
                    {
                        LOGI("OverlayLoader: InMemoryDexClassLoader created");
                        return loader;
                    }
                    LOGE("OverlayLoader: InMemoryDexClassLoader NewObject returned null");
                }
                else
                {
                    checkException(env, "NewObjectArray dexArray");
                    env->DeleteLocalRef(dexBuffer);
                }
            }
            else
            {
                checkException(env, "NewDirectByteBuffer");
            }
        }
        else
        {
            checkException(env, "GetMethodID InMemoryDexClassLoader<init>");
        }
        env->DeleteLocalRef(imdcClass);
    }
    else
    {
        checkException(env, "FindClass InMemoryDexClassLoader");
        LOGI("OverlayLoader: InMemoryDexClassLoader unavailable (API < 26?), falling back to DexClassLoader");
    }

    // 路径 2：DexClassLoader（写 dex 到 cache 目录）
    std::string dexPath = writeDexToCache(env);
    if (dexPath.empty())
    {
        LOGE("OverlayLoader: writeDexToCache failed");
        return nullptr;
    }

    jclass dclClass = env->FindClass("dalvik/system/DexClassLoader");
    if (!dclClass)
    {
        checkException(env, "FindClass DexClassLoader");
        return nullptr;
    }
    jmethodID dclCtor = env->GetMethodID(dclClass, "<init>",
                                          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    if (!dclCtor)
    {
        checkException(env, "GetMethodID DexClassLoader<init>");
        env->DeleteLocalRef(dclClass);
        return nullptr;
    }
    jstring dexPathStr = env->NewStringUTF(dexPath.c_str());
    jstring optimizedDir = env->NewStringUTF(getCacheDir(env).c_str());
    jobject loader = env->NewObject(dclClass, dclCtor, dexPathStr, optimizedDir, nullptr, nullptr);
    checkException(env, "NewObject DexClassLoader");
    env->DeleteLocalRef(dexPathStr);
    env->DeleteLocalRef(optimizedDir);
    env->DeleteLocalRef(dclClass);
    if (loader)
    {
        LOGI("OverlayLoader: DexClassLoader created (fallback)");
        return loader;
    }
    LOGE("OverlayLoader: DexClassLoader NewObject returned null");
    return nullptr;
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
        LOGE("OverlayLoader: failed to get JNIEnv (JavaVM attach failed)");
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    LOGI("OverlayLoader: starting, dex size=%zu", (size_t)overlay_dex_len);

    // 1. 创建 ClassLoader（InMemoryDexClassLoader 优先，DexClassLoader 回退）
    jobject classLoader = createClassLoader(env);
    if (!classLoader)
    {
        LOGE("OverlayLoader: failed to create any ClassLoader");
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    // 2. 通过 classLoader.loadClass() 加载 OverlayHelper
    jclass loaderClass = env->GetObjectClass(classLoader);
    jmethodID loadClassMethod = env->GetMethodID(loaderClass, "loadClass",
                                                 "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!loadClassMethod)
    {
        LOGE("OverlayLoader: GetMethodID loadClass failed");
        checkException(env, "GetMethodID loadClass");
        env->DeleteLocalRef(loaderClass);
        env->DeleteLocalRef(classLoader);
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
        LOGE("OverlayLoader: loadClass OverlayHelper returned null (ClassNotFoundException)");
        env->DeleteLocalRef(loaderClass);
        env->DeleteLocalRef(classLoader);
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }
    LOGI("OverlayLoader: OverlayHelper loaded successfully");

    // 3. RegisterNatives：Zygisk dlopen 注入的 so JVM 不知道，必须手动注册 native 方法
    // 注册 NativeMethods
    jstring nmName = env->NewStringUTF("imgui.il2cpp.tool.NativeMethods");
    jclass nmClass = (jclass)env->CallObjectMethod(classLoader, loadClassMethod, nmName);
    env->DeleteLocalRef(nmName);
    if (nmClass)
    {
        JNINativeMethod nmMethods[] = {
            {(char *)"onDrawFrame", (char *)"()V", (void *)Java_imgui_il2cpp_tool_NativeMethods_onDrawFrame},
            {(char *)"onSurfaceChanged", (char *)"(II)V", (void *)Java_imgui_il2cpp_tool_NativeMethods_onSurfaceChanged},
            {(char *)"onSurfaceCreate", (char *)"()V", (void *)Java_imgui_il2cpp_tool_NativeMethods_onSurfaceCreate},
        };
        if (env->RegisterNatives(nmClass, nmMethods, 3) == JNI_OK)
            LOGI("OverlayLoader: Registered 3 NativeMethods");
        else
            LOGE("OverlayLoader: RegisterNatives NativeMethods failed");
        checkException(env, "RegisterNatives NativeMethods");
        env->DeleteLocalRef(nmClass);
    }
    else
    {
        LOGE("OverlayLoader: loadClass NativeMethods failed");
        checkException(env, "loadClass NativeMethods");
    }

    // 注册 OverlayHelper.nativeOnTouch + nativeOnSurfaceDestroyed
    JNINativeMethod ohMethods[] = {
        {(char *)"nativeOnTouch", (char *)"(IFFIJ)V", (void *)Java_imgui_il2cpp_tool_OverlayHelper_nativeOnTouch},
        {(char *)"nativeOnSurfaceDestroyed", (char *)"()V", (void *)Java_imgui_il2cpp_tool_OverlayHelper_nativeOnSurfaceDestroyed},
    };
    if (env->RegisterNatives(helperClass, ohMethods, 2) == JNI_OK)
        LOGI("OverlayLoader: Registered nativeOnTouch + nativeOnSurfaceDestroyed");
    else
        LOGE("OverlayLoader: RegisterNatives OverlayHelper failed");
    checkException(env, "RegisterNatives OverlayHelper");

    // 4. 调用 OverlayHelper.start() 静态方法
    jmethodID startMethod = env->GetStaticMethodID(helperClass, "start", "()V");
    if (!startMethod)
    {
        LOGE("OverlayLoader: GetStaticMethodID start failed");
        checkException(env, "GetStaticMethodID start");
        env->DeleteLocalRef(loaderClass);
        env->DeleteLocalRef(classLoader);
        env->DeleteLocalRef(helperClass);
        detachEnv(attached);
        pthread_mutex_unlock(&g_overlayMutex);
        return false;
    }

    env->CallStaticVoidMethod(helperClass, startMethod);
    checkException(env, "CallStaticVoidMethod start");

    LOGI("OverlayLoader: OverlayHelper.start() called, overlay bridge ready; waiting for a foreground Activity");

    // 清理局部引用
    env->DeleteLocalRef(loaderClass);
    env->DeleteLocalRef(classLoader);
    env->DeleteLocalRef(helperClass);

    g_overlayStarted = true;
    detachEnv(attached);
    pthread_mutex_unlock(&g_overlayMutex);
    return true;
}
