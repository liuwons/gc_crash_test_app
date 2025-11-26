#!/bin/bash

# GC Crash Test - Quick Test Script
# This script builds, installs, and runs the crash test app

set -e

echo "========================================"
echo "GC Crash Reproduction Test"
echo "========================================"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if device is connected
if ! adb devices | grep -q "device$"; then
    echo -e "${RED}Error: No Android device connected${NC}"
    echo "Please connect a device or start an emulator"
    exit 1
fi

echo -e "${GREEN}✓ Device connected${NC}"
DEVICE=$(adb devices | grep "device$" | head -1 | awk '{print $1}')
echo "  Device: $DEVICE"
echo ""

# Build the app
echo "Building app..."
./gradlew assembleDebug
echo -e "${GREEN}✓ Build successful${NC}"
echo ""

# Install the app
echo "Installing app..."
adb install -r app/build/outputs/apk/debug/app-debug.apk
echo -e "${GREEN}✓ App installed${NC}"
echo ""

# Launch the app
echo "Launching app..."
adb shell am start -n com.test.gccrash/.MainActivity
echo -e "${GREEN}✓ App launched${NC}"
echo ""

# Wait a bit for app to start
sleep 2

echo "========================================"
echo "Manual Test Steps:"
echo "========================================"
echo "1. Tap 'Initialize Hook' button in the app"
echo "2. Tap 'Trigger Crash' button"
echo "3. Watch logcat for crash or try multi-thread test"
echo ""
echo "Expected crash signature:"
echo "  SIGSEGV in art::OatQuickMethodHeader::GetFrameInfo()"
echo ""
echo "========================================"
echo "Monitoring logcat..."
echo "Press Ctrl+C to stop"
echo "========================================"
echo ""

# Monitor logcat
adb logcat -c  # Clear logcat first
adb logcat -s GCCrashTest-Native:* GCCrashTest:* AndroidRuntime:E DEBUG:I | grep --color=auto -E "(GCCrashTest|SIGSEGV|FATAL|GetFrameInfo|VisitRoots)"

