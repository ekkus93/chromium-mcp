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

namespace {

const net::NetworkTrafficAnnotationTag& GetDownloadTrafficAnnotation() {
  static const net::NetworkTrafficAnnotationTag annotation =
      net::DefineNetworkTrafficAnnotation("chromium_embedded_mcp_download", R"(
        semantics {
          sender: "Chromium Embedded MCP Download Tool"
          description: "Downloads a URL selected by the authenticated local MCP client."
          trigger: "The local client invokes page.download."
          data: "The requested URL and normal browser request headers."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting: "The service requires an explicit feature and command-line opt-in."
          policy_exception_justification: "This is a user-enabled local browser control feature."
        })");
  return annotation;
}

class LoadStateWaiter : public content::WebContentsObserver,
                        public base::RefCounted<LoadStateWaiter> {
 public:
  LoadStateWaiter(content::WebContents* contents,
                  base::Value request_id,
                  std::string page_id,
                  base::OnceCallback<void(std::string)> reply)
      : content::WebContentsObserver(contents),
        request_id_(std::move(request_id)),
        page_id_(std::move(page_id)),
        reply_(std::move(reply)) {}

  void Start() {
    if (!web_contents()->IsLoading()) {
      Finish(/*success=*/true);
      return;
    }
    timeout_.Start(FROM_HERE, base::Seconds(30),
                   base::BindOnce(&LoadStateWaiter::OnTimeout,
                                  scoped_refptr<LoadStateWaiter>(this)));
  }

  void DidStopLoading() override { Finish(/*success=*/true); }

  void WebContentsDestroyed() override { Finish(/*success=*/false); }

  void Cancel() { Finish(/*success=*/false); }

 private:
  friend class base::RefCounted<LoadStateWaiter>;
  ~LoadStateWaiter() override = default;

  void OnTimeout() { Finish(/*success=*/false); }

  void Finish(bool success) {
    if (!reply_) {
      return;
    }
    scoped_refptr<LoadStateWaiter> keep_alive(this);
    timeout_.Stop();
    Observe(nullptr);
    base::DictValue structured;
    structured.Set("page_id", page_id_);
    if (success) {
      structured.Set("load_state", "load");
    } else {
      structured.Set("error", "Page load wait timed out or page closed.");
    }
    std::move(reply_).Run(MakeToolCallResponse(
        std::move(request_id_), std::move(structured),
        success ? "Page finished loading." : "Page load wait failed.",
        !success));
  }

  base::Value request_id_;
  std::string page_id_;
  base::OnceCallback<void(std::string)> reply_;
  base::OneShotTimer timeout_;
};

class URLWaiter : public content::WebContentsObserver,
                  public base::RefCounted<URLWaiter> {
 public:
  URLWaiter(content::WebContents* contents,
            base::Value request_id,
            std::string page_id,
            GURL target_url,
            base::OnceCallback<void(std::string)> reply)
      : content::WebContentsObserver(contents),
        request_id_(std::move(request_id)),
        page_id_(std::move(page_id)),
        target_url_(std::move(target_url)),
        reply_(std::move(reply)) {}

  void Start() {
    if (web_contents()->GetLastCommittedURL() == target_url_) {
      Finish(/*success=*/true);
      return;
    }
    timeout_.Start(
        FROM_HERE, base::Seconds(30),
        base::BindOnce(&URLWaiter::OnTimeout, scoped_refptr<URLWaiter>(this)));
  }

  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted() &&
        navigation->GetURL() == target_url_) {
      Finish(/*success=*/true);
    }
  }

  void WebContentsDestroyed() override { Finish(/*success=*/false); }

  void Cancel() { Finish(/*success=*/false); }

 private:
  friend class base::RefCounted<URLWaiter>;
  ~URLWaiter() override = default;

  void OnTimeout() { Finish(/*success=*/false); }

  void Finish(bool success) {
    if (!reply_) {
      return;
    }
    scoped_refptr<URLWaiter> keep_alive(this);
    timeout_.Stop();
    Observe(nullptr);
    base::DictValue structured;
    structured.Set("page_id", page_id_);
    structured.Set("url", target_url_.spec());
    if (!success) {
      structured.Set("error", "URL wait timed out or page closed.");
    }
    std::move(reply_).Run(MakeToolCallResponse(
        std::move(request_id_), std::move(structured),
        success ? "Page reached the requested URL." : "Page URL wait failed.",
        !success));
  }

  base::Value request_id_;
  std::string page_id_;
  GURL target_url_;
  base::OnceCallback<void(std::string)> reply_;
  base::OneShotTimer timeout_;
};

