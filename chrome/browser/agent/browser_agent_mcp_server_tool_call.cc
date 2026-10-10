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

std::string BrowserAgentMcpServer::HandleToolCall(
    const base::Value* id,
    const base::DictValue& request) {
  constexpr char kServerName[] = "Chromium Browser";
  constexpr char kServerVersion[] = "0.1.0";
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", id->Clone());
  base::DictValue result;
  result.Set("resultType", "complete");
  protocol::Request tool_request;
  protocol::Error parse_error;
  if (!protocol::ParseToolCall(request, &tool_request, &parse_error)) {
    return MakeJsonRpcError(id->Clone(), parse_error.code, parse_error.message);
  }
  base::DictValue tool_result;
  std::string result_text;
  std::string early_response;
  if (!HandleBrowserContextToolCall(id, tool_request, tool_result, result,
                                    result_text, &early_response)) {
    Profile* page_profile = profile_;
    std::string page_context_id = default_context_id_;
    if (tool_request.tool == "page.list" || tool_request.tool == "page.new") {
      if (const std::string* requested_context_id =
              tool_request.arguments.FindString("context_id")) {
        page_profile = ResolveContext(*requested_context_id);
        if (!page_profile) {
          tool_result.Set("error", "Unknown or closed context_id.");
          result.Set("isError", true);
          result_text = *tool_result.FindString("error");
        } else {
          page_context_id = *requested_context_id;
        }
      }
    }
    ProfileBrowserCollection* browsers =
        ProfileBrowserCollection::GetForProfile(page_profile);
    std::string page_id;
    const std::string* requested_page_id =
        tool_request.arguments.FindString("page_id");
    tabs::TabInterface* selected_tab = nullptr;
    BrowserWindowInterface* selected_browser = nullptr;
    int selected_index = -1;
    if (requested_page_id) {
      auto stored_handle = page_handles_.find(*requested_page_id);
      if (stored_handle != page_handles_.end()) {
        selected_tab = stored_handle->second.Get();
        if (selected_tab) {
          selected_browser = selected_tab->GetBrowserWindowInterface();
          if (!selected_browser) {
            selected_tab = nullptr;
          } else {
            selected_index =
                selected_browser->GetTabStripModel()->GetIndexOfTab(
                    selected_tab);
            if (selected_index == TabStripModel::kNoTab) {
              selected_tab = nullptr;
              selected_browser = nullptr;
            }
          }
        }
      }
    }

    if (result_text.empty() && tool_request.tool == "page.list") {
      base::ListValue pages;
      browsers->ForEach([&](BrowserWindowInterface* browser) {
        TabStripModel* strip = browser->GetTabStripModel();
        for (int i = 0; i < strip->count(); ++i) {
          tabs::TabInterface* tab = strip->GetTabAtIndex(i);
          auto it = std::find_if(page_handles_.begin(), page_handles_.end(),
                                 [&](const auto& entry) {
                                   return entry.second == tab->GetHandle();
                                 });
          if (it == page_handles_.end()) {
            page_id = base::UnguessableToken::Create().ToString();
            page_handles_.emplace(page_id, tab->GetHandle());
            page_context_ids_.emplace(page_id, page_context_id);
          } else {
            page_id = it->first;
            page_context_ids_.insert_or_assign(page_id, page_context_id);
          }
          base::DictValue page;
          page.Set("page_id", page_id);
          page.Set("context_id", page_context_id);
          page.Set("url", tab->GetContents()->GetVisibleURL().spec());
          page.Set("title", base::UTF16ToUTF8(tab->GetContents()->GetTitle()));
          page.Set("loading", tab->GetContents()->IsLoading());
          pages.Append(std::move(page));
        }
        return true;
      });
      tool_result.Set("pages", std::move(pages));
      result_text = "Listed open pages.";
    } else if (result_text.empty() && tool_request.tool == "page.new") {
      BrowserWindowInterface* browser = browsers->GetLastActiveBrowser();
      if (!browser) {
        browser = CreateBrowserWindow(BrowserWindowCreateParams(
            page_profile, /*from_user_gesture=*/false));
        if (browser) {
          browser->GetWindow()->Show();
        }
      }
      if (!browser) {
        tool_result.Set("error", "Could not create a browser window.");
      } else {
        auto contents = content::WebContents::Create(
            content::WebContents::CreateParams(page_profile));
        TabStripModel* strip = browser->GetTabStripModel();
        strip->AppendWebContents(std::move(contents), true);
        tabs::TabInterface* tab = strip->GetActiveTab();
        if (auto language = context_accept_languages_.find(page_context_id);
            language != context_accept_languages_.end()) {
          content::WebContents* new_contents = tab->GetContents();
          new_contents->GetMutableRendererPrefs()->accept_languages =
              language->second;
          new_contents->SyncRendererPrefs();
        }
        page_id = base::UnguessableToken::Create().ToString();
        page_handles_.emplace(page_id, tab->GetHandle());
        page_context_ids_.emplace(page_id, page_context_id);
        auto language = context_accept_languages_.find(page_context_id);
        console_observers_.emplace(
            page_id,
            std::make_unique<BrowserAgentConsoleObserver>(
                tab->GetContents(), language == context_accept_languages_.end()
                                        ? std::string()
                                        : language->second));
        if (const std::string* url = tool_request.arguments.FindString("url")) {
          tab->GetContents()->GetController().LoadURL(
              GURL(*url), content::Referrer(), ui::PAGE_TRANSITION_TYPED, "");
        }
        tool_result.Set("page_id", page_id);
        tool_result.Set("context_id", page_context_id);
        result_text = "Opened a new page.";
      }
    } else if (result_text.empty() && !selected_tab) {
      tool_result.Set("error", "Unknown or closed page_id.");
    } else if (result_text.empty() && tool_request.tool == "page.get_info") {
      content::WebContents* contents = selected_tab->GetContents();
      tool_result.Set("page_id", *requested_page_id);
      tool_result.Set("url", contents->GetVisibleURL().spec());
      tool_result.Set("title", base::UTF16ToUTF8(contents->GetTitle()));
      tool_result.Set("loading", contents->IsLoading());
      result_text = "Read page information.";
    } else if (result_text.empty() &&
               tool_request.tool == "page.set_viewport_size") {
      const std::optional<int> width = tool_request.arguments.FindInt("width");
      const std::optional<int> height =
          tool_request.arguments.FindInt("height");
      if (!width || !height || *width <= 0 || *height <= 0) {
        tool_result.Set("error",
                        "Positive integer width and height are required.");
        result.Set("isError", true);
        result_text = *tool_result.FindString("error");
      } else {
        selected_tab->GetContents()->Resize(gfx::Rect(*width, *height));
        tool_result.Set("page_id", *requested_page_id);
        tool_result.Set("width", *width);
        tool_result.Set("height", *height);
        result_text = "Resized page viewport through WebContents.";
      }
    } else if (result_text.empty() && tool_request.tool == "page.dialog_info") {
      tool_result.Set("page_id", *requested_page_id);
      auto* manager =
          javascript_dialogs::TabModalDialogManager::FromWebContents(
              selected_tab->GetContents());
      const auto info = manager ? manager->GetActiveDialogInfo() : std::nullopt;
      tool_result.Set("open", info.has_value());
      if (info) {
        const char* type = "alert";
        if (info->type == content::JAVASCRIPT_DIALOG_TYPE_CONFIRM) {
          type = "confirm";
        } else if (info->type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT) {
          type = "prompt";
        }
        tool_result.Set("type", type);
        tool_result.Set("title", base::UTF16ToUTF8(info->title));
        tool_result.Set("message", base::UTF16ToUTF8(info->message));
        tool_result.Set("default_prompt",
                        base::UTF16ToUTF8(info->default_prompt));
      } else {
        auto* app_dialog =
            javascript_dialogs::AppModalDialogQueue::GetInstance()
                ->active_dialog();
        if (app_dialog &&
            app_dialog->web_contents() == selected_tab->GetContents()) {
          tool_result.Set("open", true);
          tool_result.Set("type", app_dialog->is_before_unload_dialog()
                                      ? "beforeunload"
                                      : "app_modal");
          tool_result.Set("title", base::UTF16ToUTF8(app_dialog->title()));
          tool_result.Set("message",
                          base::UTF16ToUTF8(app_dialog->message_text()));
          tool_result.Set("default_prompt",
                          base::UTF16ToUTF8(app_dialog->default_prompt_text()));
          tool_result.Set("is_reload", app_dialog->is_reload());
        }
      }
      result_text = "Read active JavaScript dialog details.";
    } else if (result_text.empty() && tool_request.tool == "page.frames") {
      base::ListValue frames;
      selected_tab->GetContents()
          ->GetPrimaryMainFrame()
          ->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
            const content::GlobalRenderFrameHostId global_id =
                frame->GetGlobalId();
            auto existing = std::find_if(
                frame_handles_.begin(), frame_handles_.end(),
                [&](const auto& entry) {
                  return entry.second.first == *requested_page_id &&
                         entry.second.second == global_id;
                });
            const std::string frame_id =
                existing == frame_handles_.end()
                    ? base::UnguessableToken::Create().ToString()
                    : existing->first;
            if (existing == frame_handles_.end()) {
              frame_handles_.emplace(
                  frame_id, std::make_pair(*requested_page_id, global_id));
            }
            base::DictValue frame_info;
            frame_info.Set("frame_id", frame_id);
            frame_info.Set("url", frame->GetLastCommittedURL().spec());
            frame_info.Set("name", frame->GetFrameName());
            frame_info.Set("main_frame", frame->GetParent() == nullptr);
            frame_info.Set("live", frame->IsRenderFrameLive());
            frames.Append(std::move(frame_info));
          });
      tool_result.Set("frames", std::move(frames));
      result_text = "Listed page frames.";
    } else if (result_text.empty() && (tool_request.tool == "page.click" ||
                                       tool_request.tool == "page.hover")) {
      const std::optional<int> x = tool_request.arguments.FindInt("x");
      const std::optional<int> y = tool_request.arguments.FindInt("y");
      const std::string* requested_frame_id =
          tool_request.arguments.FindString("frame_id");
      content::RenderWidgetHostView* page_view =
          selected_tab->GetContents()->GetRenderWidgetHostView();
      content::RenderWidgetHostView* view = page_view;
      bool shared_child_widget = false;
      if (requested_frame_id) {
        auto frame_handle = frame_handles_.find(*requested_frame_id);
        content::RenderFrameHost* frame =
            frame_handle != frame_handles_.end() &&
                    frame_handle->second.first == *requested_page_id
                ? content::RenderFrameHost::FromID(frame_handle->second.second)
                : nullptr;
        view = frame ? frame->GetView() : nullptr;
        shared_child_widget =
            frame &&
            frame != selected_tab->GetContents()->GetPrimaryMainFrame() &&
            view == page_view;
      }
      content::RenderWidgetHost* widget =
          view ? view->GetRenderWidgetHost() : nullptr;
      if (shared_child_widget) {
        tool_result.Set("error",
                        "This child frame shares the page widget; translate "
                        "its coordinates to the main-page viewport.");
      } else if (!x || !y || !widget) {
        tool_result.Set(
            "error",
            "Integer x and y coordinates and a live frame are required.");
      } else {
        const gfx::PointF location(*x, *y);
        widget->ForwardMouseEvent(blink::WebMouseEvent(
            blink::WebInputEvent::Type::kMouseMove, location, location,
            blink::WebPointerProperties::Button::kNoButton, 0,
            blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now()));
        if (tool_request.tool == "page.click") {
          widget->ForwardMouseEvent(blink::WebMouseEvent(
              blink::WebInputEvent::Type::kMouseDown, location, location,
              blink::WebPointerProperties::Button::kLeft, 1,
              blink::WebInputEvent::kLeftButtonDown, base::TimeTicks::Now()));
          widget->ForwardMouseEvent(blink::WebMouseEvent(
              blink::WebInputEvent::Type::kMouseUp, location, location,
              blink::WebPointerProperties::Button::kLeft, 1,
              blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now()));
        }
        tool_result.Set("page_id", *requested_page_id);
        if (requested_frame_id) {
          tool_result.Set("frame_id", *requested_frame_id);
        }
        tool_result.Set("coordinate_space", requested_frame_id
                                                ? "frame_viewport"
                                                : "page_viewport");
        tool_result.Set("x", *x);
        tool_result.Set("y", *y);
        const bool click = tool_request.tool == "page.click";
        tool_result.Set(click ? "clicked" : "hovered", true);
        result_text = click ? "Sent native mouse click events."
                            : "Sent native mouse move event.";
      }
    } else if (result_text.empty() && tool_request.tool == "page.type_text") {
      const std::string* text = tool_request.arguments.FindString("text");
      content::RenderWidgetHostView* view =
          selected_tab->GetContents()->GetRenderWidgetHostView();
      content::RenderWidgetHost* widget =
          view ? view->GetRenderWidgetHost() : nullptr;
      if (!text || !widget) {
        tool_result.Set("error", "Text and an active page are required.");
      } else {
        widget->Focus();
        for (char16_t character : base::UTF8ToUTF16(*text)) {
          blink::WebKeyboardEvent event(blink::WebInputEvent::Type::kChar,
                                        blink::WebInputEvent::kNoModifiers,
                                        base::TimeTicks::Now());
          event.text[0] = character;
          event.unmodified_text[0] = character;
          event.windows_key_code = character;
          widget->ForwardKeyboardEvent(
              input::NativeWebKeyboardEvent(event, view->GetNativeView()));
        }
        tool_result.Set("page_id", *requested_page_id);
        tool_result.Set("characters_sent",
                        static_cast<int>(base::UTF8ToUTF16(*text).size()));
        result_text = "Sent native keyboard character events.";
      }
    } else if (result_text.empty() && tool_request.tool == "page.press") {
      const std::string* key = tool_request.arguments.FindString("key");
      content::RenderWidgetHostView* view =
          selected_tab->GetContents()->GetRenderWidgetHostView();
      content::RenderWidgetHost* widget =
          view ? view->GetRenderWidgetHost() : nullptr;
      std::optional<std::pair<ui::KeyboardCode, ui::DomCode>> key_info;
      if (key) {
        if (*key == "Enter") {
          key_info = {{ui::VKEY_RETURN, ui::DomCode::ENTER}};
        } else if (*key == "Tab") {
          key_info = {{ui::VKEY_TAB, ui::DomCode::TAB}};
        } else if (*key == "Escape") {
          key_info = {{ui::VKEY_ESCAPE, ui::DomCode::ESCAPE}};
        } else if (*key == "Backspace") {
          key_info = {{ui::VKEY_BACK, ui::DomCode::BACKSPACE}};
        } else if (*key == "Delete") {
          key_info = {{ui::VKEY_DELETE, ui::DomCode::DEL}};
        } else if (*key == "ArrowLeft") {
          key_info = {{ui::VKEY_LEFT, ui::DomCode::ARROW_LEFT}};
        } else if (*key == "ArrowRight") {
          key_info = {{ui::VKEY_RIGHT, ui::DomCode::ARROW_RIGHT}};
        } else if (*key == "ArrowUp") {
          key_info = {{ui::VKEY_UP, ui::DomCode::ARROW_UP}};
        } else if (*key == "ArrowDown") {
          key_info = {{ui::VKEY_DOWN, ui::DomCode::ARROW_DOWN}};
        } else if (*key == "Home") {
          key_info = {{ui::VKEY_HOME, ui::DomCode::HOME}};
        } else if (*key == "End") {
          key_info = {{ui::VKEY_END, ui::DomCode::END}};
        } else if (*key == "PageUp") {
          key_info = {{ui::VKEY_PRIOR, ui::DomCode::PAGE_UP}};
        } else if (*key == "PageDown") {
          key_info = {{ui::VKEY_NEXT, ui::DomCode::PAGE_DOWN}};
        } else if (*key == "Space") {
          key_info = {{ui::VKEY_SPACE, ui::DomCode::SPACE}};
        }
      }
      if (!widget || !key_info) {
        tool_result.Set("error",
                        "A live page and supported key name are required.");
      } else {
        widget->Focus();
        const base::TimeTicks timestamp = base::TimeTicks::Now();
        ui::KeyEvent down(ui::EventType::kKeyPressed, key_info->first,
                          key_info->second, ui::EF_NONE, timestamp);
        ui::KeyEvent up(ui::EventType::kKeyReleased, key_info->first,
                        key_info->second, ui::EF_NONE, timestamp);
        widget->ForwardKeyboardEvent(input::NativeWebKeyboardEvent(down));
        widget->ForwardKeyboardEvent(input::NativeWebKeyboardEvent(up));
        tool_result.Set("page_id", *requested_page_id);
        tool_result.Set("key", *key);
        tool_result.Set("pressed", true);
        result_text = "Sent native key press and release events.";
      }
    } else if (result_text.empty() && tool_request.tool == "page.goto") {
      const std::string* url = tool_request.arguments.FindString("url");
      if (!url || !GURL(*url).is_valid()) {
        tool_result.Set("error", "A valid url argument is required.");
      } else {
        selected_tab->GetContents()->GetController().LoadURL(
            GURL(*url), content::Referrer(), ui::PAGE_TRANSITION_TYPED, "");
        tool_result.Set("page_id", *requested_page_id);
        tool_result.Set("url", *url);
        result_text = "Navigation started.";
      }
    } else if (result_text.empty() && tool_request.tool == "page.back") {
      selected_tab->GetContents()->GetController().GoBack();
      tool_result.Set("page_id", *requested_page_id);
      result_text = "Back navigation requested.";
    } else if (result_text.empty() && tool_request.tool == "page.forward") {
      selected_tab->GetContents()->GetController().GoForward();
      tool_result.Set("page_id", *requested_page_id);
      result_text = "Forward navigation requested.";
    } else if (result_text.empty() && tool_request.tool == "page.reload") {
      selected_tab->GetContents()->GetController().Reload(
          content::ReloadType::NORMAL, /*check_for_repost=*/true);
      tool_result.Set("page_id", *requested_page_id);
      result_text = "Reload requested.";
    } else if (result_text.empty() && tool_request.tool == "page.stop") {
      selected_tab->GetContents()->Stop();
      tool_result.Set("page_id", *requested_page_id);
      result_text = "Stopped page loading.";
    } else if (result_text.empty() && tool_request.tool == "page.close") {
      console_observers_.erase(*requested_page_id);
      std::erase_if(frame_handles_, [&](const auto& entry) {
        return entry.second.first == *requested_page_id;
      });
      page_handles_.erase(*requested_page_id);
      page_context_ids_.erase(*requested_page_id);
      selected_browser->GetTabStripModel()->CloseWebContentsAt(
          selected_index, TabCloseTypes::CLOSE_USER_GESTURE);
      tool_result.Set("page_id", *requested_page_id);
      tool_result.Set("closed", true);
      result_text = "Closed page.";
    } else if (result_text.empty() && tool_request.tool == "page.focus") {
      selected_browser->GetTabStripModel()->ActivateTabAt(selected_index);
      selected_browser->GetWindow()->Activate();
      tool_result.Set("page_id", *requested_page_id);
      tool_result.Set("focused", true);
      result_text = "Focused page.";
    } else if (result_text.empty() &&
               tool_request.tool == "page.set_user_agent") {
      const std::string* user_agent =
          tool_request.arguments.FindString("user_agent");
      if (!user_agent || (!user_agent->empty() &&
                          !net::HttpUtil::IsValidHeaderValue(*user_agent))) {
        tool_result.Set("error", "A valid user_agent string is required.");
        result_text = "Invalid user_agent string.";
        result.Set("isError", true);
      } else {
        selected_tab->GetContents()->SetUserAgentOverride(
            blink::UserAgentOverride::UserAgentOnly(*user_agent),
            /*override_in_new_tabs=*/true);
        content::NavigationEntry* entry = selected_tab->GetContents()
                                              ->GetController()
                                              .GetLastCommittedEntry();
        if (entry) {
          entry->SetIsOverridingUserAgent(!user_agent->empty());
        }
        tool_result.Set("page_id", *requested_page_id);
        tool_result.Set("user_agent", *user_agent);
        tool_result.Set("reload_required", true);
        result_text = user_agent->empty()
                          ? "Cleared the page user-agent override."
                          : "Set the native page user-agent override.";
      }
    } else if (result_text.empty() &&
               (tool_request.tool == "page.accept_dialog" ||
                tool_request.tool == "page.dismiss_dialog")) {
      content::WebContents* contents = selected_tab->GetContents();
      content::JavaScriptDialogManager* manager =
          contents->GetDelegate()
              ? contents->GetDelegate()->GetJavaScriptDialogManager(contents)
              : nullptr;
      const bool accept = tool_request.tool == "page.accept_dialog";
      const std::string* prompt_text =
          tool_request.arguments.FindString("prompt_text");
      std::u16string prompt_override =
          prompt_text ? base::UTF8ToUTF16(*prompt_text) : std::u16string();
      const bool handled =
          manager &&
          manager->HandleJavaScriptDialog(
              contents, accept, prompt_text ? &prompt_override : nullptr);
      if (!handled) {
        tool_result.Set("error", "No active JavaScript dialog on this page.");
      } else {
        tool_result.Set("page_id", *requested_page_id);
        tool_result.Set("accepted", accept);
        tool_result.Set("handled", true);
        result_text = accept ? "Accepted JavaScript dialog."
                             : "Dismissed JavaScript dialog.";
      }
    } else if (result_text.empty()) {
      return MakeJsonRpcError(id->Clone(), -32602, "Unknown tool.");
    }
    if (result_text.empty()) {
      result.Set("isError", true);
      result_text = *tool_result.FindString("error");
    }
  }
  if (!early_response.empty()) {
    return early_response;
  }
  base::ListValue content_items;
  base::DictValue text_content;
  text_content.Set("type", "text");
  text_content.Set("text", result_text);
  content_items.Append(std::move(text_content));
  result.Set("content", std::move(content_items));
  if (result.FindBool("isError").value_or(false)) {
    tool_result.Set("error_code", ToolErrorCode(result_text));
  }
  result.Set("structuredContent", std::move(tool_result));

  base::DictValue server_info;
  server_info.Set("name", kServerName);
  server_info.Set("version", kServerVersion);
  result.Set("_meta",
             base::DictValue().Set("io.modelcontextprotocol/serverInfo",
                                   std::move(server_info)));
  response.Set("result", std::move(result));
  return base::WriteJson(base::Value(std::move(response))).value_or("{}");
}

