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

bool BrowserAgentMcpServer::HandlePageSetupTools(
    const base::DictValue& request,
    int connection_id,
    base::OnceCallback<void(std::string)>& reply) {
  const std::string* method = request.FindString("method");
  protocol::Request tool_request;
  protocol::Error parse_error;
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.permission_status") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* permission =
        tool_request.arguments.FindString("permission");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    std::optional<blink::mojom::PermissionName> name;
    bool has_device_status = false;
    if (permission) {
      if (*permission == "geolocation") {
        name = blink::mojom::PermissionName::GEOLOCATION;
        has_device_status = true;
      } else if (*permission == "camera") {
        name = blink::mojom::PermissionName::VIDEO_CAPTURE;
        has_device_status = true;
      } else if (*permission == "microphone") {
        name = blink::mojom::PermissionName::AUDIO_CAPTURE;
        has_device_status = true;
      } else if (*permission == "notifications") {
        name = blink::mojom::PermissionName::NOTIFICATIONS;
      }
    }
    if (!frame || !name) {
      base::DictValue error;
      error.Set("error",
                "A live page_id and supported permission are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id and supported permission are required.", true));
      return true;
    }
    content::PermissionController* controller =
        tab->GetContents()->GetBrowserContext()->GetPermissionController();
    auto descriptor = blink::mojom::PermissionDescriptor::New();
    descriptor->name = *name;
    const blink::mojom::PermissionStatus browser_status =
        controller->GetPermissionStatusForCurrentDocument(descriptor, frame);
    const blink::mojom::PermissionStatus combined_status =
        has_device_status ? controller->GetCombinedPermissionAndDeviceStatus(
                                descriptor, frame)
                          : browser_status;
    base::DictValue structured;
    structured.Set("page_id", *page_id);
    structured.Set("permission", *permission);
    structured.Set("browser_status", PermissionStatusName(browser_status));
    structured.Set("combined_status", PermissionStatusName(combined_status));
    structured.Set("has_device_status", has_device_status);
    structured.Set(
        "os_or_device_denied",
        browser_status == blink::mojom::PermissionStatus::GRANTED &&
            combined_status == blink::mojom::PermissionStatus::DENIED);
    std::move(reply).Run(MakeToolCallResponse(
        std::move(tool_request.request_id), std::move(structured),
        "Read browser and device permission status.", false));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.prepare_file_upload") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const base::ListValue* inputs = tool_request.arguments.FindList("paths");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    std::vector<base::FilePath> paths;
    bool valid = inputs && !inputs->empty();
    if (valid) {
      for (const base::Value& input : *inputs) {
        if (!input.is_string()) {
          valid = false;
          break;
        }
        base::FilePath path = base::FilePath::FromUTF8Unsafe(input.GetString());
        if (!path.IsAbsolute()) {
          valid = false;
          break;
        }
        paths.push_back(std::move(path));
      }
    }
    if (!frame || !valid) {
      base::DictValue error;
      error.Set("error",
                "A live page_id and absolute file paths are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id and absolute file paths are required.", true));
      return true;
    }
    const size_t file_count = paths.size();
    content::SetFilesForNextFileChooserForAutomation(frame, std::move(paths));
    base::DictValue structured;
    structured.Set("page_id", *page_id);
    structured.Set("file_count", static_cast<int>(file_count));
    structured.Set("ready", true);
    std::move(reply).Run(MakeToolCallResponse(
        std::move(tool_request.request_id), std::move(structured),
        "Prepared files for the next native file input chooser.", false));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      (tool_request.tool == "page.storage_state" ||
       tool_request.tool == "page.set_storage")) {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    const std::string* requested_frame_id =
        tool_request.arguments.FindString("frame_id");
    content::RenderFrameHost* frame = nullptr;
    if (page_id && requested_frame_id) {
      auto frame_handle = frame_handles_.find(*requested_frame_id);
      if (frame_handle != frame_handles_.end() &&
          frame_handle->second.first == *page_id) {
        frame = content::RenderFrameHost::FromID(frame_handle->second.second);
      }
    } else if (tab && tab->GetContents()) {
      frame = tab->GetContents()->GetPrimaryMainFrame();
    }
    const bool write = tool_request.tool == "page.set_storage";
    if (!page_id || !frame) {
      base::DictValue error;
      error.Set("error", "Unknown or closed page_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed page_id.", true));
      return true;
    }
    if (write) {
      for (const char* area : {"local", "session"}) {
        const base::Value* input = tool_request.arguments.Find(area);
        if (!input) {
          continue;
        }
        bool valid = input->is_dict();
        if (valid) {
          for (auto entry : input->GetDict()) {
            valid &= entry.second.is_string();
          }
        }
        if (!valid) {
          base::DictValue error;
          error.Set("error", "Storage entries must be string values.");
          std::move(reply).Run(MakeToolCallResponse(
              std::move(tool_request.request_id), std::move(error),
              "Storage entries must be string values.", true));
          return true;
        }
      }
    }
    const std::string arguments_json =
        base::WriteJson(base::Value(tool_request.arguments.Clone()))
            .value_or("{}");
    const std::string script = base::StrCat({R"JS((() => {
      try {
        const input = )JS",
                                             arguments_json, R"JS(;
        if (input.local) for (const [k, v] of Object.entries(input.local))
          localStorage.setItem(k, v);
        if (input.session) for (const [k, v] of Object.entries(input.session))
          sessionStorage.setItem(k, v);
        const read = store => Object.fromEntries(Array.from(
          {length: store.length}, (_, i) => [store.key(i), store.getItem(store.key(i))]));
        return {origin: location.origin, local: read(localStorage),
          session: read(sessionStorage)};
      } catch (e) { return {error: String(e)}; }
    })())JS"});
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(script),
        base::BindOnce(
            [](base::Value request_id, std::string page_id, bool write,
               base::OnceCallback<void(std::string)> reply,
               base::Value evaluation) {
              const base::DictValue* data = evaluation.GetIfDict();
              if (!data || data->FindString("error")) {
                const std::string error = data && data->FindString("error")
                                              ? *data->FindString("error")
                                              : "Storage is not available.";
                base::DictValue structured;
                structured.Set("error", error);
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured), error, true));
                return;
              }
              base::DictValue structured = data->Clone();
              structured.Set("page_id", std::move(page_id));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  write ? "Set page origin storage."
                        : "Read page origin storage.",
                  false));
            },
            std::move(tool_request.request_id), *page_id, write,
            std::move(reply)));
    return true;
  }
  return false;
}

}  // namespace browser_agent
