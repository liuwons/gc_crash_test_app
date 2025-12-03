#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>

#define TAG "GCCrashTest-Native"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Global state
static jmethodID g_hooked_getCallingUid = nullptr;
static void* g_original_getCallingUid = nullptr;
static int g_native_function_offset = -1;
static bool g_hook_active = false;
static JavaVM* g_jvm = nullptr;
static int g_api_level = 0;

// For calling back to Java from hooked method
static jclass g_mainActivityClass = nullptr;
static jmethodID g_onGetCallingUidMethod = nullptr;

// Helper: Get JNIEnv for current thread
static JNIEnv* GetJNIEnv() {
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_jvm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

static bool isIndexId(jmethodID mid) {
    return (reinterpret_cast<uintptr_t>(mid) % 2) != 0;
}

static uintptr_t getArtMethodPointer(JNIEnv* env, jmethodID mid, jclass clazz, bool isStatic) {
    if (g_api_level >= 30 && isIndexId(mid)) {
        jobject methodObject = env->ToReflectedMethod(clazz, mid, isStatic);
        if (!methodObject) {
            LOGE("Failed to convert jmethodID to reflected method");
            return 0;
        }
        
        jclass methodClass = env->FindClass("java/lang/reflect/Method");
        if (!methodClass) {
            LOGE("Failed to find Method class");
            env->DeleteLocalRef(methodObject);
            return 0;
        }
        
        jfieldID artMethodField = env->GetFieldID(methodClass, "artMethod", "J");
        if (!artMethodField) {
            env->ExceptionClear();
            LOGD("Trying to get ArtMethod through reflection");
            
            jclass classClass = env->FindClass("java/lang/Class");
            jmethodID getDeclaredFieldMethod = env->GetMethodID(classClass, "getDeclaredField", 
                                                                 "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
            
            jstring fieldName = env->NewStringUTF("artMethod");
            jobject fieldObject = env->CallObjectMethod(methodClass, getDeclaredFieldMethod, fieldName);
            env->DeleteLocalRef(fieldName);
            
            if (!fieldObject) {
                LOGE("Failed to get artMethod field through reflection");
                env->DeleteLocalRef(methodObject);
                env->DeleteLocalRef(methodClass);
                return 0;
            }
            
            // Make field accessible
            jclass fieldClass = env->FindClass("java/lang/reflect/Field");
            jmethodID setAccessibleMethod = env->GetMethodID(fieldClass, "setAccessible", "(Z)V");
            env->CallVoidMethod(fieldObject, setAccessibleMethod, JNI_TRUE);
            
            // Get the field ID
            artMethodField = env->FromReflectedField(fieldObject);
            env->DeleteLocalRef(fieldObject);
            env->DeleteLocalRef(fieldClass);
            
            if (!artMethodField) {
                LOGE("Failed to convert reflected field to field ID");
                env->DeleteLocalRef(methodObject);
                env->DeleteLocalRef(methodClass);
                return 0;
            }
        }
        
        jlong artMethodPtr = env->GetLongField(methodObject, artMethodField);
        
        env->DeleteLocalRef(methodObject);
        env->DeleteLocalRef(methodClass);
        
        LOGI("Converted index jmethodID to ArtMethod pointer: 0x%lx", (uintptr_t)artMethodPtr);
        return (uintptr_t)artMethodPtr;
    }
    
    return reinterpret_cast<uintptr_t>(mid);
}

static void original_nativeMark(JNIEnv* env, jclass clazz) {
    LOGD("original_nativeMark called");
}

typedef jint (*OriginalGetCallingUid)();

static jint hooked_getCallingUid() {
    jint originalUid = 0;
    if (g_original_getCallingUid) {
        auto origFunc = reinterpret_cast<OriginalGetCallingUid>(g_original_getCallingUid);
        originalUid = origFunc();
        LOGI("Original getCallingUid returned: %d", originalUid);
    }
    
    JNIEnv* env = GetJNIEnv();
    if (!env) {
        LOGE("Failed to get JNIEnv!");
        return originalUid;
    }
    
    if (g_mainActivityClass && g_onGetCallingUidMethod) {
        jint result = env->CallStaticIntMethod(g_mainActivityClass,
                                                g_onGetCallingUidMethod, 
                                                originalUid);
        
        // Clear any pending exceptions
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        
        LOGI("Java onGetCallingUid() returned: %d", result);
        LOGE("=== Exiting Hooked getCallingUid ===");
        return result;
    }
    
    LOGE("=== Exiting Hooked getCallingUid ===");
    return originalUid;
}

static bool measureNativeOffset(JNIEnv* env, jclass mainActivity) {
    LOGD("Measuring native function offset in ArtMethod structure...");
    LOGD("Android API level: %d", g_api_level);
    
    // Register original function to get a reference
    JNINativeMethod method = {
        const_cast<char*>("nativeMark"),
        const_cast<char*>("()V"),
        reinterpret_cast<void*>(original_nativeMark)
    };
    
    if (env->RegisterNatives(mainActivity, &method, 1) < 0) {
        LOGE("Failed to register native method");
        return false;
    }
    
    // Get the jmethodID
    jmethodID mid = env->GetStaticMethodID(mainActivity, "nativeMark", "()V");
    if (!mid) {
        LOGE("Failed to get method ID");
        return false;
    }
    
    // Convert jmethodID to actual ArtMethod pointer (handles Android R+ index-based IDs)
    uintptr_t artMethodPtr = getArtMethodPointer(env, mid, mainActivity, true);
    if (artMethodPtr == 0) {
        LOGE("Failed to get ArtMethod pointer");
        return false;
    }
    
    // Search for the function pointer in the ArtMethod structure
    auto start = artMethodPtr;
    auto target = reinterpret_cast<uintptr_t>(original_nativeMark);
    
    LOGD("Searching for function pointer:");
    LOGD("  ArtMethod address: 0x%lx", start);
    LOGD("  Target function:   0x%lx", target);
    
    // Search up to 200 bytes (covers ArtMethod structure in most cases)
    for (int offset = 0; offset < 200; offset += 8) {  // 8-byte aligned for 64-bit
        uintptr_t* ptr = reinterpret_cast<uintptr_t*>(start + offset);
        if (*ptr == target) {
            g_native_function_offset = offset;
            LOGI("Found native function pointer at offset: %d", offset);
            
            // Log the surrounding structure for debugging
            LOGD("ArtMethod structure around offset %d:", offset);
            for (int i = -16; i <= 16; i += 8) {
                uintptr_t* p = reinterpret_cast<uintptr_t*>(start + offset + i);
                LOGD("  [%+3d] 0x%016lx", i, *p);
            }
            
            return true;
        }
    }
    
    LOGE("Failed to find native function pointer in ArtMethod");
    return false;
}

static bool artSetJNIFunction(JNIEnv* env, jmethodID method, jclass clazz, void* new_func, void** old_func) {
    if (g_native_function_offset < 0) {
        LOGE("Native offset not measured");
        return false;
    }
    
    // Get actual ArtMethod pointer (handles Android R+ index-based jmethodID)
    uintptr_t method_addr = getArtMethodPointer(env, method, clazz, true);
    if (method_addr == 0) {
        LOGE("Failed to get ArtMethod pointer for hooking");
        return false;
    }
    
    auto func_ptr = reinterpret_cast<void**>(method_addr + g_native_function_offset);
    
    // Save original
    if (old_func) {
        *old_func = *func_ptr;
    }
    
    LOGI("Hooking JNI function:");
    LOGI("  ArtMethod address: 0x%lx", method_addr);
    LOGI("  Offset:            %d", g_native_function_offset);
    LOGI("  Original function: 0x%lx", reinterpret_cast<uintptr_t>(*func_ptr));
    LOGI("  New function:      0x%lx", reinterpret_cast<uintptr_t>(new_func));

    *func_ptr = new_func;
    
    LOGI("✓ JNI function pointer replaced");
    return true;
}

static bool setupJavaCallback(JNIEnv* env) {
    LOGI("Setting up Java callback for hook...");
    
    jclass mainActivityClass = env->FindClass("com/test/gccrash/MainActivity");
    if (!mainActivityClass) {
        LOGE("Failed to find MainActivity class");
        return false;
    }
    
    // Create global reference to keep the class alive
    g_mainActivityClass = reinterpret_cast<jclass>(env->NewGlobalRef(mainActivityClass));
    env->DeleteLocalRef(mainActivityClass);
    
    if (!g_mainActivityClass) {
        LOGE("Failed to create global reference for MainActivity");
        return false;
    }
    
    // Get the callback method
    g_onGetCallingUidMethod = env->GetStaticMethodID(g_mainActivityClass, "onGetCallingUid", "(I)I");
    if (!g_onGetCallingUidMethod) {
        LOGE("Failed to find onGetCallingUid method");
        env->ExceptionClear();
        return false;
    }
    
    LOGI("✓ Java callback set up successfully");
    LOGI("  Class: com.test.gccrash.MainActivity");
    LOGI("  Method: onGetCallingUid(I)I");
    return true;
}

static bool hookFrameworkMethods(JNIEnv* env) {
    LOGI("Hooking Android framework methods...");
    LOGI("");
    
    if (!setupJavaCallback(env)) {
        LOGE("Failed to set up Java callback - crash may not occur");
    }
    
    LOGI("\n--- Hooking Binder.getCallingUid ---");
    LOGI("");
    
    jclass binderClass = env->FindClass("android/os/Binder");
    if (binderClass) {
        jmethodID getCallingUid = env->GetStaticMethodID(binderClass, "getCallingUid", "()I");
        if (getCallingUid) {
            g_hooked_getCallingUid = getCallingUid;
            
            if (artSetJNIFunction(env, getCallingUid, binderClass,
                                 reinterpret_cast<void*>(hooked_getCallingUid),
                                 &g_original_getCallingUid)) {
                LOGI("✓ Successfully hooked Binder.getCallingUid");
            } else {
                LOGE("✗ Failed to hook Binder.getCallingUid");
            }
        } else {
            LOGE("✗ Failed to find Binder.getCallingUid method");
            env->ExceptionClear();
        }
        env->DeleteLocalRef(binderClass);
    } else {
        LOGE("✗ Failed to find Binder class");
    }
    
    return g_hooked_getCallingUid != nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_test_gccrash_MainActivity_initHook(JNIEnv* env, jobject thiz) {
    LOGI("========================================");
    LOGI("Initializing GC Crash Test Hook");
    LOGI("========================================");
    
    if (g_hook_active) {
        LOGW("Hook already active");
        return;
    }
    
    // Get JavaVM
    if (env->GetJavaVM(&g_jvm) != JNI_OK) {
        LOGE("Failed to get JavaVM");
        return;
    }
    
    // Get Android API level
    jclass versionClass = env->FindClass("android/os/Build$VERSION");
    if (versionClass) {
        jfieldID sdkIntField = env->GetStaticFieldID(versionClass, "SDK_INT", "I");
        if (sdkIntField) {
            g_api_level = env->GetStaticIntField(versionClass, sdkIntField);
            LOGI("Android API level: %d", g_api_level);
        }
        env->DeleteLocalRef(versionClass);
    }
    
    // Get MainActivity class for measuring offset
    jclass mainActivity = env->FindClass("com/test/gccrash/MainActivity");
    if (!mainActivity) {
        LOGE("Failed to find MainActivity class");
        return;
    }
    
    // Measure offset using our test method
    if (!measureNativeOffset(env, mainActivity)) {
        LOGE("Failed to measure native offset");
        env->DeleteLocalRef(mainActivity);
        return;
    }
    
    env->DeleteLocalRef(mainActivity);
    
    // Now hook the actual framework methods
    if (!hookFrameworkMethods(env)) {
        LOGE("Failed to hook any framework methods");
        return;
    }
    
    g_hook_active = true;
    
    LOGI("========================================");
    LOGI("Hook Active - Ready to Trigger Crash");
    LOGI("========================================");
    LOGI("");
    LOGI("Hooked methods:");
    if (g_hooked_getCallingUid) {
        LOGI("  ✓ Binder.getCallingUid");
        LOGI("    -> Will call Java onGetCallingUid()");
    }
    LOGI("");
    LOGI("Java callback:");
    if (g_onGetCallingUidMethod) {
        LOGI("  ✓ MainActivity.onGetCallingUid(I)I");
    }
    LOGI("");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_test_gccrash_MainActivity_isHookActive(JNIEnv* env, jobject thiz) {
    return g_hook_active ? JNI_TRUE : JNI_FALSE;
}

