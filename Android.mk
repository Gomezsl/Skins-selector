LOCAL_PATH := $(call my-dir)

# ---- nucleo: libVestidorSA64.so ----
include $(CLEAR_VARS)
LOCAL_MODULE := VestidorSA64
LOCAL_SRC_FILES := SkinSelector.cpp AndroidModLoader/mod/config.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/AndroidModLoader $(LOCAL_PATH)
LOCAL_CPPFLAGS := -std=c++17 -O2 -fno-exceptions -fno-rtti
LOCAL_LDLIBS := -llog -ldl
include $(BUILD_SHARED_LIBRARY)

# ---- Dear ImGui 1.89.7 (estatica, simbolos ocultos para no chocar con el ImGui de ZyZ) ----
include $(CLEAR_VARS)
LOCAL_MODULE := imgui_es2
LOCAL_SRC_FILES := imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_demo.cpp imgui/backends/imgui_impl_opengl3.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/imgui $(LOCAL_PATH)/imgui/backends
LOCAL_CPPFLAGS := -std=c++17 -O2 -fno-exceptions -fno-rtti -fvisibility=hidden -DIMGUI_IMPL_OPENGL_ES2
include $(BUILD_STATIC_LIBRARY)

# ---- menu: libVestidorSAMenu64.so ----
include $(CLEAR_VARS)
LOCAL_MODULE := VestidorSAMenu64
LOCAL_SRC_FILES := SkinSelectorMenu.cpp AndroidModLoader/mod/config.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/AndroidModLoader $(LOCAL_PATH)/imgui $(LOCAL_PATH)/imgui/backends
LOCAL_CPPFLAGS := -std=c++17 -O2 -fno-exceptions -fno-rtti
LOCAL_STATIC_LIBRARIES := imgui_es2
LOCAL_LDLIBS := -llog -ldl -lEGL -lGLESv2
LOCAL_LDFLAGS := -Wl,--exclude-libs,ALL
include $(BUILD_SHARED_LIBRARY)
