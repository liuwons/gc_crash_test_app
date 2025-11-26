cmake_minimum_required(VERSION 3.22.1)

project("gccrashtest")

add_library(gccrashtest SHARED
    gc_crash_test.cpp
)

target_link_libraries(gccrashtest
    android
    log
)

# Enable C++17 and disable RTTI
set_target_properties(gccrashtest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

