// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/permissions/system/system_permission_settings.h"

#include <gio/gio.h>

#include <memory>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/process/launch.h"
#include "chrome/browser/permissions/system/platform_handle.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "ui/base/glib/gsettings.h"

namespace system_permission_settings {

namespace {

constexpr char kLocationSchema[] = "org.gnome.system.location";
constexpr char kLocationEnabledKey[] = "enabled";
constexpr char kPrivacySchema[] = "org.gnome.desktop.privacy";
constexpr char kCameraDisabledKey[] = "disable-camera";
constexpr char kMicrophoneDisabledKey[] = "disable-microphone";

bool HasSetting(const char* schema_id, const char* key) {
  GSettingsSchemaSource* schema_source = g_settings_schema_source_get_default();
  if (!schema_source) {
    return false;
  }

  GSettingsSchema* schema =
      g_settings_schema_source_lookup(schema_source, schema_id, true);
  if (!schema) {
    return false;
  }

  const bool has_key = g_settings_schema_has_key(schema, key);
  g_settings_schema_unref(schema);
  return has_key;
}

class LinuxSettingsObservation : public ScopedObservation {
 public:
  LinuxSettingsObservation(ScopedGObject<GSettings> location_settings,
                           ScopedGObject<GSettings> privacy_settings,
                           SystemPermissionChangedCallback callback)
      : location_settings_(std::move(location_settings)),
        privacy_settings_(std::move(privacy_settings)),
        callback_(std::move(callback)) {
    if (location_settings_ &&
        HasSetting(kLocationSchema, kLocationEnabledKey)) {
      location_signal_id_ =
          g_signal_connect(location_settings_, "changed::enabled",
                           G_CALLBACK(OnLocationChanged), this);
    }
    if (privacy_settings_ && HasSetting(kPrivacySchema, kCameraDisabledKey)) {
      camera_signal_id_ =
          g_signal_connect(privacy_settings_, "changed::disable-camera",
                           G_CALLBACK(OnPrivacyChanged), this);
    }
    if (privacy_settings_ &&
        HasSetting(kPrivacySchema, kMicrophoneDisabledKey)) {
      microphone_signal_id_ =
          g_signal_connect(privacy_settings_, "changed::disable-microphone",
                           G_CALLBACK(OnPrivacyChanged), this);
    }
  }

  LinuxSettingsObservation(const LinuxSettingsObservation&) = delete;
  LinuxSettingsObservation& operator=(const LinuxSettingsObservation&) = delete;

  ~LinuxSettingsObservation() override {
    Disconnect(location_settings_, location_signal_id_);
    Disconnect(privacy_settings_, camera_signal_id_);
    Disconnect(privacy_settings_, microphone_signal_id_);
  }

  bool is_observing() const {
    return location_signal_id_ || camera_signal_id_ || microphone_signal_id_;
  }

 private:
  static void Disconnect(GSettings* settings, gulong signal_id) {
    if (settings && signal_id) {
      g_signal_handler_disconnect(settings, signal_id);
    }
  }

  static void OnLocationChanged(GSettings* settings,
                                gchar* key,
                                gpointer user_data) {
    auto* observation = static_cast<LinuxSettingsObservation*>(user_data);
    observation->callback_.Run(ContentSettingsType::GEOLOCATION,
                               !g_settings_get_boolean(settings, key));
  }

  static void OnPrivacyChanged(GSettings* settings,
                               gchar* key,
                               gpointer user_data) {
    auto* observation = static_cast<LinuxSettingsObservation*>(user_data);
    if (std::string(key) == kCameraDisabledKey) {
      const bool is_blocked = g_settings_get_boolean(settings, key);
      observation->callback_.Run(ContentSettingsType::MEDIASTREAM_CAMERA,
                                 is_blocked);
      observation->callback_.Run(ContentSettingsType::CAMERA_PAN_TILT_ZOOM,
                                 is_blocked);
    } else if (std::string(key) == kMicrophoneDisabledKey) {
      observation->callback_.Run(ContentSettingsType::MEDIASTREAM_MIC,
                                 g_settings_get_boolean(settings, key));
    }
  }

