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

bool BrowserAgentMcpServer::HandlePageReadTools(
    const base::DictValue& request,
    int connection_id,
    base::OnceCallback<void(std::string)>& reply) {
  const std::string* method = request.FindString("method");
  protocol::Request tool_request;
  protocol::Error parse_error;
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
      return true;
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
        if (e.matches('button,input[type=button],input[type=submit],input[type=reset]')) return 'button';
        if (e.matches('a[href]')) return 'link';
        if (e.matches('input[type=checkbox]')) return 'checkbox';
        if (e.matches('input[type=radio]')) return 'radio';
        if (e.matches('input[type=range]')) return 'slider';
        if (e.matches('input[type=number]')) return 'spinbutton';
        if (e.matches('input:not([type]),input[type=text],input[type=email],input[type=password],input[type=search],input[type=tel],input[type=url],textarea')) return 'textbox';
        if (e.matches('select[multiple]')) return 'listbox';
        if (e.matches('select')) return 'combobox';
        if (e.matches('h1,h2,h3,h4,h5,h6')) return 'heading';
        if (e.matches('img')) return 'img';
        if (e.matches('ul,ol')) return 'list';
        if (e.matches('li')) return 'listitem';
        if (e.matches('table')) return 'table';
        if (e.matches('tr')) return 'row';
        if (e.matches('th')) return 'columnheader';
        if (e.matches('td')) return 'cell';
        if (e.matches('form')) return 'form';
        if (e.matches('nav')) return 'navigation';
        if (e.matches('main')) return 'main';
        if (e.matches('header')) return 'banner';
        if (e.matches('footer')) return 'contentinfo';
        return '';
      };
      const nameOf = e => {
        const labelledby = e.getAttribute('aria-labelledby');
        if (labelledby) {
          const name = labelledby.trim().split(/\\s+/).map(id =>
              document.getElementById(id)?.innerText || '').join(' ').trim();
          if (name) return name;
        }
        const aria = e.getAttribute('aria-label');
        if (aria) return aria;
        if (e.labels && e.labels.length)
          return Array.from(e.labels).map(l => l.innerText.trim()).join(' ');
        if (e.matches('img,area,input[type=image]')) return e.getAttribute('alt') || '';
        if (e.matches('input[type=button],input[type=submit],input[type=reset]')) return e.value || '';
        return (e.innerText || e.textContent || '').trim() || e.getAttribute('title') || '';
      };
      try {
        return Array.from(document.querySelectorAll(q.selector || '*'))
            .filter(e => {
              const s = getComputedStyle(e);
              const r = e.getBoundingClientRect();
              return s.display !== 'none' && s.visibility !== 'hidden' &&
                  Number(s.opacity) > 0 && r.width > 0 && r.height > 0 &&
                  r.right > 0 && r.bottom > 0 && r.left < innerWidth &&
                  r.top < innerHeight;
            })
            .map(e => {
              const r = e.getBoundingClientRect();
              const x = r.left + r.width / 2, y = r.top + r.height / 2;
              const hit = document.elementFromPoint(x, y);
              const actionable = !!hit && (hit === e || e.contains(hit));
              return {tag: e.tagName.toLowerCase(), role: roleOf(e),
                name: nameOf(e), text: (e.innerText || '').trim(),
                x: r.x, y: r.y, width: r.width, height: r.height,
                center_x: x, center_y: y, actionable};
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
    return true;
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
      return true;
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
    return true;
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
      return true;
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
    return true;
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
      return true;
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
    return true;
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
      return true;
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
    return true;
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
      return true;
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
        return true;
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
          return true;
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
          return true;
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
        return true;
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
      return true;
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
    return true;
  }
  return false;
}

}  // namespace browser_agent