void BrowserAgentMcpServer::MaybeDestroyOwnedContext(
    std::string context_id,
    base::WeakPtr<Profile> context_profile) {
  auto pending = pending_context_destructions_.find(context_id);
  if (pending == pending_context_destructions_.end()) {
    return;
  }
  Profile* target = context_profile.get();
  if (!target) {
    pending_context_destructions_.erase(context_id);
    return;
  }
  bool has_open_windows = false;
  if (ProfileBrowserCollection* browsers =
          ProfileBrowserCollection::GetForProfile(target)) {
    browsers->ForEach([&](BrowserWindowInterface*) {
      has_open_windows = true;
      return false;
    });
  }
  if (has_open_windows) {
    if (base::TimeTicks::Now() >= pending->second) {
      // Closure was refused or stalled. Keep the context and its handles live
      // so the client can observe the failure and retry after resolving it.
      pending_context_destructions_.erase(pending);
      return;
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&BrowserAgentMcpServer::MaybeDestroyOwnedContext,
                       weak_factory_.GetWeakPtr(), std::move(context_id),
                       std::move(context_profile)),
        base::Milliseconds(100));
    return;
  }
  pending_context_destructions_.erase(pending);
  std::erase_if(context_handles_,
                [target](const auto& entry) { return entry.second == target; });
  owned_context_ids_.erase(context_id);
  std::erase_if(page_context_ids_, [&](const auto& entry) {
    if (entry.second != context_id) {
      return false;
    }
    page_handles_.erase(entry.first);
    console_observers_.erase(entry.first);
    std::erase_if(frame_handles_, [&](const auto& frame_entry) {
      return frame_entry.second.first == entry.first;
    });
    return true;
  });
  destroying_context_profiles_.insert(target);
  ProfileDestroyer::DestroyOTRProfileWhenAppropriateWithTimeout(
      target, base::Seconds(60));
}

void BrowserAgentMcpServer::ForgetDestroyedContextProfile(Profile* profile) {
  const auto profiles = profile_->GetAllOffTheRecordProfiles();
  if (std::ranges::find(profiles, profile) == profiles.end()) {
    destroying_context_profiles_.erase(profile);
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&BrowserAgentMcpServer::ForgetDestroyedContextProfile,
                     weak_factory_.GetWeakPtr(), profile),
      base::Milliseconds(100));
}

}  // namespace browser_agent