  ScopedGObject<GSettings> location_settings_;
  ScopedGObject<GSettings> privacy_settings_;
  SystemPermissionChangedCallback callback_;
  gulong location_signal_id_ = 0;
  gulong camera_signal_id_ = 0;
  gulong microphone_signal_id_ = 0;
};

class PlatformHandleImpl : public PlatformHandle {
 public:
  PlatformHandleImpl()
      : location_settings_(ui::GSettingsNew(kLocationSchema)),
        privacy_settings_(ui::GSettingsNew(kPrivacySchema)),
        has_location_setting_(location_settings_ &&
                              HasSetting(kLocationSchema, kLocationEnabledKey)),
        has_camera_setting_(privacy_settings_ &&
                            HasSetting(kPrivacySchema, kCameraDisabledKey)),
        has_microphone_setting_(
            privacy_settings_ &&
            HasSetting(kPrivacySchema, kMicrophoneDisabledKey)) {}

  PlatformHandleImpl(const PlatformHandleImpl&) = delete;
  PlatformHandleImpl& operator=(const PlatformHandleImpl&) = delete;
  ~PlatformHandleImpl() override = default;

  bool CanPrompt(ContentSettingsType type) override { return false; }

  bool IsDenied(ContentSettingsType type) override {
    switch (type) {
      case ContentSettingsType::GEOLOCATION:
        return has_location_setting_ &&
               !g_settings_get_boolean(location_settings_, kLocationEnabledKey);
      case ContentSettingsType::MEDIASTREAM_CAMERA:
      case ContentSettingsType::CAMERA_PAN_TILT_ZOOM:
        return has_camera_setting_ &&
               g_settings_get_boolean(privacy_settings_, kCameraDisabledKey);
      case ContentSettingsType::MEDIASTREAM_MIC:
        return has_microphone_setting_ &&
               g_settings_get_boolean(privacy_settings_,
                                      kMicrophoneDisabledKey);
      default:
        return false;
    }
  }

  bool IsAllowed(ContentSettingsType type) override {
    switch (type) {
      case ContentSettingsType::GEOLOCATION:
      case ContentSettingsType::MEDIASTREAM_CAMERA:
      case ContentSettingsType::CAMERA_PAN_TILT_ZOOM:
      case ContentSettingsType::MEDIASTREAM_MIC:
        return !IsDenied(type);
      default:
        return true;
    }
  }

  void IsDeniedFresh(ContentSettingsType type,
                     SystemPermissionDeniedCallback callback) override {
    std::move(callback).Run(IsDenied(type));
  }

  void OpenSystemSettings(content::WebContents*,
                          ContentSettingsType type) override {
    if (!HasSystemSettingForType(type)) {
      return;
    }
    base::LaunchProcess({"gnome-control-center", "privacy"},
                        base::LaunchOptions());
  }

  void Request(ContentSettingsType, SystemPermissionResponseCallback) override {
    // GNOME exposes global privacy switches rather than a per-application
    // prompt. The user can change them from system settings.
  }

  std::unique_ptr<ScopedObservation> Observe(
      SystemPermissionChangedCallback callback) override {
    auto observation = std::make_unique<LinuxSettingsObservation>(
        location_settings_, privacy_settings_, std::move(callback));
    if (!observation->is_observing()) {
      return nullptr;
    }
    return observation;
  }

 private:
  bool HasSystemSettingForType(ContentSettingsType type) const {
    switch (type) {
      case ContentSettingsType::GEOLOCATION:
        return has_location_setting_;
      case ContentSettingsType::MEDIASTREAM_CAMERA:
      case ContentSettingsType::CAMERA_PAN_TILT_ZOOM:
        return has_camera_setting_;
      case ContentSettingsType::MEDIASTREAM_MIC:
        return has_microphone_setting_;
      default:
        return false;
    }
  }

  ScopedGObject<GSettings> location_settings_;
  ScopedGObject<GSettings> privacy_settings_;
  const bool has_location_setting_;
  const bool has_camera_setting_;
  const bool has_microphone_setting_;
};

}  // namespace

std::unique_ptr<PlatformHandle> PlatformHandle::Create() {
  return std::make_unique<PlatformHandleImpl>();
}

}  // namespace system_permission_settings
