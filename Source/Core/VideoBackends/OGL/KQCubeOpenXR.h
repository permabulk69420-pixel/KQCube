// Copyright 2026 KQCube contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/MathUtil.h"

#ifdef ANDROID
#include <jni.h>
#endif

namespace OGL
{
class OGLTexture;

// Quest/OpenXR presentation is intentionally contained in the OpenGL backend. The rest of
// Dolphin only sees one optional presentation hook and keeps its ordinary Android path intact.
class KQCubeOpenXR final
{
public:
  KQCubeOpenXR();
  ~KQCubeOpenXR();

  KQCubeOpenXR(const KQCubeOpenXR&) = delete;
  KQCubeOpenXR& operator=(const KQCubeOpenXR&) = delete;

  bool Present(const OGLTexture& source, const MathUtil::Rectangle<int>& source_rect);
  bool OwnsPresentation() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

namespace KQCubeOpenXRBridge
{
#ifdef ANDROID
void SetAndroidActivity(JNIEnv* env, jobject activity);
void ClearAndroidActivity(JNIEnv* env, jobject activity);
#endif

bool IsRequested();
bool IsPresentationActive();
bool ConsumeExitRequested();
bool ConsumePresentationFailure();
}  // namespace KQCubeOpenXRBridge
}  // namespace OGL
