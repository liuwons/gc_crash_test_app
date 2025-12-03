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
static bool g_hook_active = false;
static JavaVM* g_jvm = nullptr;
static int g_api_level = 0;

// ArtMethod offsets and trampoline
struct ArtMethodConfig {
    size_t size = 0;
    size_t access_flags_offset = 0;
    size_t data_offset = 0;
    size_t entry_point_from_quick_compiled_code_offset = 0;
    void* generic_jni_trampoline = nullptr;
    bool initialized = false;
} g_art_config;

// Constants
constexpr uint32_t kAccCriticalNative = 0x00100000;
constexpr uint32_t kAccFastNative = 0x00080000;
constexpr uint32_t kAccNative = 0x0100;

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

constexpr uint32_t kAccPublic = 0x0001;
constexpr uint32_t kAccStatic = 0x0008;

static bool initArtConfig(JNIEnv* env) {
    if (g_art_config.initialized) return true;

    LOGI("Initializing ArtMethod configuration (Robust Scan)...");

    // 1. Register a dummy native method to use as a template
    jclass mainActivity = env->FindClass("com/test/gccrash/MainActivity");
    if (!mainActivity) {
        LOGE("Failed to find MainActivity class");
        return false;
    }

    JNINativeMethod method = {
            const_cast<char*>("nativeMark"),
            const_cast<char*>("()V"),
            reinterpret_cast<void*>(original_nativeMark)
    };

    if (env->RegisterNatives(mainActivity, &method, 1) < 0) {
        LOGE("Failed to register nativeMark");
        return false;
    }

    jmethodID mid = env->GetStaticMethodID(mainActivity, "nativeMark", "()V");
    uintptr_t artMethod = getArtMethodPointer(env, mid, mainActivity, true);

    if (artMethod == 0) {
        LOGE("Failed to get ArtMethod pointer for nativeMark");
        return false;
    }

    // 2. Scan for data_offset (pointer to original_nativeMark)
    // We search a reasonable range (e.g. 0 to 128 bytes)
    bool foundData = false;
    uintptr_t targetFunc = reinterpret_cast<uintptr_t>(original_nativeMark);

    for (size_t i = 0; i < 128; i += sizeof(void*)) {
        uintptr_t val = *reinterpret_cast<uintptr_t*>(artMethod + i);
        if (val == targetFunc) {
            g_art_config.data_offset = i;
            foundData = true;
            LOGI("Found data_offset: %zu", i);
            break;
        }
    }

    if (!foundData) {
        LOGE("Failed to find data_offset in ArtMethod");
        return false;
    }

    // 3. Scan for access_flags_offset
    // nativeMark is static public native:
    // kAccPublic(0x1) | kAccStatic(0x8) | kAccNative(0x100) = 0x109
    // But runtime flags might be set. We check for essential flags (STATIC | NATIVE).
    uint32_t requiredFlags = kAccStatic | kAccNative;
    uint32_t mask = 0xFFFF; // Check low 16 bits (Java access flags)
    bool foundFlags = false;

    // We search up to data_offset because flags are usually before data
    for (size_t i = 0; i < g_art_config.data_offset; i += 4) {
        uint32_t val = *reinterpret_cast<uint32_t*>(artMethod + i);

        // Log potential candidates for debugging
        if ((val & mask & requiredFlags) == requiredFlags) {
            LOGI("Potential access_flags candidate at offset %zu: 0x%x", i, val);

            // Prefer the first one that matches expected pattern or if it exactly matches expected public flags
            if ((val & mask) == (kAccPublic | kAccStatic | kAccNative)) {
                g_art_config.access_flags_offset = i;
                foundFlags = true;
                LOGI("Found exact access_flags_offset: %zu", i);
                break;
            }

            // If we haven't found a "perfect" match yet, take this one as tentative
            if (!foundFlags) {
                g_art_config.access_flags_offset = i;
                foundFlags = true;
                LOGI("Found tentative access_flags_offset: %zu", i);
            }
        }
    }

    if (!foundFlags) {
        // Fallback: assume offset 4 if not found (very common)
        LOGW("Could not find access_flags with mask, defaulting to 4");

        // Log value at offset 4 to see what it was
        uint32_t val4 = *reinterpret_cast<uint32_t*>(artMethod + 4);
        LOGW("Value at offset 4: 0x%x", val4);

        g_art_config.access_flags_offset = 4;
    } else {
        LOGI("Confirmed access_flags_offset: %zu", g_art_config.access_flags_offset);
    }

    // 4. Derive entry_point_from_quick_compiled_code_offset
    // Convention: it follows data_offset
    g_art_config.entry_point_from_quick_compiled_code_offset = g_art_config.data_offset + sizeof(void*);
    LOGI("Derived entry_point_from_quick_compiled_code_offset: %zu", g_art_config.entry_point_from_quick_compiled_code_offset);

    // 5. Get Generic JNI Trampoline
    // Since nativeMark is not compiled (registered dynamically), its entry point IS the generic JNI trampoline
    g_art_config.generic_jni_trampoline = *reinterpret_cast<void**>(artMethod + g_art_config.entry_point_from_quick_compiled_code_offset);
    LOGI("Generic JNI Trampoline: %p", g_art_config.generic_jni_trampoline);

    g_art_config.initialized = true;
    return true;
}

