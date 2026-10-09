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

base::DictValue MakeTool(std::string name,
                         std::string description,
                         base::DictValue input_schema) {
  base::DictValue tool;
  tool.Set("name", std::move(name));
  tool.Set("description", std::move(description));
  tool.Set("inputSchema", std::move(input_schema));
  return tool;
}

base::DictValue EmptyObjectSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  schema.Set("properties", base::DictValue());
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue StringPropertySchema() {
  base::DictValue schema;
  schema.Set("type", "string");
  return schema;
}

base::DictValue NumberPropertySchema() {
  base::DictValue schema;
  schema.Set("type", "number");
  return schema;
}

base::DictValue BooleanPropertySchema() {
  base::DictValue schema;
  schema.Set("type", "boolean");
  return schema;
}

base::DictValue IntegerPropertySchema() {
  base::DictValue schema;
  schema.Set("type", "integer");
  return schema;
}

base::DictValue SetBooleanContextSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("context_id", StringPropertySchema());
  properties.Set("enabled", BooleanPropertySchema());
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("enabled");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue SetLanguageSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("context_id", StringPropertySchema());
  properties.Set("accept_language", StringPropertySchema());
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("accept_language");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue ObjectSchema(std::initializer_list<const char*> properties,
                             std::initializer_list<const char*> required) {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue property_schemas;
  for (const char* property : properties) {
    property_schemas.Set(property, StringPropertySchema());
  }
  schema.Set("properties", std::move(property_schemas));
  base::ListValue required_properties;
  for (const char* property : required) {
    required_properties.Append(property);
  }
  if (!required_properties.empty()) {
    schema.Set("required", std::move(required_properties));
  }
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue PointSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("page_id", StringPropertySchema());
  properties.Set("x", NumberPropertySchema());
  properties.Set("y", NumberPropertySchema());
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("page_id");
  required.Append("x");
  required.Append("y");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue SetCookiesSchema() {
  base::DictValue cookie;
  cookie.Set("type", "object");
  base::DictValue cookie_properties;
  for (const char* property :
       {"url", "name", "value", "domain", "path", "same_site"}) {
    cookie_properties.Set(property, StringPropertySchema());
  }
  for (const char* property : {"secure", "http_only"}) {
    base::DictValue boolean_schema;
    boolean_schema.Set("type", "boolean");
    cookie_properties.Set(property, std::move(boolean_schema));
  }
  cookie_properties.Set("expires_unix_ms", NumberPropertySchema());
  cookie.Set("properties", std::move(cookie_properties));
  base::ListValue cookie_required;
  cookie_required.Append("url");
  cookie_required.Append("name");
  cookie_required.Append("value");
  cookie.Set("required", std::move(cookie_required));
  cookie.Set("additionalProperties", false);

  base::DictValue cookies_array;
  cookies_array.Set("type", "array");
  cookies_array.Set("items", std::move(cookie));

  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("context_id", StringPropertySchema());
  properties.Set("cookies", std::move(cookies_array));
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("cookies");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue SetStorageSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("page_id", StringPropertySchema());
  properties.Set("frame_id", StringPropertySchema());
  for (const char* area : {"local", "session"}) {
    base::DictValue area_schema;
    area_schema.Set("type", "object");
    area_schema.Set("additionalProperties", StringPropertySchema());
    properties.Set(area, std::move(area_schema));
  }
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("page_id");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue ViewportSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("page_id", StringPropertySchema());
  properties.Set("width", IntegerPropertySchema());
  properties.Set("height", IntegerPropertySchema());
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("page_id");
  required.Append("width");
  required.Append("height");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::DictValue PrepareFileUploadSchema() {
  base::DictValue schema;
  schema.Set("type", "object");
  base::DictValue properties;
  properties.Set("page_id", StringPropertySchema());
  base::DictValue paths;
  paths.Set("type", "array");
  paths.Set("items", StringPropertySchema());
  properties.Set("paths", std::move(paths));
  schema.Set("properties", std::move(properties));
  base::ListValue required;
  required.Append("page_id");
  required.Append("paths");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

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
    capabilities.Set("resources", base::DictValue());
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
                          "Find visible elements by CSS, role, name, or text.",
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
                 "Fill a CSS-selected input, textarea, or editable element.",
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
    result.Set("resources", std::move(resources));
    result.Set("ttlMs", 0);
    result.Set("cacheScope", "private");
  } else if (method && *method == "resources/read") {
    const base::DictValue* params = request.FindDict("params");
    const std::string* uri = params ? params->FindString("uri") : nullptr;
    if (!uri || (*uri != "chromium-mcp://browser/version" &&
                 *uri != "chromium-mcp://browser/contexts")) {
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
    } else {
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
    protocol::Request tool_request;
    protocol::Error parse_error;
    if (!protocol::ParseToolCall(request, &tool_request, &parse_error)) {
      return MakeJsonRpcError(id->Clone(), parse_error.code,
                              parse_error.message);
    }
    base::DictValue tool_result;
    std::string result_text;
    if (tool_request.tool == "browser.version") {
      tool_result.Set("product", std::string(version_info::GetProductName()));
      tool_result.Set("version", std::string(version_info::GetVersionNumber()));
      tool_result.Set("mcp_protocol_version", protocol::kMcpProtocolVersion);
      result_text = base::StrCat({*tool_result.FindString("product"), " ",
                                  *tool_result.FindString("version")});
    } else if (tool_request.tool == "browser.list_contexts") {
      base::ListValue contexts;
      base::DictValue regular_context;
      regular_context.Set("context_id", default_context_id_);
      regular_context.Set("off_the_record", false);
      contexts.Append(std::move(regular_context));
      for (Profile* off_the_record : profile_->GetAllOffTheRecordProfiles()) {
        base::DictValue context;
        context.Set("context_id", RegisterContext(off_the_record));
        context.Set("off_the_record", true);
        contexts.Append(std::move(context));
      }
      tool_result.Set("contexts", std::move(contexts));
      result_text = "Listed browser contexts.";
    } else if (tool_request.tool == "browser.create_context") {
      Profile* off_the_record = profile_->GetPrimaryOTRProfile(true);
      tool_result.Set("context_id", RegisterContext(off_the_record));
      tool_result.Set("off_the_record", true);
      result_text = "Created a private browser context.";
    } else if (tool_request.tool == "browser.close_context") {
      const std::string* context_id =
          tool_request.arguments.FindString("context_id");
      Profile* target = context_id ? ResolveContext(*context_id) : nullptr;
      if (!target || target == profile_) {
        tool_result.Set("error", "A live private context_id is required.");
      } else {
        ProfileBrowserCollection* context_browsers =
            ProfileBrowserCollection::GetForProfile(target);
        std::vector<BrowserWindowInterface*> open_browsers;
        if (context_browsers) {
          context_browsers->ForEach([&](BrowserWindowInterface* browser) {
            open_browsers.push_back(browser);
            return true;
          });
        }
        std::erase_if(context_handles_, [&](const auto& entry) {
          return entry.second == target;
        });
        std::erase_if(page_context_ids_, [&](const auto& entry) {
          if (entry.second != *context_id) {
            return false;
          }
          page_handles_.erase(entry.first);
          console_observers_.erase(entry.first);
          std::erase_if(frame_handles_, [&](const auto& frame_entry) {
            return frame_entry.second.first == entry.first;
          });
          return true;
        });
        for (BrowserWindowInterface* browser : open_browsers) {
          browser->GetWindow()->Close();
        }
        if (open_browsers.empty()) {
          profile_->DestroyOffTheRecordProfile(target);
        }
        tool_result.Set("context_id", *context_id);
        tool_result.Set("closed", true);
        result_text = "Closed private browser context.";
      }
      if (result_text.empty()) {
        result.Set("isError", true);
        result_text = *tool_result.FindString("error");
      }
    } else if (tool_request.tool == "context.downloads") {
      const std::string* requested_context_id =
          tool_request.arguments.FindString("context_id");
      const std::string context_id =
          requested_context_id ? *requested_context_id : default_context_id_;
      Profile* context = ResolveContext(context_id);
      content::DownloadManager* manager =
          context ? context->GetDownloadManager() : nullptr;
      if (!manager) {
        tool_result.Set("error", "Unknown or closed context_id.");
        result.Set("isError", true);
        result_text = *tool_result.FindString("error");
      } else {
        download::SimpleDownloadManager::DownloadVector downloads;
        manager->GetAllDownloads(&downloads);
        base::ListValue items;
        for (download::DownloadItem* download : downloads) {
          base::DictValue item;
          item.Set("guid", download->GetGuid());
          item.Set("url", download->GetURL().spec());
          item.Set("target_path", download->GetTargetFilePath().AsUTF8Unsafe());
          item.Set("received_bytes",
                   static_cast<double>(download->GetReceivedBytes()));
          item.Set("total_bytes",
                   static_cast<double>(download->GetTotalBytes()));
          item.Set("state", DownloadStateName(download->GetState()));
          items.Append(std::move(item));
        }
        tool_result.Set("context_id", context_id);
        tool_result.Set("downloads", std::move(items));
        result_text = "Listed context downloads.";
      }
    } else if (tool_request.tool == "context.list_permissions") {
      base::ListValue permissions;
      for (const content_settings::ContentSettingsInfo* info :
           *content_settings::ContentSettingsRegistry::GetInstance()) {
        base::DictValue item;
        item.Set("name", info->website_settings_info()->name());
        base::ListValue settings;
        for (ContentSetting candidate :
             {CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK, CONTENT_SETTING_ASK,
              CONTENT_SETTING_SESSION_ONLY}) {
          if (info->IsSettingValid(candidate)) {
            settings.Append(PermissionSettingName(candidate));
          }
        }
        item.Set("settings", std::move(settings));
        permissions.Append(std::move(item));
      }
      tool_result.Set("permissions", std::move(permissions));
      result_text = "Listed native content settings.";
    } else if (tool_request.tool == "context.permission" ||
               tool_request.tool == "context.set_permission") {
      const std::string* requested_context_id =
          tool_request.arguments.FindString("context_id");
      const std::string context_id =
          requested_context_id ? *requested_context_id : default_context_id_;
      const std::string* url_text = tool_request.arguments.FindString("url");
      const std::string* permission_name =
          tool_request.arguments.FindString("permission");
      const std::string* setting_name =
          tool_request.arguments.FindString("setting");
      Profile* context = ResolveContext(context_id);
      const GURL url = url_text ? GURL(*url_text) : GURL();
      const std::optional<ContentSettingsType> type =
          permission_name ? PermissionType(*permission_name) : std::nullopt;
      const bool set_permission = tool_request.tool == "context.set_permission";
      const std::optional<ContentSetting> desired_setting =
          setting_name ? PermissionSetting(*setting_name) : std::nullopt;
      const content_settings::ContentSettingsInfo* setting_info =
          type ? content_settings::ContentSettingsRegistry::GetInstance()->Get(
                     *type)
               : nullptr;
      if (!context || !url.is_valid() || !type ||
          (set_permission &&
           (!desired_setting || !setting_info ||
            !setting_info->IsSettingValid(*desired_setting)))) {
        tool_result.Set(
            "error",
            "A live context, URL, permission, and valid setting are required.");
        result.Set("isError", true);
        result_text = *tool_result.FindString("error");
      } else {
        HostContentSettingsMap* settings_map =
            HostContentSettingsMapFactory::GetForProfile(context);
        if (set_permission) {
          settings_map->SetContentSettingDefaultScope(url, url, *type,
                                                      *desired_setting);
        }
        tool_result.Set("context_id", context_id);
        tool_result.Set("url", url.spec());
        tool_result.Set("permission", *permission_name);
        tool_result.Set("setting",
                        PermissionSettingName(
                            settings_map->GetContentSetting(url, url, *type)));
        result_text = set_permission ? "Set native site permission."
                                     : "Read native site permission.";
      }
    } else if (tool_request.tool == "download.cancel") {
      const std::string* requested_context_id =
          tool_request.arguments.FindString("context_id");
      const std::string context_id =
          requested_context_id ? *requested_context_id : default_context_id_;
      const std::string* guid = tool_request.arguments.FindString("guid");
      Profile* context = ResolveContext(context_id);
      content::DownloadManager* manager =
          context ? context->GetDownloadManager() : nullptr;
      download::DownloadItem* item =
          manager && guid ? manager->GetDownloadByGuid(*guid) : nullptr;
      if (!item) {
        tool_result.Set("error",
                        "A live context_id and download guid are required.");
        result.Set("isError", true);
        result_text = *tool_result.FindString("error");
      } else {
        item->Cancel(/*user_cancel=*/true);
        tool_result.Set("guid", *guid);
        tool_result.Set("cancel_requested", true);
        tool_result.Set("cancelled",
                        item->GetState() == download::DownloadItem::CANCELLED);
        result_text = "Requested native download cancellation.";
      }
    } else {
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
            page.Set("title",
                     base::UTF16ToUTF8(tab->GetContents()->GetTitle()));
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
              page_id, std::make_unique<BrowserAgentConsoleObserver>(
                           tab->GetContents(),
                           language == context_accept_languages_.end()
                               ? std::string()
                               : language->second));
          if (const std::string* url =
                  tool_request.arguments.FindString("url")) {
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
        const std::optional<int> width =
            tool_request.arguments.FindInt("width");
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
      } else if (result_text.empty() &&
                 tool_request.tool == "page.dialog_info") {
        tool_result.Set("page_id", *requested_page_id);
        auto* manager =
            javascript_dialogs::TabModalDialogManager::FromWebContents(
                selected_tab->GetContents());
        const auto info =
            manager ? manager->GetActiveDialogInfo() : std::nullopt;
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
            tool_result.Set(
                "default_prompt",
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
        content::RenderWidgetHostView* view =
            selected_tab->GetContents()->GetRenderWidgetHostView();
        content::RenderWidgetHost* widget =
            view ? view->GetRenderWidgetHost() : nullptr;
        if (!x || !y || !widget) {
          tool_result.Set("error", "Integer x and y coordinates are required.");
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
