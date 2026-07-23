add_library(videoogl
  GPUTimer.h
  KQCubeOpenXR.cpp
  KQCubeOpenXR.h
  OGLBoundingBox.cpp
  OGLBoundingBox.h
  OGLConfig.cpp
  OGLConfig.h
  OGLGfx.cpp
  OGLGfx.h
  OGLMain.cpp
  OGLNativeVertexFormat.cpp
  OGLPerfQuery.cpp
  OGLPerfQuery.h
  OGLPipeline.cpp
  OGLPipeline.h
  OGLShader.cpp
  OGLShader.h
  OGLStreamBuffer.cpp
  OGLStreamBuffer.h
  OGLTexture.cpp
  OGLTexture.h
  OGLVertexManager.cpp
  OGLVertexManager.h
  ProgramShaderCache.cpp
  ProgramShaderCache.h
  SamplerCache.cpp
  SamplerCache.h
  VideoBackend.h
)

target_link_libraries(videoogl PUBLIC common videocommon)
if(ANDROID)
  find_package(OpenXR REQUIRED CONFIG)
  target_compile_definitions(videoogl PRIVATE
    XR_USE_GRAPHICS_API_OPENGL_ES
    XR_USE_PLATFORM_ANDROID
  )
  target_link_libraries(videoogl PRIVATE OpenXR::openxr_loader inputcommon android EGL GLESv3 log)
endif()
if(X11_FOUND)
  target_link_libraries(videoogl PRIVATE PkgConfig::X11)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(videoogl PRIVATE use_pch)
endif()
