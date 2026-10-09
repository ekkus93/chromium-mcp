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

bool BrowserAgentMcpServer::HandleContextNetworkAndTracingTools(
    const base::DictValue& request,
    int connection_id,
    base::OnceCallback<void(std::string)>& reply) {
  const std::string* method = request.FindString("method");
  protocol::Request tool_request;
  protocol::Error parse_error;
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      (tool_request.tool == "context.set_accept_language" ||
       tool_request.tool == "context.set_referrers_enabled" ||
       tool_request.tool == "context.close_idle_connections")) {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    const std::string context_id =
        requested_context_id ? *requested_context_id : default_context_id_;
    Profile* context = ResolveContext(context_id);
    network::mojom::NetworkContext* network_context =
        context ? context->GetDefaultStoragePartition()->GetNetworkContext()
                : nullptr;
    base::DictValue structured;
    structured.Set("context_id", context_id);
    if (!network_context) {
      structured.Set("error", "Unknown or closed context_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(structured),
          "Unknown or closed context_id.", true));
      return true;
    }
    if (tool_request.tool == "context.set_accept_language") {
      const std::string* language =
          tool_request.arguments.FindString("accept_language");
      if (!language || !net::HttpUtil::IsValidHeaderValue(*language)) {
        structured.Set("error", "A valid accept_language value is required.");
        std::move(reply).Run(MakeToolCallResponse(
            std::move(tool_request.request_id), std::move(structured),
            "A valid accept_language value is required.", true));
        return true;
      }
      context->GetPrefs()->SetString(language::prefs::kAcceptLanguages,
                                     *language);
      network_context->SetAcceptLanguage(*language);
      context_accept_languages_.insert_or_assign(context_id, *language);
      for (const auto& [page_id, page_context_id] : page_context_ids_) {
        if (page_context_id != context_id) {
          continue;
        }
        auto page_handle = page_handles_.find(page_id);
        tabs::TabInterface* page_tab = page_handle == page_handles_.end()
                                           ? nullptr
                                           : page_handle->second.Get();
        if (!page_tab || !page_tab->GetContents()) {
          continue;
        }
        auto& observer = console_observers_[page_id];
        if (!observer) {
          observer = std::make_unique<BrowserAgentConsoleObserver>(
              page_tab->GetContents(), *language);
        } else {
          observer->SetAcceptLanguage(*language);
        }
      }
      if (ProfileBrowserCollection* browsers =
              ProfileBrowserCollection::GetForProfile(context)) {
        browsers->ForEach([&](BrowserWindowInterface* browser) {
          TabStripModel* tabs = browser->GetTabStripModel();
          for (int i = 0; i < tabs->count(); ++i) {
            content::WebContents* contents = tabs->GetWebContentsAt(i);
            contents->GetMutableRendererPrefs()->accept_languages = *language;
            contents->SyncRendererPrefs();
          }
          return true;
        });
      }
      structured.Set("accept_language", *language);
    } else if (tool_request.tool == "context.set_referrers_enabled") {
      const std::optional<bool> enabled =
          tool_request.arguments.FindBool("enabled");
      if (!enabled) {
        structured.Set("error", "The enabled boolean is required.");
        std::move(reply).Run(MakeToolCallResponse(
            std::move(tool_request.request_id), std::move(structured),
            "The enabled boolean is required.", true));
        return true;
      }
      network_context->SetEnableReferrers(*enabled);
      structured.Set("enabled", *enabled);
    } else {
      network_context->CloseIdleConnections(base::BindOnce([] {}));
      structured.Set("closed", true);
    }
    std::move(reply).Run(MakeToolCallResponse(
        std::move(tool_request.request_id), std::move(structured),
        "Applied native context network control.", false));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "browser.start_trace") {
    content::TracingController* controller =
        content::TracingController::GetInstance();
    if (trace_directory_.empty() || controller->IsTracing()) {
      const std::string error = trace_directory_.empty()
                                    ? "Tracing output is not configured."
                                    : "Chromium tracing is already active.";
      base::DictValue structured;
      structured.Set("error", error);
      std::move(reply).Run(
          MakeToolCallResponse(std::move(tool_request.request_id),
                               std::move(structured), error, true));
      return true;
    }
    const std::string* categories =
        tool_request.arguments.FindString("categories");
    const base::trace_event::TraceConfig trace_config(
        categories ? *categories : "*", "record-until-full");
    if (!controller->StartTracing(trace_config, base::BindOnce([]() {}))) {
      base::DictValue error;
      error.Set("error", "Chromium could not start tracing.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Chromium could not start tracing.", true));
      return true;
    }
    base::DictValue structured;
    structured.Set("started", true);
    std::move(reply).Run(MakeToolCallResponse(
        std::move(tool_request.request_id), std::move(structured),
        "Started native Chromium tracing.", false));
    return true;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "browser.stop_trace") {
    content::TracingController* controller =
        content::TracingController::GetInstance();
    if (trace_directory_.empty() || !controller->IsTracing()) {
      const std::string error = trace_directory_.empty()
                                    ? "Tracing output is not configured."
                                    : "Chromium tracing is not active.";
      base::DictValue structured;
      structured.Set("error", error);
      std::move(reply).Run(
          MakeToolCallResponse(std::move(tool_request.request_id),
                               std::move(structured), error, true));
      return true;
    }
    const base::FilePath trace_path = trace_directory_.AppendASCII(
        base::Uuid::GenerateRandomV4().AsLowercaseString() + ".json");
    auto stop_reply = std::make_shared<base::OnceCallback<void(std::string)>>(
        std::move(reply));
    auto endpoint = content::TracingController::CreateFileEndpoint(
        trace_path,
        base::BindOnce(
            [](base::Value request_id, base::FilePath trace_path,
               std::shared_ptr<base::OnceCallback<void(std::string)>> reply) {
              if (!*reply) {
                return;
              }
              base::DictValue structured;
              structured.Set("path", trace_path.AsUTF8Unsafe());
              structured.Set("complete", true);
              std::move(*reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Wrote Chromium trace JSON.", false));
            },
            tool_request.request_id.Clone(), trace_path, stop_reply));
    if (!controller->StopTracing(endpoint) && *stop_reply) {
      base::DictValue error;
      error.Set("error", "Chromium could not stop tracing.");
      std::move(*stop_reply)
          .Run(MakeToolCallResponse(std::move(tool_request.request_id),
                                    std::move(error),
                                    "Chromium could not stop tracing.", true));
    }
    return true;
  }
  return false;
}

}  // namespace browser_agent
