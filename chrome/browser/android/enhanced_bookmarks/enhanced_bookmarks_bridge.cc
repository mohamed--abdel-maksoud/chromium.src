// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/enhanced_bookmarks/enhanced_bookmarks_bridge.h"

#include "base/android/jni_string.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile_android.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/android/bookmark_type.h"
#include "components/enhanced_bookmarks/metadata_accessor.h"
#include "jni/EnhancedBookmarksBridge_jni.h"

using bookmarks::BookmarkType;

namespace enhanced_bookmarks {
namespace android {

EnhancedBookmarksBridge::EnhancedBookmarksBridge(JNIEnv* env,
                                                 jobject obj,
                                                 Profile* profile) {
  profile_ = profile;
  bookmark_model_ = BookmarkModelFactory::GetForProfile(profile_);
}

void EnhancedBookmarksBridge::Destroy(JNIEnv*, jobject) {
  delete this;
}

ScopedJavaLocalRef<jstring> EnhancedBookmarksBridge::GetBookmarkDescription(
    JNIEnv* env, jobject obj, jlong id, jint type) {
  DCHECK(bookmark_model_->loaded());
  DCHECK_EQ(type, BookmarkType::NORMAL);

  const BookmarkNode* node = bookmarks::GetBookmarkNodeByID(
      bookmark_model_, static_cast<int64>(id));

  return node ?
          base::android::ConvertUTF8ToJavaString(
              env, enhanced_bookmarks::DescriptionFromBookmark(node)) :
          ScopedJavaLocalRef<jstring>();
}

void EnhancedBookmarksBridge::SetBookmarkDescription(JNIEnv* env,
                                                     jobject obj,
                                                     jlong id,
                                                     jint type,
                                                     jstring description) {
  DCHECK(bookmark_model_->loaded());
  DCHECK_EQ(type, BookmarkType::NORMAL);

  const BookmarkNode* node = bookmarks::GetBookmarkNodeByID(
      bookmark_model_, static_cast<int64>(id));

  enhanced_bookmarks::SetDescriptionForBookmark(
      bookmark_model_, node,
      base::android::ConvertJavaStringToUTF8(env, description));
}

static jlong Init(JNIEnv* env, jobject obj, jobject j_profile) {
  return reinterpret_cast<jlong>(new EnhancedBookmarksBridge(
        env, obj, ProfileAndroid::FromProfileAndroid(j_profile)));
}

bool RegisterEnhancedBookmarksBridge(JNIEnv* env) {
  return RegisterNativesImpl(env);
}

}  // namespace android
}  // namespace enhanced_bookmarks
