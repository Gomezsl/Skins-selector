LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# resultado: libSkinSelector64.so
LOCAL_MODULE := SkinSelector64
LOCAL_SRC_FILES := SkinSelector.cpp
# AndroidModLoader = repo RusJJ/AndroidModLoader (contiene mod/amlmod.h)
LOCAL_C_INCLUDES := $(LOCAL_PATH)/AndroidModLoader $(LOCAL_PATH)
LOCAL_CPPFLAGS := -std=c++17 -O2 -fno-exceptions -fno-rtti
LOCAL_LDLIBS := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
