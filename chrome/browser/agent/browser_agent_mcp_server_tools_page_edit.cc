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

bool BrowserAgentMcpServer::HandlePageEditTools(
    const base::DictValue& request,
    int connection_id,
    base::OnceCallback<void(std::string)>& reply) {
  const std::string* method = request.FindString("method");
  protocol::Request tool_request;
  protocol::Error parse_error;
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
      return true;
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
    return true;
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
      return true;
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
    return true;
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
      return true;
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
    return true;
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
      return true;
    }
    const std::string selector_json =
        base::WriteJson(base::Value(*selector)).value_or("\"\"");
    const std::string value_json =
        base::WriteJson(base::Value(*fill_value)).value_or("\"\"");
    const std::string script = base::StrCat(
        {"(() => { const e = document.querySelector(", selector_json,
         "); if (!e) return {error: 'selector did not match'}; ",
         "const expected = ", value_json,
         "; const supportedInput = e instanceof HTMLInputElement && ",
         "['text','search','email','url','tel','password','number'].includes(e.type); ",
         "if (!(e.isContentEditable || supportedInput || e instanceof "
         "HTMLTextAreaElement)) return {error: 'element is not a supported "
         "editable control'}; ",
         "if (e.disabled || e.readOnly) return {error: 'element is disabled "
         "or read-only'}; e.focus(); ",
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
         "const actual = e.isContentEditable ? e.textContent : e.value; ",
         "if (actual !== expected) return {error: 'value readback failed'}; ",
         "return {value: actual}; "
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
              const base::DictValue* result_dict = result.GetIfDict();
              if (!result_dict || !result_dict->Find("value")) {
                structured.Set("error", "Fill evaluation returned no value.");
                std::move(reply).Run(MakeToolCallResponse(
                    std::move(request_id), std::move(structured),
                    "Fill evaluation returned no value.", true));
                return;
              }
              structured.Set("page_id", std::move(page_id));
              structured.Set("filled", true);
              std::move(reply).Run(MakeToolCallResponse(
                  std::move(request_id), std::move(structured),
                  "Filled the selected form control.", false));
            },
            std::move(tool_request.request_id), *page_id, std::move(reply)));
    return true;
  }
  return false;
}

}  // namespace browser_agent