class DownloadWaiter : public download::DownloadItem::Observer,
                       public base::RefCounted<DownloadWaiter> {
 public:
  DownloadWaiter(download::DownloadItem* item,
                 base::Value request_id,
                 base::OnceCallback<void(std::string)> reply)
      : item_(item),
        request_id_(std::move(request_id)),
        reply_(std::move(reply)) {}

  void Start() {
    item_->AddObserver(this);
    if (item_->GetState() != download::DownloadItem::IN_PROGRESS) {
      Finish(/*terminal=*/true);
      return;
    }
    timeout_.Start(FROM_HERE, base::Seconds(30),
                   base::BindOnce(&DownloadWaiter::OnTimeout,
                                  scoped_refptr<DownloadWaiter>(this)));
  }

  void OnDownloadUpdated(download::DownloadItem* item) override {
    if (item->GetState() != download::DownloadItem::IN_PROGRESS) {
      Finish(/*terminal=*/true);
    }
  }

  void OnDownloadDestroyed(download::DownloadItem* item) override {
    item_ = nullptr;
    Finish(/*terminal=*/false);
  }

  void Cancel() { Finish(/*terminal=*/false); }

 private:
  friend class base::RefCounted<DownloadWaiter>;
  ~DownloadWaiter() override = default;

  void OnTimeout() { Finish(/*terminal=*/false); }

  void Finish(bool terminal) {
    if (!reply_) {
      return;
    }
    scoped_refptr<DownloadWaiter> keep_alive(this);
    timeout_.Stop();
    base::DictValue structured;
    if (item_) {
      structured.Set("guid", item_->GetGuid());
      structured.Set("state", DownloadStateName(item_->GetState()));
      item_->RemoveObserver(this);
      item_ = nullptr;
    }
    if (!terminal) {
      structured.Set("error", "Download wait timed out or was cancelled.");
    }
    std::move(reply_).Run(MakeToolCallResponse(
        std::move(request_id_), std::move(structured),
        terminal ? "Download reached a terminal state."
                 : "Download wait ended before a terminal state.",
        !terminal));
  }

  raw_ptr<download::DownloadItem> item_;
  base::Value request_id_;
  base::OnceCallback<void(std::string)> reply_;
  base::OneShotTimer timeout_;
};

}  // namespace

