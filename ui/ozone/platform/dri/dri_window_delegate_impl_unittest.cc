// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/ozone/platform/dri/dri_buffer.h"
#include "ui/ozone/platform/dri/dri_surface.h"
#include "ui/ozone/platform/dri/dri_surface_factory.h"
#include "ui/ozone/platform/dri/dri_window_delegate_impl.h"
#include "ui/ozone/platform/dri/dri_window_delegate_manager.h"
#include "ui/ozone/platform/dri/hardware_display_controller.h"
#include "ui/ozone/platform/dri/screen_manager.h"
#include "ui/ozone/platform/dri/test/mock_dri_wrapper.h"
#include "ui/ozone/public/surface_ozone_canvas.h"

namespace {

// Mode of size 6x4.
const drmModeModeInfo kDefaultMode =
    {0, 6, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, {'\0'}};

const gfx::AcceleratedWidget kDefaultWidgetHandle = 1;
const uint32_t kDefaultCrtc = 1;
const uint32_t kDefaultConnector = 2;

class MockScreenManager : public ui::ScreenManager {
 public:
  MockScreenManager(ui::DriWrapper* dri,
                    ui::ScanoutBufferGenerator* buffer_generator)
      : ScreenManager(dri, buffer_generator), dri_(dri) {}
  ~MockScreenManager() override {}

  // Normally we'd use DRM to figure out the controller configuration. But we
  // can't use DRM in unit tests, so we just create a fake configuration.
  void ForceInitializationOfPrimaryDisplay() override {
    ConfigureDisplayController(kDefaultCrtc, kDefaultConnector, gfx::Point(),
                               kDefaultMode);
  }

 private:
  ui::DriWrapper* dri_;  // Not owned.

  DISALLOW_COPY_AND_ASSIGN(MockScreenManager);
};

}  // namespace

class DriWindowDelegateImplTest : public testing::Test {
 public:
  DriWindowDelegateImplTest() {}

  void SetUp() override;
  void TearDown() override;

 protected:
  scoped_ptr<base::MessageLoop> message_loop_;
  scoped_ptr<ui::MockDriWrapper> dri_;
  scoped_ptr<ui::DriBufferGenerator> buffer_generator_;
  scoped_ptr<MockScreenManager> screen_manager_;
  scoped_ptr<ui::DriWindowDelegateManager> window_delegate_manager_;

 private:
  DISALLOW_COPY_AND_ASSIGN(DriWindowDelegateImplTest);
};

void DriWindowDelegateImplTest::SetUp() {
  message_loop_.reset(new base::MessageLoopForUI);
  dri_.reset(new ui::MockDriWrapper(3));
  buffer_generator_.reset(new ui::DriBufferGenerator(dri_.get()));
  screen_manager_.reset(
      new MockScreenManager(dri_.get(), buffer_generator_.get()));
  window_delegate_manager_.reset(new ui::DriWindowDelegateManager());

  scoped_ptr<ui::DriWindowDelegate> window_delegate(
      new ui::DriWindowDelegateImpl(kDefaultWidgetHandle, dri_.get(),
                                    window_delegate_manager_.get(),
                                    screen_manager_.get()));
  window_delegate->Initialize();
  window_delegate_manager_->AddWindowDelegate(kDefaultWidgetHandle,
                                              window_delegate.Pass());
}

void DriWindowDelegateImplTest::TearDown() {
  scoped_ptr<ui::DriWindowDelegate> delegate =
      window_delegate_manager_->RemoveWindowDelegate(kDefaultWidgetHandle);
  delegate->Shutdown();
  message_loop_.reset();
}

TEST_F(DriWindowDelegateImplTest, SetCursorImage) {
  SkBitmap image;
  SkImageInfo info =
      SkImageInfo::Make(6, 4, kN32_SkColorType, kPremul_SkAlphaType);
  image.allocPixels(info);
  image.eraseColor(SK_ColorWHITE);

  std::vector<SkBitmap> cursor_bitmaps;
  cursor_bitmaps.push_back(image);
  window_delegate_manager_->GetWindowDelegate(kDefaultWidgetHandle)
      ->SetCursor(cursor_bitmaps, gfx::Point(4, 2), 0);

  SkBitmap cursor;
  // Buffers 0 and 1 are the cursor buffers.
  cursor.setInfo(dri_->buffers()[1]->getCanvas()->imageInfo());
  EXPECT_TRUE(dri_->buffers()[1]->getCanvas()->readPixels(&cursor, 0, 0));

  // Check that the frontbuffer is displaying the right image as set above.
  for (int i = 0; i < cursor.height(); ++i) {
    for (int j = 0; j < cursor.width(); ++j) {
      if (j < info.width() && i < info.height())
        EXPECT_EQ(SK_ColorWHITE, cursor.getColor(j, i));
      else
        EXPECT_EQ(static_cast<SkColor>(SK_ColorTRANSPARENT),
                  cursor.getColor(j, i));
    }
  }
}
