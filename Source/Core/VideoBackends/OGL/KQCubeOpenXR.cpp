// Copyright 2026 KQCube contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/KQCubeOpenXR.h"

#ifdef ANDROID

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Common/ScopeGuard.h"
#include "Common/VR/OpenXRInputState.h"
#include "InputCommon/ControllerInterface/Touch/InputOverrider.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
constexpr char LOG_TAG[] = "KQCube-OpenXR";
constexpr float SCREEN_DISTANCE_METERS = 2.0f;
constexpr float SCREEN_WIDTH_METERS = 2.4f;

float GetVirtualScreenAspect(float source_aspect)
{
  return std::isfinite(source_aspect) && source_aspect > 0.0f ?
             std::clamp(source_aspect, 0.75f, 2.5f) :
             4.0f / 3.0f;
}

XrPosef GetVirtualScreenPose()
{
  XrPosef pose{};
  pose.orientation.w = 1.0f;
  pose.position.z = -SCREEN_DISTANCE_METERS;
  return pose;
}

XrExtent2Df GetVirtualScreenSize(float source_aspect)
{
  return {SCREEN_WIDTH_METERS, SCREEN_WIDTH_METERS / GetVirtualScreenAspect(source_aspect)};
}

Common::VR::OpenXRScreenHit ComputeVirtualScreenHit(const Common::VR::OpenXRPoseState& aim,
                                                    float source_aspect)
{
  Common::VR::OpenXRScreenHit hit;
  if (!aim.valid)
    return hit;

  // The cinema quad has identity orientation in LOCAL space. OpenXR aim forward is the
  // controller's local -Z axis rotated by its orientation.
  const float qx = aim.orientation[0];
  const float qy = aim.orientation[1];
  const float qz = aim.orientation[2];
  const float qw = aim.orientation[3];
  const float dx = -2.0f * (qx * qz + qw * qy);
  const float dy = -2.0f * (qy * qz - qw * qx);
  const float dz = -(1.0f - 2.0f * (qx * qx + qy * qy));
  if (std::abs(dz) < 1e-6f)
    return hit;

  const XrPosef screen_pose = GetVirtualScreenPose();
  const float distance_to_plane = aim.position[2] - screen_pose.position.z;
  const float t = -distance_to_plane / dz;
  if (t <= 0.0f)
    return hit;

  const XrExtent2Df screen_size = GetVirtualScreenSize(source_aspect);
  hit.valid = true;
  hit.u = (aim.position[0] + t * dx - screen_pose.position.x) / (screen_size.width * 0.5f);
  hit.v = (aim.position[1] + t * dy - screen_pose.position.y) / (screen_size.height * 0.5f);
  // Use perpendicular plane distance, not ray length: aiming toward a corner should not make the
  // emulated sensor bar appear farther away.
  hit.distance_m = distance_to_plane;
  return hit;
}

#define KQXR_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define KQXR_LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define KQXR_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

std::mutex s_bridge_mutex;
JavaVM* s_java_vm = nullptr;
jobject s_android_activity = nullptr;
std::atomic<bool> s_requested{false};
std::atomic<bool> s_presentation_active{false};
std::atomic<bool> s_exit_requested{false};
std::atomic<bool> s_presentation_failed{false};

void SetPresentationActive(bool active)
{
  s_presentation_active.store(active, std::memory_order_release);
}

void RequestExit()
{
  s_exit_requested.store(true, std::memory_order_release);
}

bool AttachJavaThread(JavaVM* vm, JNIEnv** env, bool* attached)
{
  *attached = false;
  const jint get_env_result = vm->GetEnv(reinterpret_cast<void**>(env), JNI_VERSION_1_6);
  if (get_env_result == JNI_OK)
    return true;
  if (get_env_result != JNI_EDETACHED || vm->AttachCurrentThread(env, nullptr) != JNI_OK)
    return false;
  *attached = true;
  return true;
}

bool CopyAndroidContext(JavaVM** vm, jobject* activity)
{
  std::lock_guard guard(s_bridge_mutex);
  if (s_java_vm == nullptr || s_android_activity == nullptr || !s_requested.load())
    return false;

  JNIEnv* env = nullptr;
  bool attached = false;
  if (!AttachJavaThread(s_java_vm, &env, &attached))
    return false;

  *vm = s_java_vm;
  *activity = env->NewGlobalRef(s_android_activity);
  if (attached)
    s_java_vm->DetachCurrentThread();
  return *activity != nullptr;
}

void DeleteGlobalRef(JavaVM* vm, jobject object)
{
  if (vm == nullptr || object == nullptr)
    return;

  JNIEnv* env = nullptr;
  bool attached = false;
  if (!AttachJavaThread(vm, &env, &attached))
  {
    KQXR_LOGW("Unable to attach render thread to release the Quest activity reference");
    return;
  }
  env->DeleteGlobalRef(object);
  if (attached)
    vm->DetachCurrentThread();
}

const char* SessionStateName(XrSessionState state)
{
  switch (state)
  {
  case XR_SESSION_STATE_UNKNOWN:
    return "UNKNOWN";
  case XR_SESSION_STATE_IDLE:
    return "IDLE";
  case XR_SESSION_STATE_READY:
    return "READY";
  case XR_SESSION_STATE_SYNCHRONIZED:
    return "SYNCHRONIZED";
  case XR_SESSION_STATE_VISIBLE:
    return "VISIBLE";
  case XR_SESSION_STATE_FOCUSED:
    return "FOCUSED";
  case XR_SESSION_STATE_STOPPING:
    return "STOPPING";
  case XR_SESSION_STATE_LOSS_PENDING:
    return "LOSS_PENDING";
  case XR_SESSION_STATE_EXITING:
    return "EXITING";
  default:
    return "INVALID";
  }
}

bool SupportsSurfacelessContext(EGLDisplay display)
{
  const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
  return extensions != nullptr && std::strstr(extensions, "EGL_KHR_surfaceless_context") != nullptr;
}

struct Swapchain
{
  XrSwapchain handle = XR_NULL_HANDLE;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<XrSwapchainImageOpenGLESKHR> images;
  std::vector<std::unique_ptr<OGLFramebuffer>> framebuffers;
};

struct ControllerActions
{
  XrActionSet action_set = XR_NULL_HANDLE;
  std::array<XrPath, 2> hand_paths{XR_NULL_PATH, XR_NULL_PATH};
  XrAction primary_click = XR_NULL_HANDLE;
  XrAction secondary_click = XR_NULL_HANDLE;
  XrAction menu_click = XR_NULL_HANDLE;
  XrAction thumbstick_click = XR_NULL_HANDLE;
  XrAction trigger_value = XR_NULL_HANDLE;
  XrAction squeeze_value = XR_NULL_HANDLE;
  XrAction thumbstick_x = XR_NULL_HANDLE;
  XrAction thumbstick_y = XR_NULL_HANDLE;
  XrAction aim_pose = XR_NULL_HANDLE;
  XrAction grip_pose = XR_NULL_HANDLE;
  XrAction haptic = XR_NULL_HANDLE;
  std::array<XrSpace, 2> aim_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<XrSpace, 2> grip_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<bool, 2> haptics_active{};
};

}  // namespace