void BrowserAgentMcpServer::HandleRequestOnUiThread(
    base::DictValue request,
    int connection_id,
    base::OnceCallback<void(std::string)> reply) {
  auto cancellation = std::make_shared<protocol::CancellationSource>();
  pending_requests_[connection_id] = PendingRequest{cancellation, {}};
  reply =
      base::BindOnce(&BrowserAgentMcpServer::CompleteRequestOnUiThread,
                     weak_this_, connection_id, cancellation, std::move(reply));
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
      return;
    }
    if (tool_request.tool == "context.set_accept_language") {
      const std::string* language =
          tool_request.arguments.FindString("accept_language");
      if (!language || !net::HttpUtil::IsValidHeaderValue(*language)) {
        structured.Set("error", "A valid accept_language value is required.");
        std::move(reply).Run(MakeToolCallResponse(
            std::move(tool_request.request_id), std::move(structured),
            "A valid accept_language value is required.", true));
        return;
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
        return;
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
    return;
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
      return;
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
      return;
    }
    base::DictValue structured;
    structured.Set("started", true);
    std::move(reply).Run(MakeToolCallResponse(
        std::move(tool_request.request_id), std::move(structured),
        "Started native Chromium tracing.", false));
    return;
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
      return;
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
    return;
  }
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
      return;
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
    return;
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
      return;
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
    return;
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
      return;
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
          return;
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
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.find_elements") {
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
    const std::string* selector = tool_request.arguments.FindString("selector");
    const std::string* role = tool_request.arguments.FindString("role");
    const std::string* name = tool_request.arguments.FindString("name");
    const std::string* text_query = tool_request.arguments.FindString("text");
    if (!page_id || !frame || (!selector && !role && !name && !text_query)) {
      base::DictValue error;
      error.Set("error",
                "A live page_id and at least one locator are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id and at least one locator are required.", true));
      return;
    }
    base::DictValue query;
    if (selector) {
      query.Set("selector", *selector);
    }
    if (role) {
      query.Set("role", *role);
    }
    if (name) {
      query.Set("name", *name);
    }
    if (text_query) {
      query.Set("text", *text_query);
    }
    const std::string query_json =
        base::WriteJson(base::Value(std::move(query))).value_or("{}");
    const std::string script = base::StrCat({R"JS((() => {
      const q = )JS",
                                             query_json, R"JS(;
      const roleOf = e => {
        const explicit = e.getAttribute('role');
        if (explicit) return explicit;
        if (e.matches('button,input[type=button],input[type=submit]')) return 'button';
        if (e.matches('a[href]')) return 'link';
        if (e.matches('input[type=checkbox]')) return 'checkbox';
        if (e.matches('input[type=radio]')) return 'radio';
        if (e.matches('input:not([type]),input[type=text],input[type=email],input[type=password],textarea')) return 'textbox';
        if (e.matches('select')) return 'combobox';
        if (e.matches('h1,h2,h3,h4,h5,h6')) return 'heading';
        if (e.matches('img')) return 'img';
        return '';
      };
      const nameOf = e => e.getAttribute('aria-label') ||
          (e.labels && Array.from(e.labels).map(l => l.innerText).join(' ')) ||
          e.getAttribute('alt') || (e.innerText || '').trim() ||
          e.getAttribute('title') || '';
      try {
        return Array.from(document.querySelectorAll(q.selector || '*'))
            .filter(e => e.getClientRects().length &&
                getComputedStyle(e).visibility !== 'hidden')
            .map(e => {
              const r = e.getBoundingClientRect();
              return {tag: e.tagName.toLowerCase(), role: roleOf(e),
                name: nameOf(e), text: (e.innerText || '').trim(),
                x: r.x, y: r.y, width: r.width, height: r.height,
                center_x: r.x + r.width / 2, center_y: r.y + r.height / 2};
            })
            .filter(e => (!q.role || e.role === q.role) &&
                (!q.name || e.name.includes(q.name)) &&
                (!q.text || e.text.includes(q.text)));
      } catch (e) { return {error: String(e)}; }
    })())JS"});
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(script),
        base::BindOnce(
            [](base::Value request_id, std::string page_id,
               base::OnceCallback<void(std::string)> reply,
               base::Value evaluation) {
              if (evaluation.is_dict()) {
                const std::string* error =
                    evaluation.GetDict().FindString("error");
                if (error) {
                  base::DictValue structured;
                  structured.Set("error", *error);
                  std::move(reply).Run(MakeToolCallResponse(
                      std::move(request_id), std::move(structured), *error,
                      true));
                  return;
                }
              }
              if (!evaluation.is_list()) {
                base::DictValue structured;
                structured.Set("error", "Element query failed.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured),
                    "Element query failed.", true));
                return;
              }
              base::DictValue structured;
              structured.Set("page_id", std::move(page_id));
              structured.Set("elements", std::move(evaluation));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Found matching visible elements.", false));
            },
            std::move(tool_request.request_id), *page_id, std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "download.wait") {
    const std::string* context_id =
        tool_request.arguments.FindString("context_id");
    const std::string* guid = tool_request.arguments.FindString("guid");
    Profile* context =
        ResolveContext(context_id ? *context_id : default_context_id_);
    content::DownloadManager* manager =
        context ? context->GetDownloadManager() : nullptr;
    download::DownloadItem* item =
        manager && guid ? manager->GetDownloadByGuid(*guid) : nullptr;
    if (!item) {
      base::DictValue error;
      error.Set("error", "A live context_id and download guid are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live context_id and download guid are required.", true));
      return;
    }
    scoped_refptr<DownloadWaiter> waiter = base::MakeRefCounted<DownloadWaiter>(
        item, std::move(tool_request.request_id), std::move(reply));
    pending_requests_[connection_id].stop_waiting =
        base::BindOnce(&DownloadWaiter::Cancel, waiter);
    waiter->Start();
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.download") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* url_text = tool_request.arguments.FindString("url");
    const std::string* path_text = tool_request.arguments.FindString("path");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    const GURL url = url_text ? GURL(*url_text) : GURL();
    if (!page_id || !frame || !url.is_valid()) {
      base::DictValue error;
      error.Set("error", "A live page_id and valid url are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id and valid url are required.", true));
      return;
    }
    std::unique_ptr<download::DownloadUrlParameters> parameters =
        frame->CreateDownloadUrlParameters(url, GetDownloadTrafficAnnotation());
    if (path_text && !path_text->empty()) {
      parameters->set_file_path(base::FilePath::FromUTF8Unsafe(*path_text));
    }
    parameters->set_callback(base::BindOnce(
        [](base::Value request_id, std::string page_id,
           base::OnceCallback<void(std::string)> reply,
           download::DownloadItem* item,
           download::DownloadInterruptReason reason) {
          base::DictValue structured;
          if (!item) {
            structured.Set("error", "Chromium did not start the download.");
            structured.Set("interrupt_reason", static_cast<int>(reason));
            std::move(reply).Run(MakeToolCallResponse(
                std::move(request_id), std::move(structured),
                "Chromium did not start the download.", true));
            return;
          }
          structured.Set("page_id", std::move(page_id));
          structured.Set("download_guid", item->GetGuid());
          structured.Set("started", true);
          std::move(reply).Run(
              MakeToolCallResponse(std::move(request_id), std::move(structured),
                                   "Started native browser download.", false));
        },
        std::move(tool_request.request_id), *page_id, std::move(reply)));
    tab->GetContents()->GetBrowserContext()->GetDownloadManager()->DownloadUrl(
        std::move(parameters), nullptr);
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      (tool_request.tool == "page.console_messages" ||
       tool_request.tool == "page.network_events")) {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    if (!page_id || !tab || !tab->GetContents()) {
      base::DictValue error;
      error.Set("error", "Unknown or closed page_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed page_id.", true));
      return;
    }
    auto& observer = console_observers_[*page_id];
    if (!observer) {
      std::string accept_language;
      if (auto page_context = page_context_ids_.find(*page_id);
          page_context != page_context_ids_.end()) {
        if (auto language =
                context_accept_languages_.find(page_context->second);
            language != context_accept_languages_.end()) {
          accept_language = language->second;
        }
      }
      observer = std::make_unique<BrowserAgentConsoleObserver>(
          tab->GetContents(), std::move(accept_language));
    }
    base::DictValue structured;
    structured.Set("page_id", *page_id);
    const bool network = tool_request.tool == "page.network_events";
    if (network) {
      structured.Set("events", observer->ReadAndClearNetworkEvents());
    } else {
      structured.Set("messages", observer->ReadAndClear());
    }
    std::move(reply).Run(MakeToolCallResponse(
        std::move(tool_request.request_id), std::move(structured),
        network ? "Read completed network resource events."
                : "Read console messages since the previous call.",
        false));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.visible_content") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    if (!page_id || !frame) {
      base::DictValue error;
      error.Set("error", "Unknown or closed page_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed page_id.", true));
      return;
    }
    constexpr char kVisibleContentScript[] = R"JS((() => {
      const visible = e => e.getClientRects().length > 0;
      return {
        title: document.title,
        url: location.href,
        text: document.body ? document.body.innerText : '',
        headings: Array.from(document.querySelectorAll('h1,h2,h3,h4,h5,h6'))
            .filter(visible).map(e => ({level: Number(e.tagName.slice(1)), text: e.innerText})),
        links: Array.from(document.querySelectorAll('a[href]'))
            .filter(visible).map(e => ({text: e.innerText, url: e.href}))
      };
    })())JS";
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(std::string(kVisibleContentScript)),
        base::BindOnce(
            [](base::Value request_id, std::string page_id,
               base::OnceCallback<void(std::string)> reply,
               base::Value evaluation) {
              if (!evaluation.is_dict()) {
                base::DictValue error;
                error.Set("error", "Page content is not available.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(error),
                    "Page content is not available.", true));
                return;
              }
              base::DictValue structured = evaluation.GetDict().Clone();
              structured.Set("page_id", std::move(page_id));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Read visible page content.", false));
            },
            std::move(tool_request.request_id), *page_id, std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.accessibility_snapshot") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    if (!page_id || !tab || !tab->GetContents()) {
      base::DictValue error;
      error.Set("error", "Unknown or closed page_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed page_id.", true));
      return;
    }
    tab->GetContents()->RequestAXTreeSnapshot(
        base::BindOnce(
            [](base::Value request_id, std::string page_id,
               base::OnceCallback<void(std::string)> reply,
               ui::AXTreeUpdate& snapshot) {
              base::DictValue structured;
              structured.Set("page_id", std::move(page_id));
              structured.Set("root_id", snapshot.root_id);
              base::ListValue nodes;
              for (const ui::AXNodeData& node : snapshot.nodes) {
                base::DictValue item;
                item.Set("id", node.id);
                item.Set("role", ui::ToString(node.role));
                item.Set("name", node.GetStringAttribute(
                                     ax::mojom::StringAttribute::kName));
                item.Set("value", node.GetStringAttribute(
                                      ax::mojom::StringAttribute::kValue));
                item.Set("description",
                         node.GetStringAttribute(
                             ax::mojom::StringAttribute::kDescription));
                item.Set("placeholder",
                         node.GetStringAttribute(
                             ax::mojom::StringAttribute::kPlaceholder));
                item.Set("role_description",
                         node.GetStringAttribute(
                             ax::mojom::StringAttribute::kRoleDescription));
                if (node.HasIntAttribute(
                        ax::mojom::IntAttribute::kRestriction)) {
                  item.Set("restriction",
                           node.GetIntAttribute(
                               ax::mojom::IntAttribute::kRestriction));
                }
                if (node.HasIntAttribute(
                        ax::mojom::IntAttribute::kCheckedState)) {
                  item.Set("checked_state",
                           node.GetIntAttribute(
                               ax::mojom::IntAttribute::kCheckedState));
                }
                if (node.HasIntAttribute(
                        ax::mojom::IntAttribute::kHierarchicalLevel)) {
                  item.Set("level",
                           node.GetIntAttribute(
                               ax::mojom::IntAttribute::kHierarchicalLevel));
                }
                if (node.HasIntAttribute(ax::mojom::IntAttribute::kPosInSet)) {
                  item.Set(
                      "position_in_set",
                      node.GetIntAttribute(ax::mojom::IntAttribute::kPosInSet));
                }
                if (node.HasIntAttribute(ax::mojom::IntAttribute::kSetSize)) {
                  item.Set("set_size", node.GetIntAttribute(
                                           ax::mojom::IntAttribute::kSetSize));
                }
                item.Set("expanded",
                         node.HasState(ax::mojom::State::kExpanded));
                item.Set("collapsed",
                         node.HasState(ax::mojom::State::kCollapsed));
                item.Set("required",
                         node.HasState(ax::mojom::State::kRequired));
                item.Set(
                    "busy",
                    node.HasBoolAttribute(ax::mojom::BoolAttribute::kBusy) &&
                        node.GetBoolAttribute(ax::mojom::BoolAttribute::kBusy));
                item.Set("modal", node.HasBoolAttribute(
                                      ax::mojom::BoolAttribute::kModal) &&
                                      node.GetBoolAttribute(
                                          ax::mojom::BoolAttribute::kModal));
                base::ListValue children;
                for (int32_t child_id : node.child_ids) {
                  children.Append(child_id);
                }
                item.Set("child_ids", std::move(children));
                nodes.Append(std::move(item));
              }
              if (nodes.empty()) {
                structured.Set("error", "Accessibility snapshot is empty.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured),
                    "Accessibility snapshot is empty.", true));
                return;
              }
              structured.Set("nodes", std::move(nodes));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Captured native accessibility tree.", false));
            },
            std::move(tool_request.request_id), *page_id, std::move(reply)),
        ui::kAXModeComplete, /*max_nodes=*/0, base::Seconds(30),
        content::WebContents::AXTreeSnapshotPolicy::kAll);
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.screenshot") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderWidgetHostView* view =
        tab && tab->GetContents()
            ? tab->GetContents()->GetRenderWidgetHostView()
            : nullptr;
    if (!page_id || !view) {
      base::DictValue error;
      error.Set("error", "Unknown or closed page_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed page_id.", true));
      return;
    }
    view->CopyFromSurface(
        gfx::Rect(), gfx::Size(), base::Seconds(5),
        base::BindOnce(
            [](base::Value request_id,
               base::OnceCallback<void(std::string)> reply,
               const content::CopyFromSurfaceResult& result) {
              if (!result.has_value() || result->bitmap.drawsNothing()) {
                base::DictValue error;
                error.Set("error",
                          "Chromium could not capture the page surface.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(error),
                    "Chromium could not capture the page surface.", true));
                return;
              }
              const SkBitmap& bitmap = result->bitmap;
              std::optional<std::vector<uint8_t>> png =
                  gfx::PNGCodec::EncodeBGRASkBitmap(
                      bitmap, /*discard_transparency=*/false);
              if (!png) {
                base::DictValue error;
                error.Set("error", "Chromium could not encode the page image.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(error),
                    "Chromium could not encode the page image.", true));
                return;
              }
              std::move(reply).Run(MakeImageToolCallResponse(
                  std::move(request_id), base::Base64Encode(*png),
                  bitmap.width(), bitmap.height()));
            },
            std::move(tool_request.request_id), std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "context.set_cookies") {
    const std::string* requested_context_id =
        tool_request.arguments.FindString("context_id");
    Profile* context = ResolveContext(
        requested_context_id ? *requested_context_id : default_context_id_);
    const base::ListValue* input_cookies =
        tool_request.arguments.FindList("cookies");
    if (!context || !input_cookies) {
      base::DictValue error;
      error.Set("error", "A live context_id and cookies array are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live context_id and cookies array are required.", true));
      return;
    }
    const base::Time now = base::Time::Now();
    std::vector<std::pair<net::CanonicalCookie, GURL>> cookies_to_set;
    for (const base::Value& value : *input_cookies) {
      const base::DictValue* input = value.GetIfDict();
      const std::string* url_text = input ? input->FindString("url") : nullptr;
      const std::string* name = input ? input->FindString("name") : nullptr;
      const std::string* cookie_value =
          input ? input->FindString("value") : nullptr;
      const GURL url = url_text ? GURL(*url_text) : GURL();
      if (!url.is_valid() || (!url.SchemeIsHTTPOrHTTPS()) || !name ||
          !cookie_value) {
        base::DictValue error;
        error.Set("error",
                  "Each cookie needs an HTTP(S) url, name, and value.");
        std::move(reply).Run(MakeToolCallResponse(
            std::move(tool_request.request_id), std::move(error),
            "Each cookie needs an HTTP(S) url, name, and value.", true));
        return;
      }
      const std::string* domain_value = input->FindString("domain");
      const std::string* path_value = input->FindString("path");
      const std::string domain = domain_value ? *domain_value : std::string();
      const std::string path = path_value ? *path_value : "/";
      const bool secure = input->FindBool("secure").value_or(false);
      const bool http_only = input->FindBool("http_only").value_or(false);
      net::CookieSameSite same_site = net::CookieSameSite::UNSPECIFIED;
      if (const std::string* same_site_text = input->FindString("same_site")) {
        if (*same_site_text == "none") {
          same_site = net::CookieSameSite::NO_RESTRICTION;
        } else if (*same_site_text == "lax") {
          same_site = net::CookieSameSite::LAX_MODE;
        } else if (*same_site_text == "strict") {
          same_site = net::CookieSameSite::STRICT_MODE;
        } else if (*same_site_text != "unspecified") {
          base::DictValue error;
          error.Set("error",
                    "same_site must be unspecified, none, lax, or strict.");
          std::move(reply).Run(MakeToolCallResponse(
              std::move(tool_request.request_id), std::move(error),
              "Invalid same_site value.", true));
          return;
        }
      }
      base::Time expiration;
      if (const base::Value* expiry = input->Find("expires_unix_ms")) {
        if (expiry->is_double()) {
          expiration = base::Time::FromMillisecondsSinceUnixEpoch(
              static_cast<int64_t>(expiry->GetDouble()));
        } else if (expiry->is_int()) {
          expiration =
              base::Time::FromMillisecondsSinceUnixEpoch(expiry->GetInt());
        } else {
          base::DictValue error;
          error.Set("error", "expires_unix_ms must be a number.");
          std::move(reply).Run(MakeToolCallResponse(
              std::move(tool_request.request_id), std::move(error),
              "expires_unix_ms must be a number.", true));
          return;
        }
      }
      net::CookieInclusionStatus status;
      std::unique_ptr<net::CanonicalCookie> cookie =
          net::CanonicalCookie::CreateSanitizedCookie(
              url, *name, *cookie_value, domain, path, now, expiration, now,
              secure, http_only, same_site, net::COOKIE_PRIORITY_DEFAULT,
              std::nullopt, &status);
      if (!cookie) {
        base::DictValue error;
        error.Set("error",
                  "Cookie attributes are not valid for the source url.");
        std::move(reply).Run(MakeToolCallResponse(
            std::move(tool_request.request_id), std::move(error),
            "Cookie attributes are not valid for the source url.", true));
        return;
      }
      cookies_to_set.emplace_back(std::move(*cookie), url);
    }
    if (cookies_to_set.empty()) {
      base::DictValue structured;
      structured.Set("set_count", 0);
      structured.Set("rejected_count", 0);
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(structured),
          "Set zero cookies.", false));
      return;
    }
    auto cookie_results_callback =
        base::BarrierCallback<net::CookieAccessResult>(
            cookies_to_set.size(),
            base::BindOnce(
                [](base::Value request_id,
                   base::OnceCallback<void(std::string)> reply,
                   std::vector<net::CookieAccessResult> results) {
                  int accepted = 0;
                  for (const net::CookieAccessResult& result : results) {
                    accepted += result.status.IsInclude();
                  }
                  base::DictValue structured;
                  structured.Set("set_count", accepted);
                  structured.Set("rejected_count",
                                 static_cast<int>(results.size()) - accepted);
                  const bool success =
                      accepted == static_cast<int>(results.size());
                  std::move(reply).Run(MakeToolCallResponse(
                      std::move(request_id), std::move(structured),
                      success ? "Set context cookies."
                              : "Some cookies were rejected by Chromium.",
                      !success));
                },
                std::move(tool_request.request_id), std::move(reply)));
    network::mojom::CookieManager* cookie_manager =
        context->GetDefaultStoragePartition()
            ->GetCookieManagerForBrowserProcess();
    for (auto& [cookie, url] : cookies_to_set) {
      cookie_manager->SetCanonicalCookie(std::move(cookie), url,
                                         net::CookieOptions::MakeAllInclusive(),
                                         cookie_results_callback);
    }
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.evaluate") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* expression =
        tool_request.arguments.FindString("expression");
    const std::string* requested_frame_id =
        tool_request.arguments.FindString("frame_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
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
    if (!page_id || !expression || !tab || !tab->GetContents() || !frame) {
      base::DictValue error;
      error.Set("error",
                "A live page_id, frame_id, and expression are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id, frame_id, and expression are required.", true));
      return;
    }
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(*expression),
        base::BindOnce(
            [](base::Value request_id, std::string page_id,
               std::optional<std::string> frame_id,
               base::OnceCallback<void(std::string)> reply, base::Value value) {
              base::DictValue structured;
              structured.Set("page_id", std::move(page_id));
              if (frame_id) {
                structured.Set("frame_id", std::move(*frame_id));
              }
              structured.Set("value", std::move(value));
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Evaluated JavaScript in the page main world.", false));
            },
            std::move(tool_request.request_id), *page_id,
            requested_frame_id ? std::optional<std::string>(*requested_frame_id)
                               : std::nullopt,
            std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      (tool_request.tool == "page.check" ||
       tool_request.tool == "page.uncheck")) {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* selector = tool_request.arguments.FindString("selector");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    if (!page_id || !selector || !frame) {
      base::DictValue error;
      error.Set("error", "A live page_id and selector are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id and selector are required.", true));
      return;
    }
    const std::string selector_json =
        base::WriteJson(base::Value(*selector)).value_or("\"\"");
    const std::string script = base::StrCat(
        {"(() => { const e = document.querySelector(", selector_json,
         "); if (!(e instanceof HTMLInputElement) || e.type !== 'checkbox') "
         "return {error: 'selector must match a checkbox'}; "
         "if (e.disabled || !e.getClientRects().length) "
         "return {error: 'checkbox is not actionable'}; "
         "const r = e.getBoundingClientRect(); return {checked: e.checked, "
         "x: Math.round(r.x + r.width / 2), "
         "y: Math.round(r.y + r.height / 2)}; })()"});
    const bool desired_checked = tool_request.tool == "page.check";
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(script),
        base::BindOnce(
            [](base::Value request_id, std::string page_id,
               tabs::TabHandle tab_handle, bool desired_checked,
               base::OnceCallback<void(std::string)> reply,
               base::Value evaluation) {
              base::DictValue structured;
              const base::DictValue* data = evaluation.GetIfDict();
              if (data && data->FindString("error")) {
                const std::string error = *data->FindString("error");
                structured.Set("error", error);
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured), error, true));
                return;
              }
              const std::optional<bool> checked =
                  data ? data->FindBool("checked") : std::nullopt;
              const std::optional<int> x =
                  data ? data->FindInt("x") : std::nullopt;
              const std::optional<int> y =
                  data ? data->FindInt("y") : std::nullopt;
              tabs::TabInterface* tab = tab_handle.Get();
              content::RenderWidgetHostView* view =
                  tab && tab->GetContents()
                      ? tab->GetContents()->GetRenderWidgetHostView()
                      : nullptr;
              content::RenderWidgetHost* widget =
                  view ? view->GetRenderWidgetHost() : nullptr;
              if (!checked || !x || !y || !widget) {
                structured.Set("error", "Checkbox or page is no longer live.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured),
                    "Checkbox or page is no longer live.", true));
                return;
              }
              if (*checked != desired_checked) {
                const gfx::PointF location(*x, *y);
                widget->ForwardMouseEvent(blink::WebMouseEvent(
                    blink::WebInputEvent::Type::kMouseMove, location, location,
                    blink::WebPointerProperties::Button::kNoButton, 0,
                    blink::WebInputEvent::kNoModifiers,
                    base::TimeTicks::Now()));
                widget->ForwardMouseEvent(blink::WebMouseEvent(
                    blink::WebInputEvent::Type::kMouseDown, location, location,
                    blink::WebPointerProperties::Button::kLeft, 1,
                    blink::WebInputEvent::kLeftButtonDown,
                    base::TimeTicks::Now()));
                widget->ForwardMouseEvent(blink::WebMouseEvent(
                    blink::WebInputEvent::Type::kMouseUp, location, location,
                    blink::WebPointerProperties::Button::kLeft, 1,
                    blink::WebInputEvent::kNoModifiers,
                    base::TimeTicks::Now()));
              }
              structured.Set("page_id", std::move(page_id));
              structured.Set("checked", desired_checked);
              structured.Set("changed", *checked != desired_checked);
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Checkbox state requested through native input.", false));
            },
            std::move(tool_request.request_id), *page_id, handle->second,
            desired_checked, std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      (tool_request.tool == "page.focus_element" ||
       tool_request.tool == "page.select_option")) {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* selector = tool_request.arguments.FindString("selector");
    const std::string* option_value =
        tool_request.arguments.FindString("value");
    const bool select_option = tool_request.tool == "page.select_option";
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    if (!page_id || !selector || !frame || (select_option && !option_value)) {
      base::DictValue error;
      error.Set("error",
                "A live page_id, selector, and required value are needed.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id, selector, and required value are needed.", true));
      return;
    }
    const std::string selector_json =
        base::WriteJson(base::Value(*selector)).value_or("\"\"");
    std::string script = base::StrCat(
        {"(() => { const e = document.querySelector(", selector_json,
         "); if (!e) return {error: 'selector did not match'}; "});
    if (select_option) {
      const std::string value_json =
          base::WriteJson(base::Value(*option_value)).value_or("\"\"");
      script += base::StrCat(
          {"if (!(e instanceof HTMLSelectElement)) return {error: "
           "'element is not a select'}; const option = Array.from(e.options)"
           ".find(o => o.value === ",
           value_json,
           "); if (!option) return {error: 'option did not match'}; "
           "e.value = option.value; e.dispatchEvent(new Event('input', "
           "{bubbles: true})); e.dispatchEvent(new Event('change', "
           "{bubbles: true})); return {value: e.value}; })()"});
    } else {
      script +=
          "e.focus(); return {focused: document.activeElement === e}; })()";
    }
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(script),
        base::BindOnce(
            [](base::Value request_id, std::string page_id, bool select_option,
               base::OnceCallback<void(std::string)> reply,
               base::Value evaluation) {
              base::DictValue structured;
              const base::DictValue* evaluation_dict = evaluation.GetIfDict();
              if (evaluation_dict) {
                if (const std::string* error =
                        evaluation_dict->FindString("error")) {
                  structured.Set("error", *error);
                  std::move(reply).Run(MakeToolCallResponse(
                      std::move(request_id), std::move(structured), *error,
                      true));
                  return;
                }
              }
              if (!evaluation_dict ||
                  (select_option && !evaluation_dict->FindString("value")) ||
                  (!select_option &&
                   !evaluation_dict->FindBool("focused").value_or(false))) {
                structured.Set("error", "Element operation failed.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured),
                    "Element operation failed.", true));
                return;
              }
              structured.Set("page_id", std::move(page_id));
              if (select_option) {
                structured.Set("value", *evaluation_dict->FindString("value"));
                structured.Set("selected", true);
              } else {
                structured.Set("focused", true);
              }
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  select_option ? "Selected option." : "Focused element.",
                  false));
            },
            std::move(tool_request.request_id), *page_id, select_option,
            std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.fill") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* selector = tool_request.arguments.FindString("selector");
    const std::string* fill_value = tool_request.arguments.FindString("value");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    content::RenderFrameHost* frame =
        tab && tab->GetContents() ? tab->GetContents()->GetPrimaryMainFrame()
                                  : nullptr;
    if (!page_id || !selector || !fill_value || !frame) {
      base::DictValue error;
      error.Set("error", "A live page_id, selector, and value are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id, selector, and value are required.", true));
      return;
    }
    const std::string selector_json =
        base::WriteJson(base::Value(*selector)).value_or("\"\"");
    const std::string value_json =
        base::WriteJson(base::Value(*fill_value)).value_or("\"\"");
    const std::string script = base::StrCat(
        {"(() => { const e = document.querySelector(", selector_json,
         "); if (!e) return {error: 'selector did not match'}; e.focus(); ",
         "if (e.isContentEditable) { e.textContent = ", value_json,
         "; } else if (e instanceof HTMLInputElement || e instanceof "
         "HTMLTextAreaElement) { ",
         "const p = e instanceof HTMLTextAreaElement ? "
         "HTMLTextAreaElement.prototype : HTMLInputElement.prototype; ",
         "Object.getOwnPropertyDescriptor(p, 'value').set.call(e, ", value_json,
         "); } else { return {error: 'element is not editable'}; } ",
         "e.dispatchEvent(new InputEvent('input', {bubbles: true, inputType: "
         "'insertText', data: ",
         value_json,
         "})); e.dispatchEvent(new Event('change', {bubbles: true})); ",
         "return {value: e.isContentEditable ? e.textContent : e.value}; "
         "})()"});
    content::ExecuteJavaScriptForAutomation(
        frame, base::UTF8ToUTF16(script),
        base::BindOnce(
            [](base::Value request_id, std::string page_id,
               base::OnceCallback<void(std::string)> reply,
               base::Value result) {
              base::DictValue structured;
              if (const base::DictValue* result_dict = result.GetIfDict()) {
                if (const std::string* error =
                        result_dict->FindString("error")) {
                  structured.Set("error", *error);
                  std::move(reply).Run(MakeToolCallResponse(
                      std::move(request_id), std::move(structured), *error,
                      true));
                  return;
                }
                if (const base::Value* value = result_dict->Find("value")) {
                  structured.Set("value", value->Clone());
                }
              }
              structured.Set("page_id", std::move(page_id));
              structured.Set("filled", true);
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Filled the selected form control.", false));
            },
            std::move(tool_request.request_id), *page_id, std::move(reply)));
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.wait_for_url") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    const std::string* url = tool_request.arguments.FindString("url");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    const GURL target_url = url ? GURL(*url) : GURL();
    if (!page_id || !tab || !tab->GetContents() || !target_url.is_valid()) {
      base::DictValue error;
      error.Set("error", "A live page_id and valid url are required.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "A live page_id and valid url are required.", true));
      return;
    }
    scoped_refptr<URLWaiter> waiter = base::MakeRefCounted<URLWaiter>(
        tab->GetContents(), std::move(tool_request.request_id), *page_id,
        target_url, std::move(reply));
    pending_requests_[connection_id].stop_waiting =
        base::BindOnce(&URLWaiter::Cancel, waiter);
    waiter->Start();
    return;
  }
  if (method && *method == "tools/call" &&
      protocol::ParseToolCall(request, &tool_request, &parse_error) &&
      tool_request.tool == "page.wait_for_load_state") {
    const std::string* page_id = tool_request.arguments.FindString("page_id");
    auto handle = page_id ? page_handles_.find(*page_id) : page_handles_.end();
    tabs::TabInterface* tab =
        handle == page_handles_.end() ? nullptr : handle->second.Get();
    if (!page_id || !tab || !tab->GetContents()) {
      base::DictValue error;
      error.Set("error", "Unknown or closed page_id.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Unknown or closed page_id.", true));
      return;
    }
    const std::string* requested_state =
        tool_request.arguments.FindString("state");
    if (requested_state && *requested_state != "load") {
      base::DictValue error;
      error.Set("error", "Only the load state is currently supported.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Only the load state is currently supported.", true));
      return;
    }
    scoped_refptr<LoadStateWaiter> waiter =
        base::MakeRefCounted<LoadStateWaiter>(
            tab->GetContents(), std::move(tool_request.request_id), *page_id,
            std::move(reply));
    pending_requests_[connection_id].stop_waiting =
        base::BindOnce(&LoadStateWaiter::Cancel, waiter);
    waiter->Start();
    return;
  }
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
      return;
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
    return;
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
      return;
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
    return;
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
      return;
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
    return;
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
      return;
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
    return;
  }
  std::move(reply).Run(HandleJsonRpc(request));
}

