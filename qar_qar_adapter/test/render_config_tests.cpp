// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The defaults of the render sender factory's configuration that the room depends on, and the
// default behaviour of a copy hook that cannot read textures back. Neither needs a QarOS session:
// a configuration is a plain value, and the hook here is a stub.

#include <gtest/gtest.h>

#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"

namespace
{

/// Implements the one pure virtual of the interface and nothing else.
class CopyOnlyHook : public qar_qar_adapter::IQarGlTextureCopyHook
{
public:
  bool copy_frame_into(
    const qar_ogre_streaming::QarOpenGlStereoFrame &, const QarVideoFrameOpenGl &) override
  {
    return true;
  }
};

}  // namespace

TEST(RenderConfigDefaults, TheAppVolumeIsATenMetreCubeWhoseCentreIsOneMetreUp)
{
  const qar_qar_adapter::QarCApiSenderFactory::Config config;
  EXPECT_FLOAT_EQ(config.app_volume_width_m, 10.0f);
  EXPECT_FLOAT_EQ(config.app_volume_length_m, 10.0f);
  EXPECT_FLOAT_EQ(config.app_volume_height_m, 10.0f);
  EXPECT_FLOAT_EQ(config.app_volume_center_y_m, 1.0f);
  EXPECT_FLOAT_EQ(config.app_volume_scale, 1.0f);
}

// The runtime's graphics thread makes a sender's context current with the pair stored in the
// sender's own state; it reads nothing from the factory, whose latched handles the render thread
// rewrites every tick.
TEST(GlSenderContextHandles, ASenderBindsItsOwnContextOnTheDeviceContextItWasCreatedOn)
{
  qar_qar_adapter::QarCApiSenderFactory::GlSenderContext sender;
  sender.hdc = reinterpret_cast<void *>(0x1111);
  sender.context = reinterpret_cast<void *>(0x2222);
  void * hdc = nullptr;
  void * context = nullptr;
  ASSERT_TRUE(sender.own_handles(hdc, context));
  EXPECT_EQ(hdc, reinterpret_cast<void *>(0x1111));
  EXPECT_EQ(context, reinterpret_cast<void *>(0x2222));
}

TEST(GlSenderContextHandles, AMissingHandleBindsNothingAndNothingStandsInForIt)
{
  qar_qar_adapter::QarCApiSenderFactory::GlSenderContext sender;
  void * hdc = nullptr;
  void * context = nullptr;
  EXPECT_FALSE(sender.own_handles(hdc, context)) << "nothing was stored";

  sender.context = reinterpret_cast<void *>(0x2222);
  EXPECT_FALSE(sender.own_handles(hdc, context)) << "a context without its device context";
  EXPECT_EQ(context, reinterpret_cast<void *>(0x2222));

  sender.context = nullptr;
  sender.hdc = reinterpret_cast<void *>(0x1111);
  EXPECT_FALSE(sender.own_handles(hdc, context)) << "a device context without its context";
}

TEST(GlSenderContextHandles, EverySenderHasItsOwnPairAndItsOwnFirstBindLine)
{
  qar_qar_adapter::QarCApiSenderFactory::GlSenderContext first;
  qar_qar_adapter::QarCApiSenderFactory::GlSenderContext second;
  first.hdc = reinterpret_cast<void *>(0x1111);
  first.context = reinterpret_cast<void *>(0x2222);
  second.hdc = reinterpret_cast<void *>(0x1111);
  second.context = reinterpret_cast<void *>(0x3333);
  void * hdc = nullptr;
  void * context = nullptr;
  ASSERT_TRUE(second.own_handles(hdc, context));
  EXPECT_EQ(context, reinterpret_cast<void *>(0x3333));
  EXPECT_TRUE(first.bound.first());
  EXPECT_FALSE(first.bound.first());
  EXPECT_TRUE(second.bound.first()) << "the bind of another sender is its own first";
}

TEST(CopyHookDefaults, AHookWithoutTextureReadBackReportsNoColour)
{
  CopyOnlyHook hook;
  const qar_ogre_streaming::QarOpenGlStereoFrame frame;
  float rgb[3] = {1.0f, 2.0f, 3.0f};
  EXPECT_FALSE(hook.mean_left_colour(frame, rgb));
  EXPECT_FLOAT_EQ(rgb[0], 0.0f);
  EXPECT_FLOAT_EQ(rgb[1], 0.0f);
  EXPECT_FLOAT_EQ(rgb[2], 0.0f);
}
