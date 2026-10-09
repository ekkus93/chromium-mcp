// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_mcp_server_protocol.h"

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

const char* DownloadStateName(download::DownloadItem::DownloadState state) {
  switch (state) {
    case download::DownloadItem::IN_PROGRESS:
      return "in_progress";
    case download::DownloadItem::COMPLETE:
      return "complete";
    case download::DownloadItem::CANCELLED:
      return "cancelled";
    case download::DownloadItem::INTERRUPTED:
      return "interrupted";
    case download::DownloadItem::MAX_DOWNLOAD_STATE:
      return "unknown";
  }
  return "unknown";
}

std::optional<ContentSettingsType> PermissionType(std::string_view name) {
  if (name == "geolocation") {
    return ContentSettingsType::GEOLOCATION;
  }
  if (name == "notifications") {
    return ContentSettingsType::NOTIFICATIONS;
  }
  if (name == "camera") {
    return ContentSettingsType::MEDIASTREAM_CAMERA;
  }
  if (name == "microphone") {
    return ContentSettingsType::MEDIASTREAM_MIC;
  }
  if (name == "popups") {
    return ContentSettingsType::POPUPS;
  }
  for (const content_settings::ContentSettingsInfo* info :
       *content_settings::ContentSettingsRegistry::GetInstance()) {
    if (info->website_settings_info()->name() == name) {
      return info->website_settings_info()->type();
    }
  }
  return std::nullopt;
}

std::optional<ContentSetting> PermissionSetting(std::string_view setting) {
  if (setting == "allow") {
    return CONTENT_SETTING_ALLOW;
  }
  if (setting == "block") {
    return CONTENT_SETTING_BLOCK;
  }
  if (setting == "ask") {
    return CONTENT_SETTING_ASK;
  }
  if (setting == "session_only") {
    return CONTENT_SETTING_SESSION_ONLY;
  }
  return std::nullopt;
}

const char* PermissionSettingName(ContentSetting setting) {
  switch (setting) {
    case CONTENT_SETTING_ALLOW:
      return "allow";
    case CONTENT_SETTING_BLOCK:
      return "block";
    case CONTENT_SETTING_ASK:
      return "ask";
    case CONTENT_SETTING_DEFAULT:
      return "default";
    case CONTENT_SETTING_SESSION_ONLY:
      return "session_only";
    case CONTENT_SETTING_NUM_SETTINGS:
      return "unknown";
  }
  return "unknown";
}

const char* PermissionStatusName(blink::mojom::PermissionStatus status) {
  switch (status) {
    case blink::mojom::PermissionStatus::GRANTED:
      return "granted";
    case blink::mojom::PermissionStatus::DENIED:
      return "denied";
    case blink::mojom::PermissionStatus::ASK:
      return "ask";
  }
  return "unknown";
}

namespace {
constexpr char kServerName[] = "Chromium Browser";
constexpr char kServerVersion[] = "0.1.0";
}  // namespace

const char* ToolErrorCode(std::string_view message) {
  if (message.find("timed out") != std::string_view::npos ||
      message.find("timeout") != std::string_view::npos) {
    return "timeout";
  }
  if (message.find("Unknown or closed") != std::string_view::npos ||
      message.find("No active") != std::string_view::npos) {
    return "not_found";
  }
  if (message.find("required") != std::string_view::npos ||
      message.find("must be") != std::string_view::npos ||
      message.find("Only the") != std::string_view::npos) {
    return "invalid_argument";
  }
  return "browser_error";
}

std::string MakeToolCallResponse(base::Value request_id,
                                 base::DictValue structured_content,
                                 std::string text,
                                 bool is_error) {
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  base::DictValue result;
  result.Set("resultType", "complete");
  if (is_error) {
    result.Set("isError", true);
    const std::string* message = structured_content.FindString("error");
    structured_content.Set("error_code",
                           ToolErrorCode(message ? *message : text));
  }
  base::ListValue content;
  base::DictValue text_content;
  text_content.Set("type", "text");
  text_content.Set("text", std::move(text));
  content.Append(std::move(text_content));
  result.Set("content", std::move(content));
  result.Set("structuredContent", std::move(structured_content));
  response.Set("result", std::move(result));
  return base::WriteJson(base::Value(std::move(response))).value_or("{}");
}