void BrowserAgentMcpServer::CompleteRequestOnUiThread(
    int connection_id,
    std::shared_ptr<protocol::CancellationSource> cancellation,
    base::OnceCallback<void(std::string)> reply,
    std::string response) {
  auto pending = pending_requests_.find(connection_id);
  if (pending != pending_requests_.end() &&
      pending->second.cancellation == cancellation) {
    pending_requests_.erase(pending);
  }
  if (!cancellation->token().IsCancelled()) {
    std::move(reply).Run(std::move(response));
  }
}

void BrowserAgentMcpServer::CancelRequestOnUiThread(int connection_id) {
  auto pending = pending_requests_.find(connection_id);
  if (pending == pending_requests_.end()) {
    return;
  }
  pending->second.cancellation->Cancel();
  base::OnceClosure stop_waiting = std::move(pending->second.stop_waiting);
  pending_requests_.erase(pending);
  if (stop_waiting) {
    std::move(stop_waiting).Run();
  }
}

void BrowserAgentMcpServer::StartEventStreamOnUiThread(int connection_id) {
  if (!event_stream_watches_.insert(connection_id).second ||
      event_stream_watches_.size() != 1) {
    return;
  }
  event_page_snapshot_ = CollectBrowserPageSnapshot();
  event_poll_timer_.Start(
      FROM_HERE, base::Milliseconds(100),
      base::BindRepeating(&BrowserAgentMcpServer::PollBrowserEvents,
                          weak_factory_.GetWeakPtr()));
}

void BrowserAgentMcpServer::StopEventStreamOnUiThread(int connection_id) {
  event_stream_watches_.erase(connection_id);
  if (event_stream_watches_.empty()) {
    event_poll_timer_.Stop();
    event_page_snapshot_.clear();
    for (auto& [page_id, observer] : console_observers_) {
      observer->SetNavigationEventCallback({});
    }
  }
}

}  // namespace browser_agent