namespace KQCubeOpenXRBridge
{
void SetAndroidActivity(JNIEnv* env, jobject activity)
{
  std::lock_guard guard(s_bridge_mutex);
  if (s_android_activity != nullptr)
    env->DeleteGlobalRef(s_android_activity);

  env->GetJavaVM(&s_java_vm);
  s_android_activity = env->NewGlobalRef(activity);
  s_exit_requested.store(false, std::memory_order_release);
  s_presentation_failed.store(false, std::memory_order_release);
  s_requested.store(s_android_activity != nullptr, std::memory_order_release);
  KQXR_LOGI("Quest activity selected; OpenXR will start after Dolphin creates its GLES context");
}

void ClearAndroidActivity(JNIEnv* env, jobject activity)
{
  std::lock_guard guard(s_bridge_mutex);
  if (s_android_activity == nullptr || !env->IsSameObject(s_android_activity, activity))
    return;

  env->DeleteGlobalRef(s_android_activity);
  s_android_activity = nullptr;
  s_requested.store(false, std::memory_order_release);
  RequestExit();
  KQXR_LOGI("Quest activity is leaving; render-thread OpenXR teardown requested");
}

bool IsRequested()
{
  return s_requested.load(std::memory_order_acquire);
}

bool IsPresentationActive()
{
  return s_presentation_active.load(std::memory_order_acquire);
}

bool ConsumeExitRequested()
{
  return s_exit_requested.exchange(false, std::memory_order_acq_rel);
}

bool ConsumePresentationFailure()
{
  return s_presentation_failed.exchange(false, std::memory_order_acq_rel);
}
}  // namespace KQCubeOpenXRBridge

struct KQCubeOpenXR::Impl
{
  ~Impl() { Shutdown(); }

  bool Present(const OGLTexture& source, const MathUtil::Rectangle<int>& source_rect,
               float source_aspect, std::string_view source_type,
               const EyeRenderCallback& render_eye)
  {
    if (failed)
      return false;
    if (!initialized && !Initialize())
      return false;

    PollEvents();
    if (exit_requested)
    {
      RequestExit();
      return true;
    }
    if (!session_running)
    {
      ClearControllerInputStates();
      ResetOpenXRInputState();
      return true;
    }

    return RenderFrame(source, source_rect, source_aspect, source_type, render_eye);
  }

  bool Initialize()
  {
    if (!CopyAndroidContext(&java_vm, &activity))
    {
      KQXR_LOGE("OpenXR was requested without a live Quest activity");
      s_presentation_failed.store(true, std::memory_order_release);
      failed = true;
      return false;
    }

    if (!PrimeAndParkEGL())
    {
      DeleteGlobalRef(java_vm, activity);
      activity = nullptr;
      s_presentation_failed.store(true, std::memory_order_release);
      failed = true;
      return false;
    }

    const char* const renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* const version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    KQXR_LOGI("GLES renderer=%s, version=%s, geometry shaders=%s, stereo mode=%d",
              renderer != nullptr ? renderer : "unknown", version != nullptr ? version : "unknown",
              g_backend_info.bSupportsGeometryShaders ? "supported" : "unsupported",
              static_cast<int>(g_ActiveConfig.stereo_mode));

    // Surface callbacks must stop touching the Android EGL surface from this point onward. If any
    // OpenXR call below fails, FailInitialization restores the exact window binding first.
    owns_presentation = true;
    SetPresentationActive(true);

    if (!CreateInstanceAndSession() || !CreateSwapchains())
    {
      FailInitialization();
      return false;
    }

    if (controller_actions.action_set != XR_NULL_HANDLE)
    {
      ciface::Touch::RegisterGameCubeInputOverrider(0);
      input_registered = true;
    }

    Common::VR::OpenXRInputState::Reset();

    initialized = true;
    KQXR_LOGI("OpenXR bootstrap complete; waiting for the Quest session to become READY");
    return true;
  }

  bool PrimeAndParkEGL()
  {
    egl_display = eglGetCurrentDisplay();
    egl_context = eglGetCurrentContext();
    egl_window_surface = eglGetCurrentSurface(EGL_DRAW);
    if (egl_display == EGL_NO_DISPLAY || egl_context == EGL_NO_CONTEXT ||
        egl_window_surface == EGL_NO_SURFACE)
    {
      KQXR_LOGE("Cannot bootstrap OpenXR: Dolphin has no current EGL window/context");
      return false;
    }

    glFinish();
    if (eglSwapBuffers(egl_display, egl_window_surface) != EGL_TRUE)
    {
      KQXR_LOGE("Android bootstrap swap failed before OpenXR: EGL error 0x%x", eglGetError());
      return false;
    }
    KQXR_LOGI("Presented one Android bootstrap frame before OpenXR session creation");

    EGLint config_id = 0;
    if (eglQueryContext(egl_display, egl_context, EGL_CONFIG_ID, &config_id) != EGL_TRUE)
    {
      KQXR_LOGE("Unable to query Dolphin's EGLConfig: 0x%x", eglGetError());
      return false;
    }

    const EGLint config_attributes[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
    EGLint config_count = 0;
    if (eglChooseConfig(egl_display, config_attributes, &egl_config, 1, &config_count) !=
            EGL_TRUE ||
        config_count != 1 || egl_config == nullptr)
    {
      KQXR_LOGE("Unable to resolve Dolphin's EGLConfig %d: 0x%x", config_id, eglGetError());
      return false;
    }

    EGLint surface_type = 0;
    const bool supports_pbuffer =
        eglGetConfigAttrib(egl_display, egl_config, EGL_SURFACE_TYPE, &surface_type) == EGL_TRUE &&
        (surface_type & EGL_PBUFFER_BIT) != 0;
    if (supports_pbuffer)
    {
      const EGLint pbuffer_attributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
      egl_parking_surface = eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attributes);
      if (egl_parking_surface != EGL_NO_SURFACE &&
          eglMakeCurrent(egl_display, egl_parking_surface, egl_parking_surface, egl_context) ==
              EGL_TRUE)
      {
        KQXR_LOGI("Parked Dolphin's GLES context on a 16x16 pbuffer (EGLConfig %d)", config_id);
        return true;
      }

      const EGLint error = eglGetError();
      if (egl_parking_surface != EGL_NO_SURFACE)
      {
        eglDestroySurface(egl_display, egl_parking_surface);
        egl_parking_surface = EGL_NO_SURFACE;
      }
      eglMakeCurrent(egl_display, egl_window_surface, egl_window_surface, egl_context);
      KQXR_LOGW("Pbuffer parking failed (0x%x); trying surfaceless EGL", error);
    }

    if (SupportsSurfacelessContext(egl_display) &&
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context) == EGL_TRUE)
    {
      parking_is_surfaceless = true;
      KQXR_LOGI("Parked Dolphin's GLES context with EGL_KHR_surfaceless_context");
      return true;
    }