std::string MakeJsonRpcError(base::Value request_id,
                             int code,
                             std::string message) {
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  base::DictValue error;
  error.Set("code", code);
  error.Set("message", std::move(message));
  response.Set("error", std::move(error));
  return base::WriteJson(base::Value(std::move(response))).value_or("{}");
}

std::string MakeImageToolCallResponse(base::Value request_id,
                                      std::string image_data,
                                      int width,
                                      int height) {
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  base::DictValue result;
  result.Set("resultType", "complete");
  base::ListValue content;
  base::DictValue text_content;
  text_content.Set("type", "text");
  text_content.Set("text", "Captured the page surface.");
  content.Append(std::move(text_content));
  base::DictValue image_content;
  image_content.Set("type", "image");
  image_content.Set("mimeType", "image/png");
  image_content.Set("data", std::move(image_data));
  content.Append(std::move(image_content));
  result.Set("content", std::move(content));
  base::DictValue structured;
  structured.Set("width", width);
  structured.Set("height", height);
  result.Set("structuredContent", std::move(structured));
  response.Set("result", std::move(result));
  return base::WriteJson(base::Value(std::move(response))).value_or("{}");
}

std::string BrowserAgentMcpServer::HandleJsonRpc(
    const base::DictValue& request) {
  const base::Value* id = request.Find("id");
  const std::string* method = request.FindString("method");
  const bool valid_id =
      id && (id->is_int() || id->is_double() || id->is_string());
  const std::string* jsonrpc = request.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0" || !valid_id || !method) {
    return MakeJsonRpcError(valid_id ? id->Clone() : base::Value(), -32600,
                            "Invalid JSON-RPC request.");
  }
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", id->Clone());

  base::DictValue result;
  result.Set("resultType", "complete");
  if (method && *method == "server/discover") {
    base::ListValue supported_versions;
    supported_versions.Append(protocol::kMcpProtocolVersion);
    result.Set("supportedVersions", std::move(supported_versions));
    base::DictValue capabilities;
    capabilities.Set("tools", base::DictValue());
    base::DictValue resource_capabilities;
    resource_capabilities.Set("subscribe", true);
    capabilities.Set("resources", std::move(resource_capabilities));
    result.Set("capabilities", std::move(capabilities));
    result.Set("instructions",
               "Use Chromium-native browser tools to control this profile.");
    result.Set("ttlMs", 0);
    result.Set("cacheScope", "private");
  } else if (method && *method == "tools/list") {
    base::ListValue tools;
    tools.Append(MakeTool("browser.version",
                          "Return Chromium version metadata.",
                          EmptyObjectSchema()));
    tools.Append(MakeTool("browser.list_contexts",
                          "List regular and existing private contexts.",
                          EmptyObjectSchema()));
    tools.Append(MakeTool("browser.create_context",
                          "Create a private browser context.",
                          EmptyObjectSchema()));
    tools.Append(MakeTool("browser.close_context",
                          "Close a private browser context.",
                          ObjectSchema({"context_id"}, {"context_id"})));
    tools.Append(MakeTool("context.storage_state",
                          "Read cookie storage state for a context.",
                          ObjectSchema({"context_id"})));
    tools.Append(MakeTool("context.clear_http_cache",
                          "Clear the context's native HTTP cache.",
                          ObjectSchema({"context_id"})));
    tools.Append(MakeTool("context.set_accept_language",
                          "Set the native context Accept-Language header.",
                          SetLanguageSchema()));
    tools.Append(MakeTool("context.set_referrers_enabled",
                          "Enable or disable native referrer headers.",
                          SetBooleanContextSchema()));
    tools.Append(MakeTool("context.close_idle_connections",
                          "Close idle native network connections.",
                          ObjectSchema({"context_id"})));
    tools.Append(MakeTool("context.local_storage_origins",
                          "List native local storage keys and sizes.",
                          ObjectSchema({"context_id"})));
    tools.Append(
        MakeTool("context.clear_local_storage",
                 "Delete local storage for a native storage key.",
                 ObjectSchema({"context_id", "storage_key"}, {"storage_key"})));
    tools.Append(
        MakeTool("context.set_cookies",
                 "Set cookies through the native context cookie manager.",
                 SetCookiesSchema()));
    tools.Append(
        MakeTool("context.downloads",
                 "List downloads from the native context download manager.",
                 ObjectSchema({"context_id"})));
    tools.Append(MakeTool("context.permission",
                          "Read a native site content permission setting.",
                          ObjectSchema({"context_id", "url", "permission"},
                                       {"url", "permission"})));
    tools.Append(MakeTool("context.list_permissions",
                          "List registered native content setting names.",
                          EmptyObjectSchema()));
    tools.Append(MakeTool(
        "context.set_permission", "Set a native site content permission.",
        ObjectSchema({"context_id", "url", "permission", "setting"},
                     {"url", "permission", "setting"})));
    tools.Append(MakeTool("download.cancel",
                          "Cancel a native browser download by GUID.",
                          ObjectSchema({"context_id", "guid"}, {"guid"})));
    tools.Append(MakeTool("download.wait",
                          "Wait for a native download to finish or stop.",
                          ObjectSchema({"context_id", "guid"}, {"guid"})));
    tools.Append(MakeTool("page.list", "List open pages in this profile.",
                          ObjectSchema({"context_id"})));
    tools.Append(MakeTool("page.new", "Open a new page.",
                          ObjectSchema({"context_id", "url"})));
    tools.Append(MakeTool("page.get_info",
                          "Read page URL, title, and load state.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(
        MakeTool("page.set_viewport_size",
                 "Resize a page viewport using the native WebContents API.",
                 ViewportSchema()));
    tools.Append(MakeTool("page.screenshot",
                          "Capture a page as an MCP PNG image.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool(
        "page.download", "Download a URL through the page's native context.",
        ObjectSchema({"page_id", "url", "path"}, {"page_id", "url"})));
    tools.Append(MakeTool("page.prepare_file_upload",
                          "Supply local files to the next native file chooser.",
                          PrepareFileUploadSchema()));
    tools.Append(MakeTool(
        "page.permission_status",
        "Read browser and OS/device permission status.",
        ObjectSchema({"page_id", "permission"}, {"page_id", "permission"})));
    tools.Append(MakeTool("page.accessibility_snapshot",
                          "Capture the native accessibility node tree.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.visible_content",
                          "Read visible text, headings, and links.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(
        MakeTool("page.storage_state",
                 "Read local and session storage for a page frame origin.",
                 ObjectSchema({"page_id", "frame_id"}, {"page_id"})));
    tools.Append(MakeTool("page.set_storage",
                          "Set local or session storage string entries.",
                          SetStorageSchema()));
    tools.Append(MakeTool("page.console_messages",
                          "Read and clear observed console messages.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.network_events",
                          "Read and clear completed resource load events.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.frames", "List the current page frames.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.find_elements",
                          "Find viewport-intersecting elements by CSS, role, name, or text; report whether each center point is unobscured.",
                          ObjectSchema({"page_id", "frame_id", "selector",
                                        "role", "name", "text"},
                                       {"page_id"})));
    tools.Append(
        MakeTool("page.wait_for_load_state",
                 "Wait for a page to finish loading (30 second timeout).",
                 ObjectSchema({"page_id", "state"}, {"page_id"})));
    tools.Append(
        MakeTool("page.wait_for_url",
                 "Wait for an exact URL navigation commit (30 second timeout).",
                 ObjectSchema({"page_id", "url"}, {"page_id", "url"})));
    tools.Append(MakeTool("page.evaluate",
                          "Evaluate JavaScript in the page's main world.",
                          ObjectSchema({"page_id", "frame_id", "expression"},
                                       {"page_id", "expression"})));
    tools.Append(
        MakeTool("page.fill",
                 "Fill a supported writable input, textarea, or contenteditable element and verify its resulting value. Dispatches bubbling input and change events.",
                 ObjectSchema({"page_id", "selector", "value"},
                              {"page_id", "selector", "value"})));
    tools.Append(MakeTool(
        "page.focus_element", "Focus a CSS-selected element in the page.",
        ObjectSchema({"page_id", "selector"}, {"page_id", "selector"})));
    tools.Append(MakeTool("page.select_option",
                          "Select an option by value in a CSS-selected select.",
                          ObjectSchema({"page_id", "selector", "value"},
                                       {"page_id", "selector", "value"})));
    tools.Append(MakeTool("page.click",
                          "Send a native left click at viewport coordinates.",
                          PointSchema()));
    tools.Append(MakeTool("page.hover",
                          "Send a native mouse move to viewport coordinates.",
                          PointSchema()));
    tools.Append(MakeTool(
        "page.check", "Check a CSS-selected checkbox with native input.",
        ObjectSchema({"page_id", "selector"}, {"page_id", "selector"})));
    tools.Append(MakeTool(
        "page.uncheck", "Uncheck a CSS-selected checkbox with native input.",
        ObjectSchema({"page_id", "selector"}, {"page_id", "selector"})));
    tools.Append(
        MakeTool("page.type_text",
                 "Send text through Chromium's native keyboard input path.",
                 ObjectSchema({"page_id", "text"}, {"page_id", "text"})));
    tools.Append(MakeTool(
        "page.press", "Press a supported named key using native input.",
        ObjectSchema({"page_id", "key"}, {"page_id", "key"})));
    tools.Append(
        MakeTool("page.goto", "Navigate a page to a URL.",
                 ObjectSchema({"page_id", "url"}, {"page_id", "url"})));
    tools.Append(MakeTool("page.back", "Navigate back in page history.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.forward", "Navigate forward in page history.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.reload", "Reload the current page.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.stop", "Stop page loading.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.close", "Close a page.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.focus", "Activate a page and its window.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool(
        "page.set_user_agent",
        "Set or clear the native page user-agent override.",
        ObjectSchema({"page_id", "user_agent"}, {"page_id", "user_agent"})));
    tools.Append(MakeTool(
        "page.accept_dialog", "Accept the active JavaScript dialog or prompt.",
        ObjectSchema({"page_id", "prompt_text"}, {"page_id"})));
    tools.Append(MakeTool("page.dialog_info",
                          "Read the active tab-modal JavaScript dialog.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    tools.Append(MakeTool("page.dismiss_dialog",
                          "Dismiss the active JavaScript dialog or prompt.",
                          ObjectSchema({"page_id"}, {"page_id"})));
    if (!trace_directory_.empty()) {
      tools.Append(MakeTool("browser.start_trace",
                            "Start a native Chromium trace.",
                            ObjectSchema({"categories"})));
      tools.Append(MakeTool("browser.stop_trace",
                            "Stop tracing and write the trace JSON file.",
                            EmptyObjectSchema()));
    }
    result.Set("tools", std::move(tools));
    result.Set("ttlMs", 0);
    result.Set("cacheScope", "private");
  } else if (method && *method == "resources/list") {
    base::ListValue resources;
    base::DictValue version_resource;
    version_resource.Set("uri", "chromium-mcp://browser/version");
    version_resource.Set("name", "Chromium version");
    version_resource.Set("description",
                         "Version of the running Chromium browser.");
    version_resource.Set("mimeType", "application/json");
    resources.Append(std::move(version_resource));
    base::DictValue contexts_resource;
    contexts_resource.Set("uri", "chromium-mcp://browser/contexts");
    contexts_resource.Set("name", "Browser contexts");
    contexts_resource.Set("description",
                          "Regular and private profile contexts.");
    contexts_resource.Set("mimeType", "application/json");
    resources.Append(std::move(contexts_resource));
    base::DictValue events_resource;
    events_resource.Set("uri", "chromium-mcp://browser/events");
    events_resource.Set("name", "Browser events");
    events_resource.Set(
        "description",
        "Bounded browser event journal. Read with ?after=<cursor> to continue.");
    events_resource.Set("mimeType", "application/json");
    events_resource.Set("subscribe", true);
    resources.Append(std::move(events_resource));
    result.Set("resources", std::move(resources));
    result.Set("ttlMs", 0);
    result.Set("cacheScope", "private");
  } else if (method && *method == "resources/read") {
    const base::DictValue* params = request.FindDict("params");
    const std::string* uri = params ? params->FindString("uri") : nullptr;
    if (!uri || (*uri != "chromium-mcp://browser/version" &&
                 *uri != "chromium-mcp://browser/contexts" &&
                 !base::StartsWith(*uri, "chromium-mcp://browser/events"))) {
      return MakeJsonRpcError(id->Clone(), -32602, "Resource not found.");
    }
    base::DictValue content;
    content.Set("uri", *uri);
    content.Set("mimeType", "application/json");
    base::DictValue resource_data;
    if (*uri == "chromium-mcp://browser/version") {
      resource_data.Set("product", std::string(version_info::GetProductName()));
      resource_data.Set("version",
                        std::string(version_info::GetVersionNumber()));
      resource_data.Set("mcp_protocol_version", protocol::kMcpProtocolVersion);
    } else if (*uri == "chromium-mcp://browser/contexts") {
      base::ListValue contexts;
      base::DictValue regular;
      regular.Set("context_id", default_context_id_);
      regular.Set("off_the_record", false);
      contexts.Append(std::move(regular));
      for (Profile* off_the_record : profile_->GetAllOffTheRecordProfiles()) {
        base::DictValue context;
        context.Set("context_id", RegisterContext(off_the_record));
        context.Set("off_the_record", true);
        contexts.Append(std::move(context));
      }
      resource_data.Set("contexts", std::move(contexts));
    } else {
      int64_t after_cursor = 0;
      const size_t cursor_position = uri->find("?after=");
      if (cursor_position != std::string::npos) {
        base::StringToInt64(uri->substr(cursor_position + 7), &after_cursor);
      }
      base::ListValue events;
      for (const base::Value& entry : browser_events_) {
        const base::DictValue* event = entry.GetIfDict();
        const std::optional<double> cursor =
            event ? event->FindDouble("cursor") : std::nullopt;
        if (cursor && *cursor > after_cursor) {
          events.Append(entry.Clone());
        }
      }
      resource_data.Set("events", std::move(events));
      resource_data.Set("next_cursor",
                        static_cast<double>(next_browser_event_cursor_ - 1));
    }
    content.Set(
        "text",
        base::WriteJson(base::Value(std::move(resource_data))).value_or("{}"));
    base::ListValue contents;
    contents.Append(std::move(content));
    result.Set("contents", std::move(contents));
    result.Set("ttlMs", 0);
    result.Set("cacheScope", "private");
  } else if (method && *method == "tools/call") {
    return HandleToolCall(id, request);
  } else {
    return MakeJsonRpcError(id->Clone(), -32601, "Method not found.");
  }

  base::DictValue server_info;
  server_info.Set("name", kServerName);
  server_info.Set("version", kServerVersion);
  result.Set("_meta",
             base::DictValue().Set("io.modelcontextprotocol/serverInfo",
                                   std::move(server_info)));
  response.Set("result", std::move(result));
  return base::WriteJson(base::Value(std::move(response))).value_or("{}");
}

}  // namespace browser_agent
