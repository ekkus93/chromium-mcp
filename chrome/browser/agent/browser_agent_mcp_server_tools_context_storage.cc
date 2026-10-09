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

bool BrowserAgentMcpServer::HandleContextStorageTools(
    const base::DictValue& request,
    int connection_id,
    base::OnceCallback<void(std::string)>& reply) {
  const std::string* method = request.FindString("method");
  protocol::Request tool_request;
  protocol::Error parse_error;
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "context.clear_http_cache") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    Profile* context = ResolveContext(context_id);
    if (!context) {
      base::DictValue error;
      error.Set("error", "Unknown or closed context_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed context_id.", true));
      return true;
    }
    context->GetDefaultStoragePartition()->GetNetworkContext()->ClearHttpCache(
        base::Time(), base::Time(), nullptr,
        base::BindOnce(
            [](base::Value request_id, std::string context_id,
               base::OnceCallback<void(std::string)> reply) {
              base::DictValue structured;
              structured.Set("context_id", std::move(context_id));
              structured.Set("cleared", true);
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Cleared context HTTP cache.", false));
            },
            std::move(tool_request.request_id), context_id, std::move(reply)));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "context.clear_local_storage") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    const std::string* serialized_key =
        tool_request.arguments.FindString("storage_key");
    Profile* context = ResolveContext(context_id);
    std::optional<blink::StorageKey> storage_key =
        serialized_key ? blink::StorageKey::Deserialize(*serialized_key)
                       : std::nullopt;
    if (!context || !storage_key) {
      base::DictValue error;
      error.Set("error", "A live context and valid storage_key are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live context and valid storage_key are required.", true));
      return true;
    }
    context->GetDefaultStoragePartition()
        ->GetDOMStorageContext()
        ->DeleteLocalStorage(
            *storage_key,
            base::BindOnce(
                [](base::Value request_id, std::string context_id,
                   std::string serialized_key,
                   base::OnceCallback<void(std::string)> reply) {
                  base::DictValue structured;
                  structured.Set("context_id", std::move(context_id));
                  structured.Set("storage_key", std::move(serialized_key));
                  structured.Set("cleared", true);
                  std::move(reply).Run(MakeToolCallResponse(
                      std::move(request_id), std::move(structured),
                      "Cleared native local storage for the storage key.",
                      false));
                },
                std::move(tool_request.request_id), context_id, *serialized_key,
                std::move(reply)));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "context.local_storage_origins") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    Profile* context = ResolveContext(context_id);
    if (!context) {
      base::DictValue error;
      error.Set("error", "Unknown or closed context_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed context_id.", true));
      return true;
    }
    context->GetDefaultStoragePartition()
        ->GetDOMStorageContext()
        ->GetLocalStorageUsage(base::BindOnce(
            [](base::Value request_id,
               base::OnceCallback<void(std::string)> reply,
               const std::vector<content::StorageUsageInfo>& usage) {
              base::ListValue origins;
              for (const content::StorageUsageInfo& info : usage) {
                base::DictValue item;
                item.Set("origin", info.storage_key.origin().Serialize());
                item.Set("storage_key", info.storage_key.Serialize());
                item.Set("size_bytes",
                         static_cast<double>(info.total_size_bytes));
                origins.Append(std::move(item));
              }
              base::DictValue structured;
              structured.Set("origins", std::move(origins));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Listed context local storage origins.", false));
            },
            std::move(tool_request.request_id), std::move(reply)));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "context.storage_state") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    Profile* context = ResolveContext(context_id);
    if (!context) {
      base::DictValue error;
      error.Set("error", "Unknown or closed context_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed context_id.", true));
      return true;
    }
    content::StoragePartition* partition =
        context->GetDefaultStoragePartition();
    partition->GetCookieManagerForBrowserProcess()->GetAllCookies(
        base::BindOnce(
            [](base::Value request_id,
               base::OnceCallback<void(std::string)> reply,
               const std::vector<net::CanonicalCookie>& cookies) {
              base::DictValue structured;
              base::ListValue serialized_cookies;
              for (const net::CanonicalCookie& cookie : cookies) {
                base::DictValue item;
                item.Set("name", cookie.Name());
                item.Set("value", cookie.Value());
                item.Set("domain", cookie.Domain());
                item.Set("path", cookie.Path());
                item.Set("secure", cookie.IsSecure());
                item.Set("http_only", cookie.IsHttpOnly());
                item.Set("same_site", static_cast<int>(cookie.SameSite()));
                if (cookie.IsPersistent()) {
                  item.Set(
                      "expires_unix_ms",
                      static_cast<double>(
                          cookie.ExpiryDate().InMillisecondsSinceUnixEpoch()));
                }
                serialized_cookies.Append(std::move(item));
              }
              structured.Set("cookies", std::move(serialized_cookies));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Read context cookie storage state.", false));
            },
            std::move(tool_request.request_id), std::move(reply)));
    return true;
  }
  return false;
}

}  // namespace browser_agent