    const EGLint error = eglGetError();
    eglMakeCurrent(egl_display, egl_window_surface, egl_window_surface, egl_context);
    KQXR_LOGE("Unable to park Dolphin's GLES context: EGL error 0x%x", error);
    return false;
  }

  bool CreateInstanceAndSession()
  {
    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    XrResult result =
        xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
    if (XR_FAILED(result) || initialize_loader == nullptr)
    {
      KQXR_LOGE("The Android OpenXR loader does not expose xrInitializeLoaderKHR");
      return false;
    }

    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM = java_vm;
    loader_info.applicationContext = activity;
    if (!XrOk(
            initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info)),
            "xrInitializeLoaderKHR"))
    {
      return false;
    }

    std::vector<const char*> extensions{
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };

    uint32_t extension_count = 0;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr)))
    {
      std::vector<XrExtensionProperties> available_extensions(extension_count,
                                                              {XR_TYPE_EXTENSION_PROPERTIES});
      if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(
              nullptr, extension_count, &extension_count, available_extensions.data())))
      {
#ifdef XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME
        const auto meta_touch_plus = std::find_if(
            available_extensions.begin(), available_extensions.end(), [](const auto& extension) {
              return std::strcmp(extension.extensionName,
                                 XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME) == 0;
            });
        if (meta_touch_plus != available_extensions.end())
        {
          extensions.push_back(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
          meta_touch_plus_enabled = true;
          KQXR_LOGI("Enabling %s for Quest Touch Plus bindings",
                    XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
        }
#endif
      }
    }
    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = java_vm;
    android_info.applicationActivity = activity;

    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    instance_info.next = &android_info;
    std::strncpy(instance_info.applicationInfo.applicationName, "KQCube",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    instance_info.applicationInfo.applicationVersion = 1;
    std::strncpy(instance_info.applicationInfo.engineName, "Dolphin", XR_MAX_ENGINE_NAME_SIZE - 1);
    instance_info.applicationInfo.engineVersion = 2606;
    instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.enabledExtensionNames = extensions.data();
    XrResult create_result = xrCreateInstance(&instance_info, &instance);
    if (create_result == XR_ERROR_API_VERSION_UNSUPPORTED &&
        instance_info.applicationInfo.apiVersion != XR_API_VERSION_1_0)
    {
      KQXR_LOGW("Runtime rejected OpenXR %u.%u; retrying with 1.0",
                XR_VERSION_MAJOR(instance_info.applicationInfo.apiVersion),
                XR_VERSION_MINOR(instance_info.applicationInfo.apiVersion));
      instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
      create_result = xrCreateInstance(&instance_info, &instance);
    }
    if (!XrOk(create_result, "xrCreateInstance"))
      return false;
    // Touch Plus is core in OpenXR 1.1. Some runtimes expose the promoted profile without also
    // advertising the original extension name.
    if (instance_info.applicationInfo.apiVersion >= XR_MAKE_VERSION(1, 1, 0))
      meta_touch_plus_enabled = true;

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XrOk(xrGetInstanceProperties(instance, &properties), "xrGetInstanceProperties"))
    {
      KQXR_LOGI("Runtime: %s %u.%u.%u", properties.runtimeName,
                XR_VERSION_MAJOR(properties.runtimeVersion),
                XR_VERSION_MINOR(properties.runtimeVersion),
                XR_VERSION_PATCH(properties.runtimeVersion));
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrOk(xrGetSystem(instance, &system_info, &system_id), "xrGetSystem"))
      return false;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
    if (!XrOk(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)),
              "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") ||
        get_requirements == nullptr)
    {
      return false;
    }

    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!XrOk(get_requirements(instance, system_id, &requirements),
              "xrGetOpenGLESGraphicsRequirementsKHR"))
    {
      return false;
    }

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const XrVersion current_version = XR_MAKE_VERSION(major, minor, 0);
    if (current_version < requirements.minApiVersionSupported ||
        current_version > requirements.maxApiVersionSupported)
    {
      KQXR_LOGE("Dolphin GLES %d.%d is outside the runtime range %u.%u - %u.%u", major, minor,
                XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                XR_VERSION_MINOR(requirements.minApiVersionSupported),
                XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                XR_VERSION_MINOR(requirements.maxApiVersionSupported));
      return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphics_binding.display = egl_display;
    graphics_binding.config = egl_config;
    graphics_binding.context = egl_context;

    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &graphics_binding;
    session_info.systemId = system_id;
    if (!XrOk(xrCreateSession(instance, &session_info, &session), "xrCreateSession"))
      return false;

    if (!CreateControllerActions())
      KQXR_LOGW("Touch action setup failed; Android and paired controller input remain available");

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!XrOk(xrCreateReferenceSpace(session, &space_info, &local_space),
              "xrCreateReferenceSpace(LOCAL)"))
    {
      return false;
    }

    KQXR_LOGI("Created OpenXR session using Dolphin's GLES %d.%d context", major, minor);
    return true;
  }

  bool CreateAction(XrActionType type, const char* name, const char* localized_name,
                    XrAction* action)
  {
    XrActionCreateInfo create_info{XR_TYPE_ACTION_CREATE_INFO};
    create_info.actionType = type;
    std::strncpy(create_info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(create_info.localizedActionName, localized_name,
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    create_info.countSubactionPaths = static_cast<uint32_t>(controller_actions.hand_paths.size());
    create_info.subactionPaths = controller_actions.hand_paths.data();
    return XrOk(xrCreateAction(controller_actions.action_set, &create_info, action), name);
  }

  bool CreateControllerActions()
  {
    if (!XrOk(xrStringToPath(instance, "/user/hand/left", &controller_actions.hand_paths[0]),
              "left hand subaction path") ||
        !XrOk(xrStringToPath(instance, "/user/hand/right", &controller_actions.hand_paths[1]),
              "right hand subaction path"))
    {
      return false;
    }

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "dolphin_input", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Dolphin Input",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!XrOk(xrCreateActionSet(instance, &set_info, &controller_actions.action_set),
              "xrCreateActionSet(Dolphin input)"))
    {
      return false;
    }

    const bool created = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "primary_click",
                                      "Primary Button", &controller_actions.primary_click) &&
                         CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary_click",
                                      "Secondary Button", &controller_actions.secondary_click) &&
                         CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_click", "Menu Button",
                                      &controller_actions.menu_click) &&
                         CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click",
                                      "Thumbstick Click", &controller_actions.thumbstick_click) &&
                         CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger_value", "Trigger Value",
                                      &controller_actions.trigger_value) &&
                         CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze_value", "Squeeze Value",
                                      &controller_actions.squeeze_value) &&
                         CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "thumbstick_x", "Thumbstick X",
                                      &controller_actions.thumbstick_x) &&
                         CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "thumbstick_y", "Thumbstick Y",
                                      &controller_actions.thumbstick_y) &&
                         CreateAction(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose",
                                      &controller_actions.aim_pose) &&
                         CreateAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose",
                                      &controller_actions.grip_pose) &&
                         CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic Output",
                                      &controller_actions.haptic);
    if (!created)
    {
      DestroyControllerActions();
      return false;
    }

    struct BindingPath
    {
      XrAction action;
      const char* path;
    };

    const auto suggest_bindings = [this](const char* profile,
                                         std::initializer_list<BindingPath> definitions) {
      XrPath profile_path = XR_NULL_PATH;
      if (XR_FAILED(xrStringToPath(instance, profile, &profile_path)))
        return;

      std::vector<XrActionSuggestedBinding> bindings;
      bindings.reserve(definitions.size());
      for (const BindingPath& definition : definitions)
      {
        XrPath binding_path = XR_NULL_PATH;
        if (definition.action != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrStringToPath(instance, definition.path, &binding_path)))
        {
          bindings.push_back({definition.action, binding_path});
        }
      }
      if (bindings.empty())
        return;

      XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
      suggested.interactionProfile = profile_path;
      suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
      suggested.suggestedBindings = bindings.data();
      const XrResult result = xrSuggestInteractionProfileBindings(instance, &suggested);
      if (XR_FAILED(result))
      {
        KQXR_LOGW("Bindings for %s were rejected (%s); trying other Quest profiles", profile,
                  XrResultName(result));
      }
      else
      {
        KQXR_LOGI("Suggested %zu bindings for %s", bindings.size(), profile);
      }
    };

    suggest_bindings("/interaction_profiles/khr/simple_controller",
                     {{controller_actions.primary_click, "/user/hand/left/input/select/click"},
                      {controller_actions.primary_click, "/user/hand/right/input/select/click"},
                      {controller_actions.menu_click, "/user/hand/left/input/menu/click"},
                      {controller_actions.menu_click, "/user/hand/right/input/menu/click"},
                      {controller_actions.aim_pose, "/user/hand/left/input/aim/pose"},
                      {controller_actions.aim_pose, "/user/hand/right/input/aim/pose"},
                      {controller_actions.grip_pose, "/user/hand/left/input/grip/pose"},
                      {controller_actions.grip_pose, "/user/hand/right/input/grip/pose"},
                      {controller_actions.haptic, "/user/hand/left/output/haptic"},
                      {controller_actions.haptic, "/user/hand/right/output/haptic"}});

    const auto suggest_touch_bindings = [&suggest_bindings, this](const char* profile) {
      suggest_bindings(
          profile,
          {{controller_actions.primary_click, "/user/hand/left/input/x/click"},
           {controller_actions.secondary_click, "/user/hand/left/input/y/click"},
           {controller_actions.menu_click, "/user/hand/left/input/menu/click"},
           {controller_actions.thumbstick_click, "/user/hand/left/input/thumbstick/click"},
           {controller_actions.thumbstick_x, "/user/hand/left/input/thumbstick/x"},
           {controller_actions.thumbstick_y, "/user/hand/left/input/thumbstick/y"},
           {controller_actions.trigger_value, "/user/hand/left/input/trigger/value"},
           {controller_actions.squeeze_value, "/user/hand/left/input/squeeze/value"},
           {controller_actions.aim_pose, "/user/hand/left/input/aim/pose"},
           {controller_actions.grip_pose, "/user/hand/left/input/grip/pose"},
           {controller_actions.primary_click, "/user/hand/right/input/a/click"},
           {controller_actions.secondary_click, "/user/hand/right/input/b/click"},
           {controller_actions.menu_click, "/user/hand/right/input/system/click"},
           {controller_actions.thumbstick_click, "/user/hand/right/input/thumbstick/click"},
           {controller_actions.thumbstick_x, "/user/hand/right/input/thumbstick/x"},
           {controller_actions.thumbstick_y, "/user/hand/right/input/thumbstick/y"},
           {controller_actions.trigger_value, "/user/hand/right/input/trigger/value"},
           {controller_actions.squeeze_value, "/user/hand/right/input/squeeze/value"},
           {controller_actions.aim_pose, "/user/hand/right/input/aim/pose"},
           {controller_actions.grip_pose, "/user/hand/right/input/grip/pose"},
           {controller_actions.haptic, "/user/hand/left/output/haptic"},
           {controller_actions.haptic, "/user/hand/right/output/haptic"}});
    };

    suggest_touch_bindings("/interaction_profiles/oculus/touch_controller");
    if (meta_touch_plus_enabled)
    {
      suggest_touch_bindings("/interaction_profiles/meta/touch_plus_controller");
      suggest_touch_bindings("/interaction_profiles/meta/touch_controller_plus");
    }
    suggest_touch_bindings("/interaction_profiles/meta/touch_controller_quest_2");

    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.countActionSets = 1;
    attach_info.actionSets = &controller_actions.action_set;
    if (!XrOk(xrAttachSessionActionSets(session, &attach_info), "xrAttachSessionActionSets"))
    {
      DestroyControllerActions();
      return false;
    }

    const auto create_action_space = [this](XrAction action, XrPath hand_path, XrSpace* space,
                                            const char* operation) {
      XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      space_info.action = action;
      space_info.subactionPath = hand_path;
      space_info.poseInActionSpace.orientation.w = 1.0f;
      const XrResult result = xrCreateActionSpace(session, &space_info, space);
      if (XR_FAILED(result))
        KQXR_LOGW("%s failed: %s; buttons remain available", operation, XrResultName(result));
    };

    for (size_t hand = 0; hand < controller_actions.hand_paths.size(); ++hand)
    {
      create_action_space(controller_actions.aim_pose, controller_actions.hand_paths[hand],
                          &controller_actions.aim_spaces[hand],
                          hand == 0 ? "xrCreateActionSpace(left aim)" :
                                      "xrCreateActionSpace(right aim)");
      create_action_space(controller_actions.grip_pose, controller_actions.hand_paths[hand],
                          &controller_actions.grip_spaces[hand],
                          hand == 0 ? "xrCreateActionSpace(left grip)" :
                                      "xrCreateActionSpace(right grip)");
    }

    KQXR_LOGI("Touch actions attached: GameCube override plus shared OpenXR Wii input device");
    return true;
  }

  bool ReadBooleanAction(XrAction action, XrPath hand_path, bool* any_active) const
  {
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand_path;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session, &get_info, &state)))
      return false;
    *any_active |= state.isActive == XR_TRUE;
    return state.isActive == XR_TRUE && state.currentState == XR_TRUE;
  }

  float ReadFloatAction(XrAction action, XrPath hand_path, bool* any_active) const
  {
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand_path;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session, &get_info, &state)))
      return 0.0f;
    *any_active |= state.isActive == XR_TRUE;
    return state.isActive == XR_TRUE ? state.currentState : 0.0f;
  }

  bool ReadPoseAction(XrAction action, XrPath hand_path, bool* any_active) const
  {
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand_path;
    XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
    if (XR_FAILED(xrGetActionStatePose(session, &get_info, &state)))
      return false;
    *any_active |= state.isActive == XR_TRUE;
    return state.isActive == XR_TRUE;
  }

  void LocateControllerSpace(XrSpace space, XrTime sample_time,
                             Common::VR::OpenXRPoseState* pose_state,
                             Common::VR::OpenXRVelocityState* velocity_state) const
  {
    if (space == XR_NULL_HANDLE || local_space == XR_NULL_HANDLE)
      return;

    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = velocity_state != nullptr ? &velocity : nullptr;
    if (XR_FAILED(xrLocateSpace(space, local_space, sample_time, &location)))
      return;

    constexpr XrSpaceLocationFlags required_flags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose_state->valid = (location.locationFlags & required_flags) == required_flags;
    if (pose_state->valid)
    {
      pose_state->position = {location.pose.position.x, location.pose.position.y,
                              location.pose.position.z};
      pose_state->orientation = {location.pose.orientation.x, location.pose.orientation.y,
                                 location.pose.orientation.z, location.pose.orientation.w};
    }

    if (velocity_state == nullptr)
      return;

    velocity_state->linear_valid =
        (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
    if (velocity_state->linear_valid)
    {
      velocity_state->linear = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                velocity.linearVelocity.z};
    }
    velocity_state->angular_valid =
        (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
    if (velocity_state->angular_valid)
    {
      velocity_state->angular = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                 velocity.angularVelocity.z};
    }
  }

  std::string PathToString(XrPath path) const
  {
    if (path == XR_NULL_PATH)
      return {};

    uint32_t size = 0;
    if (XR_FAILED(xrPathToString(instance, path, 0, &size, nullptr)) || size == 0)
      return {};
    std::string value(size, '\0');
    if (XR_FAILED(xrPathToString(instance, path, size, &size, value.data())))
      return {};
    if (!value.empty() && value.back() == '\0')
      value.pop_back();
    return value;
  }

  std::array<std::string, 2> GetInteractionProfiles()
  {
    std::array<std::string, 2> profiles;
    for (size_t hand = 0; hand < controller_actions.hand_paths.size(); ++hand)
    {
      XrInteractionProfileState profile_state{XR_TYPE_INTERACTION_PROFILE_STATE};
      if (XR_FAILED(xrGetCurrentInteractionProfile(session, controller_actions.hand_paths[hand],
                                                   &profile_state)))
      {
        continue;
      }

      profiles[hand] = PathToString(profile_state.interactionProfile);
      if (profile_state.interactionProfile != logged_interaction_profiles[hand])
      {
        logged_interaction_profiles[hand] = profile_state.interactionProfile;
        KQXR_LOGI("%s hand interaction profile: %s", hand == 0 ? "Left" : "Right",
                  profiles[hand].empty() ? "<none>" : profiles[hand].c_str());
      }
    }
    return profiles;
  }

  static XrVector2f ApplyStickDeadzone(XrVector2f stick)
  {
    constexpr float deadzone = 0.15f;
    const float length = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (length <= deadzone)
      return {};
    const float remapped_length = std::min(1.0f, (length - deadzone) / (1.0f - deadzone));
    const float scale = remapped_length / length;
    return {stick.x * scale, stick.y * scale};
  }

  void SyncControllerInput(XrTime sample_time, float source_aspect)
  {
    if (controller_actions.action_set == XR_NULL_HANDLE)
      return;

    const XrActiveActionSet active_set{controller_actions.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_set;
    const XrResult sync_result = xrSyncActions(session, &sync_info);
    if (sync_result == XR_SESSION_NOT_FOCUSED)
    {
      // Keep the runtime identity across brief system overlays, but publish neutral controls so a
      // button held while the Quest system UI took focus cannot remain stuck in Dolphin.
      ClearControllerInputStates();
      StopHaptics();
      const auto previous_snapshot = Common::VR::OpenXRInputState::GetSnapshot();
      Common::VR::OpenXRInputState::SetControllers({}, true, previous_snapshot.interaction_profiles,
                                                   false, sample_time);
      input_state_published = true;
      return;
    }
    if (XR_FAILED(sync_result))
    {
      ClearControllerInputStates();
      ResetOpenXRInputState();
      return;
    }

    const std::array<std::string, 2> profiles = GetInteractionProfiles();
    constexpr float digital_threshold = 0.45f;
    std::array<Common::VR::OpenXRControllerState, 2> controllers{};
    for (size_t hand = 0; hand < controllers.size(); ++hand)
    {
      const XrPath hand_path = controller_actions.hand_paths[hand];
      auto& controller = controllers[hand];
      bool active = false;
      controller.primary_button =
          ReadBooleanAction(controller_actions.primary_click, hand_path, &active);
      controller.secondary_button =
          ReadBooleanAction(controller_actions.secondary_click, hand_path, &active);
      controller.menu_button = ReadBooleanAction(controller_actions.menu_click, hand_path, &active);
      controller.thumbstick_button =
          ReadBooleanAction(controller_actions.thumbstick_click, hand_path, &active);
      controller.trigger_value = std::clamp(
          ReadFloatAction(controller_actions.trigger_value, hand_path, &active), 0.0f, 1.0f);
      controller.squeeze_value = std::clamp(
          ReadFloatAction(controller_actions.squeeze_value, hand_path, &active), 0.0f, 1.0f);
      controller.thumbstick_x = std::clamp(
          ReadFloatAction(controller_actions.thumbstick_x, hand_path, &active), -1.0f, 1.0f);
      controller.thumbstick_y = std::clamp(
          ReadFloatAction(controller_actions.thumbstick_y, hand_path, &active), -1.0f, 1.0f);
      controller.trigger_button = controller.trigger_value > digital_threshold;
      controller.squeeze_button = controller.squeeze_value > digital_threshold;

      ReadPoseAction(controller_actions.aim_pose, controller_actions.hand_paths[hand], &active);
      ReadPoseAction(controller_actions.grip_pose, controller_actions.hand_paths[hand], &active);
      LocateControllerSpace(controller_actions.aim_spaces[hand], sample_time, &controller.aim_pose,
                            nullptr);
      LocateControllerSpace(controller_actions.grip_spaces[hand], sample_time,
                            &controller.grip_pose, &controller.grip_velocity);
      controller.screen_hit = ComputeVirtualScreenHit(controller.aim_pose, source_aspect);
      controller.connected = active || controller.aim_pose.valid || controller.grip_pose.valid;
    }

    Common::VR::OpenXRInputState::SetControllers(
        controllers, true, profiles, session_state == XR_SESSION_STATE_FOCUSED, sample_time);
    input_state_published = true;
    UpdateHaptics();

    const auto& left = controllers[0];
    const auto& right = controllers[1];

    if ((++input_sync_count % 300) == 1)
    {
      KQXR_LOGI("Touch state session=%s: L connected=%d buttons=%d/%d aim=%d grip=%d linear=%d "
                "angular=%d; R connected=%d buttons=%d/%d aim=%d grip=%d linear=%d angular=%d",
                SessionStateName(session_state), left.connected, left.primary_button,
                left.trigger_button, left.aim_pose.valid, left.grip_pose.valid,
                left.grip_velocity.linear_valid, left.grip_velocity.angular_valid, right.connected,
                right.primary_button, right.trigger_button, right.aim_pose.valid,
                right.grip_pose.valid, right.grip_velocity.linear_valid,
                right.grip_velocity.angular_valid);
    }

    if (!input_registered || (!left.connected && !right.connected))
    {
      ClearControllerInputStates();
      return;
    }

    const XrVector2f gamecube_left_stick =
        ApplyStickDeadzone({left.thumbstick_x, left.thumbstick_y});
    const XrVector2f gamecube_right_stick =
        ApplyStickDeadzone({right.thumbstick_x, right.thumbstick_y});
    using ciface::Touch::ControlID;
    ciface::Touch::SetControlState(0, ControlID::GCPAD_A_BUTTON, right.primary_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_B_BUTTON, right.secondary_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_X_BUTTON, left.primary_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_Y_BUTTON, left.secondary_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_Z_BUTTON, right.squeeze_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_START_BUTTON,
                                   left.menu_button || left.squeeze_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_L_ANALOG, left.trigger_value);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_R_ANALOG, right.trigger_value);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_L_DIGITAL, left.trigger_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_R_DIGITAL, right.trigger_button);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_MAIN_STICK_X, gamecube_left_stick.x);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_MAIN_STICK_Y, gamecube_left_stick.y);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_C_STICK_X, gamecube_right_stick.x);
    ciface::Touch::SetControlState(0, ControlID::GCPAD_C_STICK_Y, gamecube_right_stick.y);

    if (!logged_touch_active)
    {
      KQXR_LOGI("Touch input active: sticks, A/B/X/Y, L/R, right grip Z, left grip/menu Start");
      logged_touch_active = true;
    }
  }

  void ClearControllerInputStates() const
  {
    if (!input_registered)
      return;

    using ciface::Touch::ControlID;
    constexpr std::array<ControlID, 14> controls{
        ControlID::GCPAD_A_BUTTON,  ControlID::GCPAD_B_BUTTON,     ControlID::GCPAD_X_BUTTON,
        ControlID::GCPAD_Y_BUTTON,  ControlID::GCPAD_Z_BUTTON,     ControlID::GCPAD_START_BUTTON,
        ControlID::GCPAD_L_DIGITAL, ControlID::GCPAD_R_DIGITAL,    ControlID::GCPAD_L_ANALOG,
        ControlID::GCPAD_R_ANALOG,  ControlID::GCPAD_MAIN_STICK_X, ControlID::GCPAD_MAIN_STICK_Y,
        ControlID::GCPAD_C_STICK_X, ControlID::GCPAD_C_STICK_Y,
    };
    for (const ControlID control : controls)
      ciface::Touch::ClearControlState(0, control);
  }

  void ResetOpenXRInputState()
  {
    if (!input_state_published)
      return;
    Common::VR::OpenXRInputState::Reset();
    input_state_published = false;
  }

  void UpdateHaptics()
  {
    if (session == XR_NULL_HANDLE || controller_actions.haptic == XR_NULL_HANDLE)
      return;

    const Common::VR::OpenXRHapticsState haptics = Common::VR::OpenXRInputState::GetHaptics();
    constexpr XrDuration pulse_duration = 50'000'000;
    for (size_t hand = 0; hand < controller_actions.hand_paths.size(); ++hand)
    {
      XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
      action_info.action = controller_actions.haptic;
      action_info.subactionPath = controller_actions.hand_paths[hand];
      const float amplitude = std::clamp(haptics.amplitude[hand], 0.0f, 1.0f);
      if (amplitude > 0.001f)
      {
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = pulse_duration;
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = amplitude;
        xrApplyHapticFeedback(session, &action_info,
                              reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        controller_actions.haptics_active[hand] = true;
      }
      else if (controller_actions.haptics_active[hand])
      {
        xrStopHapticFeedback(session, &action_info);
        controller_actions.haptics_active[hand] = false;
      }
    }
  }

  void StopHaptics()
  {
    if (session == XR_NULL_HANDLE || controller_actions.haptic == XR_NULL_HANDLE)
      return;

    for (size_t hand = 0; hand < controller_actions.hand_paths.size(); ++hand)
    {
      if (!controller_actions.haptics_active[hand])
        continue;
      XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
      action_info.action = controller_actions.haptic;
      action_info.subactionPath = controller_actions.hand_paths[hand];
      xrStopHapticFeedback(session, &action_info);
      controller_actions.haptics_active[hand] = false;
    }
  }

  void DestroyControllerActions()
  {
    StopHaptics();
    if (input_registered)
    {
      ciface::Touch::UnregisterGameCubeInputOverrider(0);
      input_registered = false;
    }
    for (XrSpace& space : controller_actions.aim_spaces)
    {
      if (space != XR_NULL_HANDLE)
        xrDestroySpace(space);
      space = XR_NULL_HANDLE;
    }
    for (XrSpace& space : controller_actions.grip_spaces)
    {
      if (space != XR_NULL_HANDLE)
        xrDestroySpace(space);
      space = XR_NULL_HANDLE;
    }
    if (controller_actions.action_set != XR_NULL_HANDLE)
      xrDestroyActionSet(controller_actions.action_set);
    controller_actions = {};
    Common::VR::OpenXRInputState::Reset();
    input_state_published = false;
  }

  bool CreateSwapchains()
  {
    uint32_t view_count = 0;
    if (!XrOk(xrEnumerateViewConfigurationViews(instance, system_id,
                                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                &view_count, nullptr),
              "xrEnumerateViewConfigurationViews(count)") ||
        view_count != 2)
    {
      KQXR_LOGE("Quest stereo view configuration reported %u views instead of 2", view_count);
      return false;
    }

    config_views.resize(view_count);
    views.resize(view_count);
    for (uint32_t i = 0; i < view_count; ++i)
    {
      config_views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
      views[i].type = XR_TYPE_VIEW;
    }
    if (!XrOk(xrEnumerateViewConfigurationViews(instance, system_id,
                                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                view_count, &view_count, config_views.data()),
              "xrEnumerateViewConfigurationViews"))
    {
      return false;
    }

    uint32_t format_count = 0;
    if (!XrOk(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
              "xrEnumerateSwapchainFormats(count)") ||
        format_count == 0)
    {
      return false;
    }
    std::vector<int64_t> formats(format_count);
    if (!XrOk(xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data()),
              "xrEnumerateSwapchainFormats"))
    {
      return false;
    }

    constexpr std::array<int64_t, 3> preferred_formats{GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGB10_A2};
    int64_t selected_format = 0;
    for (const int64_t preferred : preferred_formats)
    {
      if (std::find(formats.begin(), formats.end(), preferred) != formats.end())
      {
        selected_format = preferred;
        break;
      }
    }
    if (selected_format == 0)
    {
      KQXR_LOGE("The runtime offered no compatible GLES color swapchain format");
      return false;
    }

    const AbstractTextureFormat framebuffer_format = selected_format == GL_RGB10_A2 ?
                                                         AbstractTextureFormat::RGB10_A2 :
                                                         AbstractTextureFormat::RGBA8;
    GLint previous_read_framebuffer = 0;
    GLint previous_draw_framebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_framebuffer);
    Common::ScopeGuard restore_framebuffer_bindings([&] {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read_framebuffer));
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw_framebuffer));
    });

    swapchains.resize(view_count);
    for (uint32_t eye = 0; eye < view_count; ++eye)
    {
      XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      create_info.usageFlags =
          XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      create_info.format = selected_format;
      create_info.sampleCount = 1;
      create_info.width = config_views[eye].recommendedImageRectWidth;
      create_info.height = config_views[eye].recommendedImageRectHeight;
      create_info.faceCount = 1;
      create_info.arraySize = 1;
      create_info.mipCount = 1;

      Swapchain& swapchain = swapchains[eye];
      swapchain.width = static_cast<int32_t>(create_info.width);
      swapchain.height = static_cast<int32_t>(create_info.height);
      if (!XrOk(xrCreateSwapchain(session, &create_info, &swapchain.handle), "xrCreateSwapchain"))
      {
        return false;
      }

      uint32_t image_count = 0;
      if (!XrOk(xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr),
                "xrEnumerateSwapchainImages(count)"))
      {
        return false;
      }
      swapchain.images.resize(image_count);
      for (XrSwapchainImageOpenGLESKHR& image : swapchain.images)
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
      if (!XrOk(xrEnumerateSwapchainImages(
                    swapchain.handle, image_count, &image_count,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
                "xrEnumerateSwapchainImages"))
      {
        return false;
      }
      if (image_count == 0)
      {
        KQXR_LOGE("Eye %u swapchain contains no images", eye);
        return false;
      }

      swapchain.framebuffers.reserve(image_count);
      for (uint32_t image_index = 0; image_index < image_count; ++image_index)
      {
        while (glGetError() != GL_NO_ERROR)
        {
        }

        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               swapchain.images[image_index].image, 0);
        constexpr GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw_buffer);

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum error = glGetError();
        if (framebuffer == 0 || status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR)
        {
          KQXR_LOGE("Eye %u image %u framebuffer creation failed: fbo=%u status=0x%x error=0x%x",
                    eye, image_index, framebuffer, status, error);
          if (framebuffer != 0)
            glDeleteFramebuffers(1, &framebuffer);
          return false;
        }

        swapchain.framebuffers.emplace_back(std::make_unique<OGLFramebuffer>(
            nullptr, nullptr, std::vector<AbstractTexture*>{}, framebuffer_format,
            AbstractTextureFormat::Undefined, static_cast<u32>(swapchain.width),
            static_cast<u32>(swapchain.height), 1, 1, framebuffer));
      }
      KQXR_LOGI("Eye %u swapchain: %dx%d, %u images, format=0x%llx; persistent FBOs complete", eye,
                swapchain.width, swapchain.height, image_count,
                static_cast<unsigned long long>(selected_format));
    }
    return true;
  }

  bool RenderFrame(const OGLTexture& source, const MathUtil::Rectangle<int>& source_rect,
                   float source_aspect, std::string_view source_type,
                   const EyeRenderCallback& render_eye)
  {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (!XrOk(xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame"))
    {
      exit_requested = true;
      return false;
    }

    // The controller poses and velocities are sampled at the same OpenXR time as this cinema
    // frame. KQCube can switch to a measured "now" timestamp later without changing consumers.
    SyncControllerInput(frame_state.predictedDisplayTime, source_aspect);

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (!XrOk(xrBeginFrame(session, &begin_info), "xrBeginFrame"))
    {
      exit_requested = true;
      return false;
    }

    std::array<XrCompositionLayerQuad, 2> quads{};
    std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
    uint32_t layer_count = 0;
    bool rendered = frame_state.shouldRender == XR_TRUE;

    if (rendered)
    {
      LocateViews(frame_state.predictedDisplayTime);

      const u32 source_layers = source.GetLayers();
      if (source_type != logged_source_type || source.GetWidth() != logged_source_width ||
          source.GetHeight() != logged_source_height || source_layers != logged_source_layers)
      {
        KQXR_LOGI(
            "Presenter source=%.*s texture=%u target=0x%x type=%u size=%ux%u layers=%u samples=%u "
            "rect=(%d,%d)-(%d,%d)",
            static_cast<int>(source_type.size()), source_type.data(), source.GetGLTextureId(),
            source.GetGLTarget(), static_cast<unsigned int>(source.GetConfig().type),
            source.GetWidth(), source.GetHeight(), source_layers, source.GetSamples(),
            source_rect.left, source_rect.top, source_rect.right, source_rect.bottom);
        logged_source_type = source_type;
        logged_source_width = source.GetWidth();
        logged_source_height = source.GetHeight();
        logged_source_layers = source_layers;
      }
      if (source_layers < 2 && !logged_mono_fallback)
      {
        KQXR_LOGW("Dolphin supplied a mono presentation source; layer 0 is the only safe fallback");
        logged_mono_fallback = true;
      }
      else if (source_layers >= 2 && !logged_stereo_source)
      {
        KQXR_LOGI("Dolphin supplied stereo XFB layers: layer 0 left, layer 1 right");
        logged_stereo_source = true;
      }

      for (uint32_t eye = 0; eye < swapchains.size(); ++eye)
      {
        Swapchain& swapchain = swapchains[eye];
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t image_index = 0;
        if (!XrOk(xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &image_index),
                  "xrAcquireSwapchainImage"))
        {
          rendered = false;
          break;
        }

        XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        image_wait.timeout = XR_INFINITE_DURATION;
        if (!XrOk(xrWaitSwapchainImage(swapchain.handle, &image_wait), "xrWaitSwapchainImage"))
        {
          XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
          XrOk(xrReleaseSwapchainImage(swapchain.handle, &release_info),
               "xrReleaseSwapchainImage(after wait failure)");
          exit_requested = true;
          rendered = false;
          break;
        }
        if (!logged_eye_acquire[eye])
        {
          KQXR_LOGI("Eye %u acquired and waited for swapchain image %u", eye, image_index);
          logged_eye_acquire[eye] = true;
        }

        const u32 source_layer = source_layers >= 2 ? eye : 0;
        const bool eye_rendered = RenderEye(eye, source_layer, swapchain, image_index, render_eye);
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const bool image_released = XrOk(xrReleaseSwapchainImage(swapchain.handle, &release_info),
                                         "xrReleaseSwapchainImage");
        if (image_released && !logged_eye_release[eye])
        {
          KQXR_LOGI("Eye %u released swapchain image %u after drawing source layer %u", eye,
                    image_index, source_layer);
          logged_eye_release[eye] = true;
        }
        if (!eye_rendered || !image_released)
        {
          rendered = false;
          break;
        }
      }

      if (rendered)
      {
        const XrPosef screen_pose = GetVirtualScreenPose();
        const XrExtent2Df screen_size = GetVirtualScreenSize(source_aspect);
        for (uint32_t eye = 0; eye < quads.size(); ++eye)
        {
          XrCompositionLayerQuad& quad = quads[eye];
          quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
          quad.space = local_space;
          quad.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
          quad.subImage.swapchain = swapchains[eye].handle;
          quad.subImage.imageRect.offset = {0, 0};
          quad.subImage.imageRect.extent = {swapchains[eye].width, swapchains[eye].height};
          quad.subImage.imageArrayIndex = 0;
          quad.pose = screen_pose;
          quad.size = screen_size;
          layers[eye] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        }
        layer_count = static_cast<uint32_t>(layers.size());
      }
    }

    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = layer_count;
    end_info.layers = layer_count == 0 ? nullptr : layers.data();
    if (!XrOk(xrEndFrame(session, &end_info), "xrEndFrame"))
    {
      exit_requested = true;
      return false;
    }

    if (layer_count != 0 && submitted_frames++ == 0)
      KQXR_LOGI("First OpenXR stereo quad pair submitted successfully");
    return true;
  }

  bool RenderEye(uint32_t eye, u32 source_layer, const Swapchain& swapchain, uint32_t image_index,
                 const EyeRenderCallback& render_eye)
  {
    if (image_index >= swapchain.images.size() || image_index >= swapchain.framebuffers.size())
    {
      KQXR_LOGE("Runtime returned invalid eye %u swapchain image index %u", eye, image_index);
      return false;
    }

    while (glGetError() != GL_NO_ERROR)
    {
    }

    OGLFramebuffer* const framebuffer = swapchain.framebuffers[image_index].get();
    const bool eye_rendered = render_eye && render_eye(eye, source_layer, framebuffer);
    glFlush();

    GLint read_binding = 0;
    GLint draw_binding = 0;
    GLint draw_buffer = 0;
    GLint viewport[4]{};
    GLint scissor_box[4]{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_binding);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_binding);
    glGetIntegerv(GL_DRAW_BUFFER0, &draw_buffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    const GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    const GLenum error = glGetError();
    const bool correct_binding = read_binding == static_cast<GLint>(framebuffer->GetFBO()) &&
                                 draw_binding == static_cast<GLint>(framebuffer->GetFBO());
    const bool complete =
        read_status == GL_FRAMEBUFFER_COMPLETE && draw_status == GL_FRAMEBUFFER_COMPLETE;

    if (eye < logged_eye_draw.size() && !logged_eye_draw[eye])
    {
      KQXR_LOGI("Eye %u drew source layer %u -> image %u FBO %u: read=%d/0x%x draw=%d/0x%x "
                "drawBuffer=0x%x viewport=(%d,%d %dx%d) scissor=%s (%d,%d %dx%d) glError=0x%x",
                eye, source_layer, image_index, framebuffer->GetFBO(), read_binding, read_status,
                draw_binding, draw_status, draw_buffer, viewport[0], viewport[1], viewport[2],
                viewport[3], scissor_enabled == GL_TRUE ? "on" : "off", scissor_box[0],
                scissor_box[1], scissor_box[2], scissor_box[3], error);
      logged_eye_draw[eye] = true;
    }

    if (!eye_rendered || !correct_binding || !complete || error != GL_NO_ERROR)
    {
      KQXR_LOGE("Eye %u draw failed for source layer %u: callback=%d binding=%d complete=%d "
                "readStatus=0x%x drawStatus=0x%x glError=0x%x",
                eye, source_layer, eye_rendered, correct_binding, complete, read_status,
                draw_status, error);
      return false;
    }
    return true;
  }

  void LocateViews(XrTime display_time)
  {
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = display_time;
    locate_info.space = local_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t view_count = 0;
    const XrResult result =
        xrLocateViews(session, &locate_info, &view_state, static_cast<uint32_t>(views.size()),
                      &view_count, views.data());
    if (XR_FAILED(result) || view_count != views.size())
    {
      if (!logged_view_failure)
      {
        KQXR_LOGW("Tracked views are not available yet: %s, count=%u", XrResultName(result),
                  view_count);
        logged_view_failure = true;
      }
      return;
    }

    if (!logged_valid_views &&
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0)
    {
      KQXR_LOGI("Tracked headset views are valid; LOCAL-space screen orientation is live");
      logged_valid_views = true;
    }
  }

  void PollEvents()
  {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (true)
    {
      const XrResult result = xrPollEvent(instance, &event);
      if (result == XR_EVENT_UNAVAILABLE)
        break;
      if (XR_FAILED(result))
      {
        KQXR_LOGE("xrPollEvent failed: %s", XrResultName(result));
        exit_requested = true;
        break;
      }

      const auto* header = reinterpret_cast<const XrEventDataBaseHeader*>(&event);
      if (header->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
      {
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(header);
        session_state = changed->state;
        KQXR_LOGI("Session state -> %s", SessionStateName(session_state));
        if (session_state == XR_SESSION_STATE_READY)
        {
          XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
          begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          if (XrOk(xrBeginSession(session, &begin_info), "xrBeginSession"))
            session_running = true;
          else
            exit_requested = true;
        }
        else if (session_state == XR_SESSION_STATE_STOPPING)
        {
          StopHaptics();
          ClearControllerInputStates();
          ResetOpenXRInputState();
          session_running = false;
          XrOk(xrEndSession(session), "xrEndSession");
        }
        else if (session_state == XR_SESSION_STATE_EXITING ||
                 session_state == XR_SESSION_STATE_LOSS_PENDING)
        {
          StopHaptics();
          ClearControllerInputStates();
          ResetOpenXRInputState();
          session_running = false;
          exit_requested = true;
        }
      }
      else if (header->type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
      {
        StopHaptics();
        ClearControllerInputStates();
        ResetOpenXRInputState();
        session_running = false;
        exit_requested = true;
      }
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  }

  bool XrOk(XrResult result, std::string_view operation) const
  {
    if (XR_SUCCEEDED(result))
      return true;
    KQXR_LOGE("%.*s failed: %s", static_cast<int>(operation.size()), operation.data(),
              XrResultName(result));
    return false;
  }

  const char* XrResultName(XrResult result) const
  {
    static thread_local char buffer[XR_MAX_RESULT_STRING_SIZE];
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, buffer)))
      return buffer;
    std::snprintf(buffer, sizeof(buffer), "XrResult(%d)", result);
    return buffer;
  }

  void FailInitialization()
  {
    KQXR_LOGE("OpenXR initialization failed; restoring normal Android presentation");
    DestroyOpenXRObjects();
    ReleaseParking(true);
    DeleteGlobalRef(java_vm, activity);
    activity = nullptr;
    owns_presentation = false;
    SetPresentationActive(false);
    s_presentation_failed.store(true, std::memory_order_release);
    failed = true;
  }

  void Shutdown()
  {
    if (instance != XR_NULL_HANDLE)
      KQXR_LOGI("Destroying OpenXR presenter on Dolphin's render thread");
    DestroyOpenXRObjects();
    ReleaseParking(true);
    DeleteGlobalRef(java_vm, activity);
    activity = nullptr;
    initialized = false;
    owns_presentation = false;
    SetPresentationActive(false);
  }

  void DestroyOpenXRObjects()
  {
    if (session_running && session != XR_NULL_HANDLE)
    {
      StopHaptics();
      if (session_state == XR_SESSION_STATE_STOPPING)
      {
        XrOk(xrEndSession(session), "xrEndSession(shutdown)");
        session_running = false;
      }
      else
      {
        const XrResult exit_result = xrRequestExitSession(session);
        if (XR_FAILED(exit_result))
        {
          KQXR_LOGW("xrRequestExitSession(shutdown) failed: %s", XrResultName(exit_result));
        }
        else
        {
          // Give the runtime a short opportunity to return STOPPING so xrEndSession runs before
          // the session is destroyed. This also releases Quest controller focus reliably when
          // returning to Dolphin's game grid.
          constexpr int shutdown_poll_attempts = 50;
          for (int attempt = 0; attempt < shutdown_poll_attempts && session_running; ++attempt)
          {
            PollEvents();
            if (session_running)
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          if (session_running)
            KQXR_LOGW("OpenXR session did not enter STOPPING before shutdown timeout");
        }
      }
      session_running = false;
    }
    for (Swapchain& swapchain : swapchains)
    {
      swapchain.framebuffers.clear();
      if (swapchain.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(swapchain.handle);
    }
    swapchains.clear();
    // Action spaces are session children and must be released before the session itself.
    DestroyControllerActions();
    if (local_space != XR_NULL_HANDLE)
      xrDestroySpace(local_space);
    if (session != XR_NULL_HANDLE)
      xrDestroySession(session);
    if (instance != XR_NULL_HANDLE)
      xrDestroyInstance(instance);
    local_space = XR_NULL_HANDLE;
    session = XR_NULL_HANDLE;
    instance = XR_NULL_HANDLE;
  }

  void ReleaseParking(bool restore_window)
  {
    if (egl_display == EGL_NO_DISPLAY || egl_context == EGL_NO_CONTEXT)
      return;

    const EGLSurface current_parking_surface =
        parking_is_surfaceless ? EGL_NO_SURFACE : egl_parking_surface;
    const bool parking_context_current = eglGetCurrentDisplay() == egl_display &&
                                         eglGetCurrentContext() == egl_context &&
                                         eglGetCurrentSurface(EGL_DRAW) == current_parking_surface;
    bool restored = false;
    if (parking_context_current && restore_window && egl_window_surface != EGL_NO_SURFACE)
    {
      restored = eglMakeCurrent(egl_display, egl_window_surface, egl_window_surface, egl_context) ==
                 EGL_TRUE;
      if (restored)
        KQXR_LOGI("Restored Dolphin's GLES context to the Android window");
      else
        KQXR_LOGW("Android EGL window could not be restored: 0x%x", eglGetError());
    }
    if (parking_context_current && !restored)
      eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (egl_parking_surface != EGL_NO_SURFACE)
      eglDestroySurface(egl_display, egl_parking_surface);
    egl_display = EGL_NO_DISPLAY;
    egl_context = EGL_NO_CONTEXT;
    egl_config = nullptr;
    egl_window_surface = EGL_NO_SURFACE;
    egl_parking_surface = EGL_NO_SURFACE;
    parking_is_surfaceless = false;
  }

  JavaVM* java_vm = nullptr;
  jobject activity = nullptr;

  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLContext egl_context = EGL_NO_CONTEXT;
  EGLConfig egl_config = nullptr;
  EGLSurface egl_window_surface = EGL_NO_SURFACE;
  EGLSurface egl_parking_surface = EGL_NO_SURFACE;
  bool parking_is_surfaceless = false;

  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system_id = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  std::vector<XrViewConfigurationView> config_views;
  std::vector<XrView> views;
  std::vector<Swapchain> swapchains;
  ControllerActions controller_actions;

  uint64_t submitted_frames = 0;
  u32 logged_source_width = 0;
  u32 logged_source_height = 0;
  u32 logged_source_layers = std::numeric_limits<u32>::max();
  std::string logged_source_type;
  std::array<bool, 2> logged_eye_acquire{};
  std::array<bool, 2> logged_eye_draw{};
  std::array<bool, 2> logged_eye_release{};
  std::array<XrPath, 2> logged_interaction_profiles{XR_NULL_PATH, XR_NULL_PATH};
  uint64_t input_sync_count = 0;
  bool initialized = false;
  bool failed = false;
  bool owns_presentation = false;
  bool session_running = false;
  bool exit_requested = false;
  bool logged_mono_fallback = false;
  bool logged_stereo_source = false;
  bool logged_view_failure = false;
  bool logged_valid_views = false;
  bool input_registered = false;
  bool input_state_published = false;
  bool logged_touch_active = false;
  bool meta_touch_plus_enabled = false;
};

KQCubeOpenXR::KQCubeOpenXR() : m_impl(std::make_unique<Impl>())
{
}

KQCubeOpenXR::~KQCubeOpenXR() = default;

bool KQCubeOpenXR::Present(const OGLTexture& source, const MathUtil::Rectangle<int>& source_rect,
                           float source_aspect, std::string_view source_type,
                           const EyeRenderCallback& render_eye)
{
  return m_impl->Present(source, source_rect, source_aspect, source_type, render_eye);
}

bool KQCubeOpenXR::OwnsPresentation() const
{
  return m_impl->owns_presentation;
}
}  // namespace OGL

#else

namespace OGL
{
struct KQCubeOpenXR::Impl
{
};

KQCubeOpenXR::KQCubeOpenXR() : m_impl(std::make_unique<Impl>())
{
}

KQCubeOpenXR::~KQCubeOpenXR() = default;

bool KQCubeOpenXR::Present(const OGLTexture&, const MathUtil::Rectangle<int>&, float,
                           std::string_view, const EyeRenderCallback&)
{
  return false;
}

bool KQCubeOpenXR::OwnsPresentation() const
{
  return false;
}

namespace KQCubeOpenXRBridge
{
bool IsRequested()
{
  return false;
}

bool IsPresentationActive()
{
  return false;
}

bool ConsumeExitRequested()
{
  return false;
}

bool ConsumePresentationFailure()
{
  return false;
}
}  // namespace KQCubeOpenXRBridge
}  // namespace OGL

#endif
