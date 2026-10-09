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

bool BrowserAgentMcpServer::HandleWaitAndDownloadTools(
    const base::DictValue& request,
    int connection_id,
    base::OnceCallback<void(std::string)>& reply) {
  const std::string* method = request.FindString("method");
  protocol::Request tool_request;
  protocol::Error parse_error;
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
      return true;
    }
    scoped_refptr<DownloadWaiter> waiter = base::MakeRefCounted<DownloadWaiter>(
        item, std::move(tool_request.request_id), std::move(reply));
    pending_requests_[connection_id].stop_waiting =
        base::BindOnce(&DownloadWaiter::Cancel, waiter);
    waiter->Start();
    return true;
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
      return true;
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
    return true;
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
      return true;
    }
    scoped_refptr<URLWaiter> waiter = base::MakeRefCounted<URLWaiter>(
        tab->GetContents(), std::move(tool_request.request_id), *page_id,
        target_url, std::move(reply));
    pending_requests_[connection_id].stop_waiting =
        base::BindOnce(&URLWaiter::Cancel, waiter);
    waiter->Start();
    return true;
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
      return true;
    }
    const std::string* requested_state =
        tool_request.arguments.FindString("state");
    if (requested_state && *requested_state != "load") {
      base::DictValue error;
      error.Set("error", "Only the load state is currently supported.");
      std::move(reply).Run(MakeToolCallResponse(
          std::move(tool_request.request_id), std::move(error),
          "Only the load state is currently supported.", true));
      return true;
    }
    scoped_refptr<LoadStateWaiter> waiter =
        base::MakeRefCounted<LoadStateWaiter>(
            tab->GetContents(), std::move(tool_request.request_id), *page_id,
            std::move(reply));
    pending_requests_[connection_id].stop_waiting =
        base::BindOnce(&LoadStateWaiter::Cancel, waiter);
    waiter->Start();
    return true;
  }
  return false;
}

void BrowserAgentMcpServer::HandleRequestOnUiThread(
    base::DictValue request,
    int connection_id,
    base::OnceCallback<void(std::string)> reply) {
  auto cancellation = std::make_shared<protocol::CancellationSource>();
  pending_requests_[connection_id] = PendingRequest{cancellation, {}};
  reply =
      base::BindOnce(&BrowserAgentMcpServer::CompleteRequestOnUiThread,
                     weak_this_, connection_id, cancellation, std::move(reply));
  if (HandleContextNetworkAndTracingTools(request, connection_id, reply) ||
      HandlePageSetupTools(request, connection_id, reply) ||
      HandlePageReadTools(request, connection_id, reply) ||
      HandlePageEditTools(request, connection_id, reply) ||
      HandleContextStorageTools(request, connection_id, reply) ||
      HandleWaitAndDownloadTools(request, connection_id, reply)) {
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