typedef jint (*OriginalGetCallingUid)();

static jint hooked_getCallingUid(JNIEnv* env, jclass clazz) {
    jint originalUid = 0;
    if (g_original_getCallingUid) {
        // Call original as a simple function (it was CriticalNative)
        auto origFunc = reinterpret_cast<OriginalGetCallingUid>(g_original_getCallingUid);
        originalUid = origFunc();
        LOGI("Original getCallingUid returned: %d", originalUid);
    } else {
        LOGE("Original getCallingUid is null!");
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

static bool artHookMethod(JNIEnv* env, jmethodID method, jclass clazz, void* new_func, void** old_func, bool isCritical) {
    if (!g_art_config.initialized) {
        LOGE("ArtConfig not initialized");
        return false;
    }

    // Get actual ArtMethod pointer
    uintptr_t method_addr = getArtMethodPointer(env, method, clazz, true);
    if (method_addr == 0) {
        LOGE("Failed to get ArtMethod pointer for hooking");
        return false;
    }

    LOGI("Hooking method at 0x%lx", method_addr);

    // 1. Update data_ (JNI entry point)
    void** data_ptr = reinterpret_cast<void**>(method_addr + g_art_config.data_offset);
    if (old_func) {
        *old_func = *data_ptr;
    }
    *data_ptr = new_func;
    LOGI("  Updated data_: %p -> %p", (old_func ? *old_func : nullptr), new_func);

    // 2. Update entry_point_from_quick_compiled_code_ to Generic JNI Trampoline
    // This ensures ART treats it as a non-compiled native method and goes through the generic JNI stub
    // which handles the JNIEnv setup and state transition correctly.
    void** entry_point_ptr = reinterpret_cast<void**>(method_addr + g_art_config.entry_point_from_quick_compiled_code_offset);
    *entry_point_ptr = g_art_config.generic_jni_trampoline;
    LOGI("  Updated entry_point: %p", g_art_config.generic_jni_trampoline);

    // 3. Update Access Flags
    // Remove CriticalNative/FastNative flags so ART treats it as a regular native method.
    // This is crucial for the Generic JNI Trampoline to work correctly and for GC to walk the stack.
    uint32_t* access_flags_ptr = reinterpret_cast<uint32_t*>(method_addr + g_art_config.access_flags_offset);
    uint32_t old_flags = *access_flags_ptr;

    // Check for CriticalNative flags and log them
    if (isCritical) {
        LOGI("  Original flags before stripping: 0x%x", old_flags);
        if (old_flags & kAccCriticalNative) LOGI("    Has kAccCriticalNative (0x%x)", kAccCriticalNative);
        if (old_flags & 0x00200000) LOGI("    Has 0x00200000");
    }

    // Strip both standard CriticalNative (0x00200000) and user-provided/alternative (0x00100000) just to be safe
    // Also strip FastNative
    uint32_t new_flags = old_flags & ~(kAccCriticalNative | 0x00200000 | kAccFastNative);

    // Ensure kAccNative is set (it should be already)
    new_flags |= kAccNative;

    *access_flags_ptr = new_flags;
    LOGI("  Updated access_flags: 0x%x -> 0x%x", old_flags, new_flags);

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

            // This IS a CriticalNative method, so pass true
            if (artHookMethod(env, getCallingUid, binderClass,
                              reinterpret_cast<void*>(hooked_getCallingUid),
                              &g_original_getCallingUid, true)) {
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

    // Initialize offsets and config
    if (!initArtConfig(env)) {
        LOGE("Failed to initialize ART config");
        return;
    }

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

