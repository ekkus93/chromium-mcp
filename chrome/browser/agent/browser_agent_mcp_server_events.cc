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

std::map<std::string, BrowserAgentMcpServer::BrowserPageSnapshot>
BrowserAgentMcpServer::CollectBrowserPageSnapshot() {
  std::map<std::string, BrowserPageSnapshot> pages;
  std::vector<std::string> context_ids;
  context_ids.reserve(context_handles_.size());
  for (const auto& [context_id, profile] : context_handles_) {
    context_ids.push_back(context_id);
  }
  for (const std::string& context_id : context_ids) {
    Profile* context = ResolveContext(context_id);
    ProfileBrowserCollection* browsers =
        context ? ProfileBrowserCollection::GetForProfile(context) : nullptr;
    if (!browsers) {
      continue;
    }
    browsers->ForEach([&](BrowserWindowInterface* browser) {
      const bool is_popup =
          browser->GetType() == BrowserWindowInterface::TYPE_POPUP;
      TabStripModel* tabs = browser->GetTabStripModel();
      for (int i = 0; i < tabs->count(); ++i) {
        tabs::TabInterface* tab = tabs->GetTabAtIndex(i);
        content::WebContents* contents = tab ? tab->GetContents() : nullptr;
        if (!contents) {
          continue;
        }
        auto page = std::find_if(page_handles_.begin(), page_handles_.end(),
                                 [&](const auto& entry) {
                                   return entry.second == tab->GetHandle();
                                 });
        std::string page_id;
        if (page == page_handles_.end()) {
          page_id = base::UnguessableToken::Create().ToString();
          page_handles_.emplace(page_id, tab->GetHandle());
          page_context_ids_.emplace(page_id, context_id);
        } else {
          page_id = page->first;
          page_context_ids_.insert_or_assign(page_id, context_id);
        }
        auto& observer = console_observers_[page_id];
        if (!observer) {
          auto language = context_accept_languages_.find(context_id);
          observer = std::make_unique<BrowserAgentConsoleObserver>(
              contents, language == context_accept_languages_.end()
                            ? std::string()
                            : language->second);
        }
        observer->SetNavigationEventCallback(
            base::BindRepeating(&BrowserAgentMcpServer::PublishNavigationEvent,
                                weak_factory_.GetWeakPtr(), page_id));
        pages.emplace(page_id, BrowserPageSnapshot{
                                   contents->GetVisibleURL().spec(), is_popup});
      }
      return true;
    });
  }
  return pages;
}

void BrowserAgentMcpServer::PollBrowserEvents() {
  auto current_pages = CollectBrowserPageSnapshot();
  for (const auto& [page_id, current] : current_pages) {
    auto previous = event_page_snapshot_.find(page_id);
    if (previous == event_page_snapshot_.end()) {
      base::DictValue event;
      event.Set("event_type", "tab_created");
      event.Set("page_id", page_id);
      event.Set("url", current.url);
      event.Set("is_popup", current.is_popup);
      PublishBrowserEvent(event.Clone());
      if (current.is_popup) {
        event.Set("event_type", "popup_created");
        PublishBrowserEvent(std::move(event));
      }
      continue;
    }
  }
  for (const auto& [page_id, previous] : event_page_snapshot_) {
    if (current_pages.contains(page_id)) {
      continue;
    }
    base::DictValue event;
    event.Set("event_type", "tab_closed");
    event.Set("page_id", page_id);
    event.Set("url", previous.url);
    PublishBrowserEvent(event.Clone());
    if (previous.is_popup) {
      event.Set("event_type", "popup_closed");
      PublishBrowserEvent(std::move(event));
    }
    page_handles_.erase(page_id);
    page_context_ids_.erase(page_id);
    console_observers_.erase(page_id);
  }
  event_page_snapshot_ = std::move(current_pages);
}

}  // namespace browser_agent
