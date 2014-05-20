// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/managed_mode/permission_request_creator_sync.h"

#include "base/callback.h"
#include "base/command_line.h"
#include "base/values.h"
#include "chrome/browser/managed_mode/managed_user_settings_service.h"
#include "chrome/browser/managed_mode/managed_user_shared_settings_service.h"
#include "chrome/common/chrome_switches.h"

using base::Time;

const char kManagedUserAccessRequestKeyPrefix[] =
    "X-ManagedUser-AccessRequests";
const char kManagedUserAccessRequestTime[] = "timestamp";
const char kManagedUserName[] = "name";

// Key for the notification setting of the custodian. This is a shared setting
// so we can include the setting in the access request data that is used to
// trigger notifications.
const char kNotificationSetting[] = "custodian-notification-setting";

PermissionRequestCreatorSync::PermissionRequestCreatorSync(
    ManagedUserSettingsService* settings_service,
    ManagedUserSharedSettingsService* shared_settings_service,
    const std::string& name,
    const std::string& managed_user_id)
    : settings_service_(settings_service),
      shared_settings_service_(shared_settings_service),
      name_(name),
      managed_user_id_(managed_user_id) {
}

PermissionRequestCreatorSync::~PermissionRequestCreatorSync() {}

void PermissionRequestCreatorSync::CreatePermissionRequest(
    const std::string& url_requested,
    const base::Closure& callback) {
  // Add the prefix.
  std::string key = ManagedUserSettingsService::MakeSplitSettingKey(
      kManagedUserAccessRequestKeyPrefix, url_requested);

  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue);

  // TODO(sergiu): Use sane time here when it's ready.
  dict->SetDouble(kManagedUserAccessRequestTime, base::Time::Now().ToJsTime());

  dict->SetString(kManagedUserName, name_);

  // Copy the notification setting of the custodian.
  const base::Value* value = shared_settings_service_->GetValue(
      managed_user_id_, kNotificationSetting);
  bool notifications_enabled = false;
  if (CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableAccessRequestNotifications)) {
    notifications_enabled = true;
  } else if (value) {
    bool success = value->GetAsBoolean(&notifications_enabled);
    DCHECK(success);
  }
  dict->SetBoolean(kNotificationSetting, notifications_enabled);

  settings_service_->UploadItem(key, dict.PassAs<base::Value>());

  callback.Run();
}
