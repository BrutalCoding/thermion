#include <jni.h>
#include <android/native_window_jni.h>
#include <android/native_activity.h>
#include <android/log.h>

#include <string>
#include <unordered_map>
#include <dlfcn.h>
#include "private/backend/VirtualMachineEnv.h"

// Global references to keep callbacks alive
static JavaVM *g_vm;

// Utility function to get JNIEnv
static JNIEnv *getEnv()
{
    JNIEnv *env;
    g_vm->AttachCurrentThread(&env, nullptr);
    return env;
}

extern "C"
{

#include "ResourceBuffer.h"
    JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
    {
        __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "JNI ONLOAD");
        g_vm = vm;

        // Load libthermion_dart.so
        void *library = dlopen("libthermion_dart.so", RTLD_NOW | RTLD_GLOBAL);
        if (!library)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "Failed to load library: %s", dlerror());
            return JNI_VERSION_1_6;
        }
        __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "Successfully loaded libthermion_dart.so");

        // Clear any existing errors
        dlerror();

        // Try to get the symbol from the specific library handle
        void *symbol = dlsym(library, "filament_VirtualMachineEnv_JNI_OnLoad");
        const char *error = dlerror();
        if (symbol && !error)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "GOT SYMBOL");
            // Cast and call the function
            typedef jint (*OnLoadFunc)(JavaVM *, void *);
            auto func = reinterpret_cast<OnLoadFunc>(symbol);
            func(vm, reserved);
        }
        else
        {
            __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "NO SYMBOL: %s", error ? error : "unknown error");

            // Try the mangled name as a fallback
            symbol = dlsym(library, "_ZN8filament17VirtualMachineEnv10JNI_OnLoadEP7_JavaVM");
            error = dlerror();
            if (symbol && !error)
            {
                __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "GOT SYMBOL (mangled)");
                typedef jint (*OnLoadFunc)(JavaVM *, void *);
                auto func = reinterpret_cast<OnLoadFunc>(symbol);
                func(vm, reserved);
            }
            else
            {
                __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid", "NO SYMBOL (mangled): %s", error ? error : "unknown error");
            }
        }

        return JNI_VERSION_1_6;
    }

    JNIEXPORT jlong JNICALL
    Java_dev_thermion_android_ThermionFlutterPlugin_getNativeWindowFromSurface(
        JNIEnv *env, jobject thiz, jobject surface)
    {
        return (jlong)ANativeWindow_fromSurface(env, surface);
    }

    // First define the callback types that match the Java method signatures
    typedef ResourceBuffer (*LoadResourceCallback)(const char *path, long owner);
    typedef void (*FreeResourceCallback)(ResourceBuffer buffer, long owner);

    // Global reference to store the Java object
    static jclass g_pluginClass = nullptr;
    static jobject g_pluginObject = nullptr;
    static jmethodID g_loadResourceMethod = nullptr;

    // Global map to store global references to ByteBuffers
    static std::unordered_map<int32_t, jobject> g_bufferRefs;
    static int32_t _lastId = -1;

    ResourceBuffer nativeLoadResourceCallback(const char *path, void *owner)
    {
        JNIEnv *env = getEnv();
        if (!env || !g_pluginObject || !g_loadResourceMethod)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                "Invalid JNI state in nativeLoadResourceCallback");
            return ResourceBuffer(nullptr, 0, 0);
        }

        // Convert C string to Java string
        jstring jPath = env->NewStringUTF(path);
        if (!jPath)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                "Failed to create Java string from path");
            return ResourceBuffer(nullptr, 0, 0);
        }

        // Call Java method loadResourceFromOwner
        jobject localByteBuffer = env->CallObjectMethod(
            g_pluginObject,
            g_loadResourceMethod,
            jPath,
            (jlong)owner);

        // Clean up the local reference to jPath
        env->DeleteLocalRef(jPath);

        // Check for exceptions
        if (env->ExceptionCheck())
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                "Exception occurred while loading resource");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return ResourceBuffer(nullptr, 0, 0);
        }

        // If no ByteBuffer was returned
        if (!localByteBuffer)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                "No buffer returned for path: %s", path);
            return ResourceBuffer(nullptr, 0, 0);
        }

        // Create a global reference to the ByteBuffer
        jobject globalByteBuffer = env->NewGlobalRef(localByteBuffer);
        env->DeleteLocalRef(localByteBuffer); // Clean up local reference

        // Get the direct buffer address and capacity
        void *bufferPtr = env->GetDirectBufferAddress(globalByteBuffer);
        jlong capacity = env->GetDirectBufferCapacity(globalByteBuffer);

        if (!bufferPtr || capacity < 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                "Invalid buffer returned from Java");
            env->DeleteGlobalRef(globalByteBuffer);
            return ResourceBuffer(nullptr, 0, 0);
        }

        _lastId++;
        int32_t bufferId = _lastId;

        // Store the global reference
        g_bufferRefs[bufferId] = globalByteBuffer;

        return ResourceBuffer(
            bufferPtr,
            static_cast<int32_t>(capacity),
            bufferId);
    }

    void nativeFreeResourceCallback(ResourceBuffer buffer, void *owner)
    {
        JNIEnv *env = getEnv();
        if (!env || !g_pluginObject)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                "Invalid JNI state in nativeFreeResourceCallback");
            return;
        }

        // Find and delete the global reference
        auto it = g_bufferRefs.find(buffer.id);
        if (it != g_bufferRefs.end())
        {
            env->DeleteGlobalRef(it->second);
            g_bufferRefs.erase(it);
        }
    }

    // Add this to the cleanupGlobals function
    void cleanupGlobals(JNIEnv *env)
    {
        // Clean up existing global references
        if (g_pluginClass != nullptr)
        {
            env->DeleteGlobalRef(g_pluginClass);
            g_pluginClass = nullptr;
        }
        if (g_pluginObject != nullptr)
        {
            env->DeleteGlobalRef(g_pluginObject);
            g_pluginObject = nullptr;
        }

        // Clean up any remaining buffer references
        for (const auto &pair : g_bufferRefs)
        {
            env->DeleteGlobalRef(pair.second);
        }
        g_bufferRefs.clear();
    }

    JNIEXPORT jlong JNICALL
    Java_dev_thermion_android_ThermionFlutterPlugin_makeResourceLoaderWrapper(
        JNIEnv *env, jobject thiz)
    {

        // Store global references to the Java object and its class
        if (g_pluginClass == nullptr)
        {
            jclass localClass = env->GetObjectClass(thiz);
            g_pluginClass = (jclass)env->NewGlobalRef(localClass); // Create global reference first
            env->DeleteLocalRef(localClass);                       // Delete the local reference as it's no longer needed
        }

        if (g_pluginClass == nullptr)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid", "g_pluginClass is null");
        }
        else
        {
            __android_log_print(ANDROID_LOG_INFO, "ThermionFlutterAndroid", "g_pluginClass is NOT null");
        }

        jclass clazz = g_pluginClass;
        jmethodID toString = env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
        jstring className = (jstring)env->CallObjectMethod(clazz, toString);
        const char *name = env->GetStringUTFChars(className, nullptr);
        __android_log_print(ANDROID_LOG_INFO, "ThermionFlutterAndroid", "Class name: %s", name);
        env->ReleaseStringUTFChars(className, name);

        if (g_pluginObject == nullptr)
        {
            g_pluginObject = env->NewGlobalRef(thiz);
        }

        if (g_pluginObject == nullptr)
        {
            __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid", "g_pluginObject is null");
        }
        else
        {
            __android_log_print(ANDROID_LOG_INFO, "ThermionFlutterAndroid", "g_pluginObject is NOT null");
        }

        if (g_loadResourceMethod == nullptr)
        {
            // First verify the class is valid
            jmethodID getNameMethod = env->GetMethodID(
                env->GetObjectClass(g_pluginClass),
                "getName",
                "()Ljava/lang/String;");
            jstring className = (jstring)env->CallObjectMethod(g_pluginClass, getNameMethod);
            const char *nameStr = env->GetStringUTFChars(className, nullptr);
            __android_log_print(ANDROID_LOG_DEBUG, "ThermionFlutterAndroid",
                                "Looking up methods in class: %s", nameStr);
            env->ReleaseStringUTFChars(className, nameStr);

            g_loadResourceMethod = env->GetMethodID(
                g_pluginClass,
                "loadResourceFromOwner",
                "(Ljava/lang/String;J)Ljava/nio/ByteBuffer;");

            if (env->ExceptionCheck())
            {
                __android_log_print(ANDROID_LOG_ERROR, "ThermionFlutterAndroid",
                                    "Failed to get loadResourceFromOwner method");
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }

        // Create and return the ResourceLoaderWrapper
        ResourceLoaderWrapper *rlw = new ResourceLoaderWrapper();
        rlw->loadToOut = nullptr;
        rlw->freeResource = nullptr;
        rlw->loadResource = nullptr;
        rlw->loadFromOwner = &nativeLoadResourceCallback;
        rlw->freeFromOwner = &nativeFreeResourceCallback;
        rlw->owner = (void *)env->GetLongField(thiz, env->GetFieldID(g_pluginClass, "nativePtr", "J"));

        return (jlong)rlw;
    }

    // Add this to JNI_OnUnload
    JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved)
    {
        JNIEnv *env;
        if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK)
        {
            cleanupGlobals(env);
        }
    }
}