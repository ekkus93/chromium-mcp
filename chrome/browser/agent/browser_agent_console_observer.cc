// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_console_observer.h"

#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/http/http_response_headers.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom.h"
#include "url/gurl.h"

namespace browser_agent {

BrowserAgentConsoleObserver::BrowserAgentConsoleObserver(
    content::WebContents* contents,
    std::string accept_language)
    : content::WebContentsObserver(contents),
      accept_language_(std::move(accept_language)) {}

BrowserAgentConsoleObserver::~BrowserAgentConsoleObserver() = default;

void BrowserAgentConsoleObserver::SetAcceptLanguage(
    std::string accept_language) {
  accept_language_ = std::move(accept_language);
}

void BrowserAgentConsoleObserver::SetNavigationEventCallback(
    base::RepeatingCallback<void(std::string, std::string)> callback) {
  navigation_event_callback_ = std::move(callback);
}

base::ListValue BrowserAgentConsoleObserver::ReadAndClear() {
  return std::exchange(messages_, base::ListValue());
}

base::ListValue BrowserAgentConsoleObserver::ReadAndClearNetworkEvents() {
  return std::exchange(network_events_, base::ListValue());
}

void BrowserAgentConsoleObserver::AddNetworkEvent(base::DictValue event) {
  network_events_.Append(event.Clone());
}

void BrowserAgentConsoleObserver::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!accept_language_.empty()) {
    navigation_handle->SetRequestHeader("Accept-Language", accept_language_);
  }
  if (navigation_event_callback_) {
    navigation_event_callback_.Run("navigation_started",
                                   navigation_handle->GetURL().spec());
  }
  base::DictValue item;
  item.Set("event_type", "navigation_request");
  item.Set("url", navigation_handle->GetURL().spec());
  item.Set("method", navigation_handle->GetRequestMethod());
  item.Set("main_frame", navigation_handle->IsInPrimaryMainFrame());
  base::ListValue request_headers;
  for (const auto& [name, value] :
       navigation_handle->GetRequestHeaders().GetHeaderVector()) {
    base::DictValue header;
    header.Set("name", name);
    header.Set("value", value);
    request_headers.Append(std::move(header));
  }
  item.Set("request_headers", std::move(request_headers));
  network_events_.Append(std::move(item));
}

void BrowserAgentConsoleObserver::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_event_callback_) {
    navigation_event_callback_.Run(navigation_handle->HasCommitted()
                                       ? "navigation_committed"
                                       : "navigation_failed",
                                   navigation_handle->GetURL().spec());
  }
  base::DictValue item;
  item.Set("event_type", "navigation_response");
  item.Set("url", navigation_handle->GetURL().spec());
  item.Set("method", navigation_handle->GetRequestMethod());
  item.Set("committed", navigation_handle->HasCommitted());
  item.Set("net_error", navigation_handle->GetNetErrorCode());
  item.Set("main_frame", navigation_handle->IsInPrimaryMainFrame());
  if (navigation_handle->GetResponseHeaders()) {
    base::ListValue response_headers;
    item.Set("http_status",
             navigation_handle->GetResponseHeaders()->response_code());
    size_t iterator = 0;
    std::string name;
    std::string value;
    while (navigation_handle->GetResponseHeaders()->EnumerateHeaderLines(
        &iterator, &name, &value)) {
      base::DictValue header;
      header.Set("name", name);
      header.Set("value", value);
      response_headers.Append(std::move(header));
    }
    item.Set("response_headers", std::move(response_headers));
  }
  network_events_.Append(std::move(item));
}

void BrowserAgentConsoleObserver::DidStopLoading() {
  if (navigation_event_callback_ && web_contents()) {
    navigation_event_callback_.Run(
        "navigation_finished", web_contents()->GetLastCommittedURL().spec());
  }
}

void BrowserAgentConsoleObserver::ResourceLoadComplete(
    content::RenderFrameHost* source_frame,
    const content::GlobalRequestID& request_id,
    const GURL& original_url,
    const blink::mojom::ResourceLoadInfo& info) {
  base::DictValue item;
  item.Set("event_type", "resource_complete");
  item.Set("request_id", static_cast<double>(info.request_id));
  item.Set("original_url", original_url.spec());
  item.Set("final_url", info.final_url.spec());
  item.Set("referrer", info.referrer.spec());
  item.Set("method", info.method);
  item.Set("request_destination", static_cast<int>(info.request_destination));
  item.Set("mime_type", info.mime_type);
  item.Set("http_status", info.http_status_code);
  item.Set("net_error", info.net_error);
  item.Set("from_cache", info.was_cached);
  item.Set("body_bytes", static_cast<double>(info.raw_body_bytes.InBytes()));
  item.Set("total_received_bytes",
           static_cast<double>(info.total_received_bytes.InBytes()));
  if (info.network_info) {
    item.Set("network_accessed", info.network_info->network_accessed);
  }
  if (source_frame) {
    item.Set("frame_url", source_frame->GetLastCommittedURL().spec());
  }
  network_events_.Append(std::move(item));
}

void BrowserAgentConsoleObserver::OnDidAddMessageToConsole(
    content::RenderFrameHost* source_frame,
    blink::mojom::ConsoleMessageLevel log_level,
    const std::u16string& message,
    int32_t line_no,
    const std::u16string& source_id,
    const std::optional<std::u16string>& untrusted_stack_trace) {
  base::DictValue item;
  item.Set("level", static_cast<int>(log_level));
  item.Set("message", base::UTF16ToUTF8(message));
  item.Set("line", line_no);
  item.Set("source", base::UTF16ToUTF8(source_id));
  if (source_frame) {
    item.Set("frame_url", source_frame->GetLastCommittedURL().spec());
  }
  if (untrusted_stack_trace) {
    item.Set("stack_trace", base::UTF16ToUTF8(*untrusted_stack_trace));
  }
  messages_.Append(std::move(item));
}

void BrowserAgentConsoleObserver::WebContentsDestroyed() {
  Observe(nullptr);
}

}  // namespace browser_agent
