package com.test.gccrash;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;
import android.widget.ScrollView;
import android.widget.LinearLayout;
import android.view.ViewGroup;
import java.lang.reflect.Method;

public class MainActivity extends Activity {
    private static final String TAG = "GCCrashTest";
    private TextView logView;
    private StringBuilder logBuffer = new StringBuilder();

    static {
        System.loadLibrary("gccrashtest");
    }

    private native void initHook();
    private native boolean isHookActive();
    
    private static native void nativeMark();
    
    public static int onGetCallingUid(int originalUid) {
        Log.i(TAG, "=== onGetCallingUid called from native hook ===");
        Log.i(TAG, "Original UID: " + originalUid);

        try {
            Thread.sleep(200);
        } catch (InterruptedException e) {
            Log.e(TAG, "Sleep interrupted", e);
        }
        
        // Allocate some objects to encourage GC
        for (int i = 0; i < 100; i++) {
            byte[] garbage = new byte[1024 * 100]; // 100KB allocations
        }
        
        Log.i(TAG, "=== onGetCallingUid returning ===");
        return originalUid;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(32, 32, 32, 32);
        
        TextView title = new TextView(this);
        title.setText("GC Crash Reproduction Test\n\n");
        title.setTextSize(16);
        title.setPadding(0, 0, 0, 32);
        layout.addView(title);
        
        Button hookButton = new Button(this);
        hookButton.setText("1. Initialize Hook (artSetJNIFunction)");
        hookButton.setOnClickListener(v -> initializeHook());
        layout.addView(hookButton);
        
        Button triggerButton = new Button(this);
        triggerButton.setText("2. Trigger Crash (Call Framework Methods)");
        triggerButton.setOnClickListener(v -> triggerCrash());
        layout.addView(triggerButton);
        
        ScrollView scrollView = new ScrollView(this);
        logView = new TextView(this);
        logView.setTextSize(12);
        logView.setPadding(16, 16, 16, 16);
        logView.setBackgroundColor(0xFF1E1E1E);
        logView.setTextColor(0xFFCCCCCC);
        scrollView.addView(logView);
        
        LinearLayout.LayoutParams scrollParams = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1.0f
        );
        layout.addView(scrollView, scrollParams);
        
        setContentView(layout);
        
        log("App started.");
        log("");
    }

    private void initializeHook() {
        log("=== Initializing Hook ===");
        try {
            // Register our native marker method
            Method markMethod = MainActivity.class.getDeclaredMethod("nativeMark");
            
            log("Calling initHook() to hook Binder.getCallingUid()...");
            initHook();
            
            if (isHookActive()) {
                log("✓ Hook initialized successfully!");
            } else {
                log("✗ Hook initialization failed");
            }
        } catch (Exception e) {
            log("✗ Error: " + e.getMessage());
            Log.e(TAG, "Hook init error", e);
        }
        log("");
    }

    private void triggerCrash() {
        log("=== Triggering Crash ===");
        log("");
        
        final boolean[] running = {true};
        
        // Thread 1: Keep calling the hooked method
        Thread callerThread = new Thread(() -> {
            int count = 0;
            Log.i(TAG, "Caller thread: Starting continuous calls");
            while (running[0] && count < 1000000) {
                try {
                    count++;
                    int uid = android.os.Binder.getCallingUid();  // Sleeps 200ms inside hook
                    if (count % 10 == 0) {
                        Log.i(TAG, "Caller: completed " + count + " calls");
                    }
                } catch (Throwable t) {
                    Log.e(TAG, "Caller error: " + t.getMessage());
                    break;
                }
            }
            Log.i(TAG, "Caller thread: completed " + count + " total calls");
        }, "Caller-Thread");
        
        // Thread 2: Allocate memory and trigger GC aggressively
        Thread gcThread = new Thread(() -> {
            try {
                Thread.sleep(300);  // Let caller get going first
            } catch (InterruptedException e) {}
            
            Log.i(TAG, "GC thread: Starting aggressive GC triggers");
            
            // Create memory pressure
            java.util.ArrayList<byte[]> garbage = new java.util.ArrayList<>();
            for (int i = 0; i < 500000 && running[0]; i++) {
                try {
                    // Allocate 5MB chunks
                    garbage.add(new byte[5 * 1024 * 1024]);
                    
                    // Trigger GC
                    System.gc();
                    Runtime.getRuntime().gc();
                    
                    if (i % 10 == 0) {
                        Log.i(TAG, "GC thread: allocated " + (i * 5) + "MB, GC triggered");
                    }
                    
                    Thread.sleep(100);
                } catch (OutOfMemoryError e) {
                    Log.i(TAG, "GC thread: OOM - clearing garbage");
                    garbage.clear();
                    System.gc();
                } catch (InterruptedException e) {
                    break;
                }
            }
            
            garbage.clear();
            Log.i(TAG, "GC thread: completed");
        }, "GC-Thread");
        
        log("Starting caller thread (continuous getCallingUid calls)...");
        callerThread.start();
        
        try { Thread.sleep(500); } catch (InterruptedException e) {}
        
        log("Starting GC thread (memory allocation + GC triggers)...");
        gcThread.start();
        
        log("");
        
        // Monitor threads
        new Thread(() -> {
            try {
                callerThread.join(20000000);
                gcThread.join(200000000);
                running[0] = false;
                
                runOnUiThread(() -> {
                    log("");
                    log("Test completed without crash.");
                    log("");
                });
            } catch (InterruptedException e) {
            }
        }).start();
    }

    private void log(String message) {
        logBuffer.append(message).append("\n");
        runOnUiThread(() -> logView.setText(logBuffer.toString()));
        Log.i(TAG, message);
    }
}

