// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/barrier_callback.h"
#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/trace_event/trace_config.h"
#include "base/unguessable_token.h"
#include "base/uuid.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/agent/browser_agent_console_observer.h"
#include "chrome/browser/agent/browser_agent_mcp_server.h"
#include "chrome/browser/agent/browser_agent_mcp_server_protocol.h"
#include "chrome/browser/agent/browser_agent_protocol.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/javascript_dialogs/app_modal_dialog_controller.h"
#include "components/javascript_dialogs/app_modal_dialog_queue.h"
#include "components/javascript_dialogs/tab_modal_dialog_manager.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/version_info.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/browser_automation.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/dom_storage_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_usage_info.h"
#include "content/public/browser/tracing_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/referrer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/base/base_window.h"
#include "ui/base/page_transition_types.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"

namespace browser_agent {

bool BrowserAgentMcpServer::HandleBrowserContextToolCall(
    const base::Value* id,
    const protocol::Request& tool_request,
    base::DictValue& tool_result,
    base::DictValue& result,
    std::string& result_text,
    std::string* early_response) {
  if (tool_request.tool == "browser.version") {
    tool_result.Set("product", std::string(version_info::GetProductName()));
    tool_result.Set("version", std::string(version_info::GetVersionNumber()));
    tool_result.Set("mcp_protocol_version", protocol::kMcpProtocolVersion);
    result_text = base::StrCat({*tool_result.FindString("product"), " ",
                                *tool_result.FindString("version")});
  } else if (tool_request.tool == "browser.list_contexts") {
    base::ListValue contexts;
    base::DictValue regular_context;
    regular_context.Set("context_id", default_context_id_);
    regular_context.Set("off_the_record", false);
    contexts.Append(std::move(regular_context));
    for (Profile* off_the_record : profile_->GetAllOffTheRecordProfiles()) {
      if (destroying_context_profiles_.contains(off_the_record)) {
        continue;
      }
      base::DictValue context;
      context.Set("context_id", RegisterContext(off_the_record));
      context.Set("off_the_record", true);
      contexts.Append(std::move(context));
    }
    tool_result.Set("contexts", std::move(contexts));
    result_text = "Listed browser contexts.";
  } else if (tool_request.tool == "browser.create_context") {
    const Profile::OTRProfileID otr_id =
        Profile::OTRProfileID::CreateUniqueForDevTools();
    if (!otr_id.AllowsBrowserWindows()) {
      tool_result.Set(
          "error", "Chromium cannot create a window-capable isolated context.");
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
      *early_response = MakeToolCallResponse(
          id->Clone(), std::move(tool_result), std::move(result_text), true);
      return true;
    }
    Profile* off_the_record =
        profile_->GetOffTheRecordProfile(otr_id, /*create_if_needed=*/true);
    if (!off_the_record) {
      tool_result.Set("error",
                      "Chromium failed to create an isolated context.");
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
      *early_response = MakeToolCallResponse(
          id->Clone(), std::move(tool_result), std::move(result_text), true);
      return true;
    }
    const std::string context_id = RegisterContext(off_the_record);
    owned_context_ids_.insert(context_id);
    tool_result.Set("context_id", context_id);
    tool_result.Set("off_the_record", true);
    result_text = "Created a private browser context.";
  } else if (tool_request.tool == "browser.close_context") {
    const std::string* context_id =
        tool_request.arguments.FindString("context_id");
    Profile* target = context_id ? ResolveContext(*context_id) : nullptr;
    if (!target || target == profile_ || !context_id ||
        !owned_context_ids_.contains(*context_id)) {
      tool_result.Set("error", "A live owned private context_id is required.");
    } else {
      ProfileBrowserCollection* context_browsers =
          ProfileBrowserCollection::GetForProfile(target);
      std::vector<BrowserWindowInterface*> open_browsers;
      if (context_browsers) {
        context_browsers->ForEach([&](BrowserWindowInterface* browser) {
          open_browsers.push_back(browser);
          return true;
        });
      }
      for (BrowserWindowInterface* browser : open_browsers) {
        browser->GetWindow()->Close();
      }
      pending_context_destructions_.insert_or_assign(
          *context_id, base::TimeTicks::Now() + base::Seconds(60));
      MaybeDestroyOwnedContext(*context_id, target->GetWeakPtr());
      tool_result.Set("context_id", *context_id);
      const bool closed = !owned_context_ids_.contains(*context_id);
      tool_result.Set("closed", closed);
      tool_result.Set("close_requested", !closed);
      result_text = closed ? "Closed private browser context."
                           : "Requested private browser context closure.";
    }
    if (result_text.empty()) {
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
    }
  } else if (tool_request.tool == "context.downloads") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    Profile* context = ResolveContext(context_id);
    content::DownloadManager* manager =
        context ? context->GetDownloadManager() : nullptr;
    if (!manager) {
      tool_result.Set("error", "Unknown or closed context_id.");
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
    } else {
      download::SimpleDownloadManager::DownloadVector downloads;
      manager->GetAllDownloads(&downloads);
      base::ListValue items;
      for (download::DownloadItem* download : downloads) {
        base::DictValue item;
        item.Set("guid", download->GetGuid());
        item.Set("url", download->GetURL().spec());
        item.Set("target_path", download->GetTargetFilePath().AsUTF8Unsafe());
        item.Set("received_bytes",
                 static_cast<double>(download->GetReceivedBytes()));
        item.Set("total_bytes", static_cast<double>(download->GetTotalBytes()));
        item.Set("state", DownloadStateName(download->GetState()));
        items.Append(std::move(item));
      }
      tool_result.Set("context_id", context_id);
      tool_result.Set("downloads", std::move(items));
      result_text = "Listed context downloads.";
    }
  } else if (tool_request.tool == "context.list_permissions") {
    base::ListValue permissions;
    for (const content_settings::ContentSettingsInfo* info :
         *content_settings::ContentSettingsRegistry::GetInstance()) {
      base::DictValue item;
      item.Set("name", info->website_settings_info()->name());
      base::ListValue settings;
      for (ContentSetting candidate :
           {CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK, CONTENT_SETTING_ASK,
            CONTENT_SETTING_SESSION_ONLY}) {
        if (info->IsSettingValid(candidate)) {
          settings.Append(PermissionSettingName(candidate));
        }
      }
      item.Set("settings", std::move(settings));
      permissions.Append(std::move(item));
    }
    tool_result.Set("permissions", std::move(permissions));
    result_text = "Listed native content settings.";
  } else if (tool_request.tool == "context.permission" ||
             tool_request.tool == "context.set_permission") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    const std::string* url_text = tool_request.arguments.FindString("url");
    const std::string* permission_name =
        tool_request.arguments.FindString("permission");
    const std::string* setting_name =
        tool_request.arguments.FindString("setting");
    Profile* context = ResolveContext(context_id);
    const GURL url = url_text ? GURL(*url_text) : GURL();
    const std::optional<ContentSettingsType> type =
        permission_name ? PermissionType(*permission_name) : std::nullopt;
    const bool set_permission = tool_request.tool == "context.set_permission";
    const std::optional<ContentSetting> desired_setting =
        setting_name ? PermissionSetting(*setting_name) : std::nullopt;
    const content_settings::ContentSettingsInfo* setting_info =
        type ? content_settings::ContentSettingsRegistry::GetInstance()->Get(
                   *type)
             : nullptr;
    if (!context || !url.is_valid() || !type ||
        (set_permission && (!desired_setting || !setting_info ||
                            !setting_info->IsSettingValid(*desired_setting)))) {
      tool_result.Set(
          "error",
          "A live context, URL, permission, and valid setting are required.");
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
    } else {
      HostContentSettingsMap* settings_map =
          HostContentSettingsMapFactory::GetForProfile(context);
      if (set_permission) {
        settings_map->SetContentSettingDefaultScope(url, url, *type,
                                                    *desired_setting);
      }
      tool_result.Set("context_id", context_id);
      tool_result.Set("url", url.spec());
      tool_result.Set("permission", *permission_name);
      tool_result.Set("setting",
                      PermissionSettingName(
                          settings_map->GetContentSetting(url, url, *type)));
      result_text = set_permission ? "Set native site permission."
                                   : "Read native site permission.";
    }
  } else if (tool_request.tool == "download.cancel") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    const std::string* guid = tool_request.arguments.FindString("guid");
    Profile* context = ResolveContext(context_id);
    content::DownloadManager* manager =
        context ? context->GetDownloadManager() : nullptr;
    download::DownloadItem* item =
        manager && guid ? manager->GetDownloadByGuid(*guid) : nullptr;
    if (!item) {
      tool_result.Set("error",
                      "A live context_id and download guid are required.");
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
    } else {
      item->Cancel(/*user_cancel=*/true);
      tool_result.Set("guid", *guid);
      tool_result.Set("cancel_requested", true);
      tool_result.Set("cancelled",
                      item->GetState() == download::DownloadItem::CANCELLED);
      result_text = "Requested native download cancellation.";
    }
  } else {
    return false;
  }
  return true;
}

}  // namespace browser_agent
