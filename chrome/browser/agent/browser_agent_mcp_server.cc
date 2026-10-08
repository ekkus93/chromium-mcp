// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_mcp_server.h"

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
const net::NetworkTrafficAnnotationTag& GetTrafficAnnotation();
const net::NetworkTrafficAnnotationTag& GetDownloadTrafficAnnotation();
}  // namespace

class BrowserAgentMcpServerState
    : public base::RefCountedThreadSafe<BrowserAgentMcpServerState> {
 public:
  void SetServer(net::HttpServer* server) {
    server_ = server;
    if (!server_) {
      event_streams_.clear();
    }
  }

  void Send(int connection_id, net::HttpStatusCode status, std::string body) {
    if (server_) {
      server_->Send(connection_id, status, body, "application/json",
                    GetTrafficAnnotation());
    }
  }

  void OpenEventStream(int connection_id) {
    if (!server_) {
      return;
    }
    event_streams_.insert(connection_id);
    SendEvent(
        connection_id,
        R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","logger":"chromium","data":{"event_type":"event_stream_opened"}}})");
  }

  bool CloseEventStream(int connection_id) {
    return event_streams_.erase(connection_id) != 0;
  }

  void SendNotification(const std::string& notification) {
    for (int connection_id : event_streams_) {
      SendEvent(connection_id, notification);
    }
  }

 private:
  void SendEvent(int connection_id, std::string_view notification) {
    const std::string payload =
        base::StrCat({"event: message\ndata: ", notification, "\n\n"});
    const std::string chunk =
        base::StringPrintf("%zx\r\n%s\r\n", payload.size(), payload.c_str());
    server_->SendRaw(connection_id, chunk, GetTrafficAnnotation());
  }

  friend class base::RefCountedThreadSafe<BrowserAgentMcpServerState>;
  ~BrowserAgentMcpServerState() = default;

  raw_ptr<net::HttpServer> server_ = nullptr;
  std::set<int> event_streams_;
};

namespace {

constexpr char kProtocolVersionHeader[] = "mcp-protocol-version";
constexpr char kMethodHeader[] = "mcp-method";
constexpr char kNameHeader[] = "mcp-name";
constexpr char kAuthorizationHeader[] = "authorization";
constexpr char kOriginHeader[] = "origin";
constexpr char kHostHeader[] = "host";
constexpr char kEndpointPath[] = "/mcp";
constexpr char kServerName[] = "Chromium Browser";
constexpr char kServerVersion[] = "0.1.0";
constexpr size_t kMaximumBodySize = 1024 * 1024;

const net::NetworkTrafficAnnotationTag& GetTrafficAnnotation() {
  static const net::NetworkTrafficAnnotationTag annotation =
      net::DefineNetworkTrafficAnnotation("chromium_embedded_mcp", R"(
        semantics {
          sender: "Chromium Embedded MCP Server"
          description: "Serves local MCP requests to the Chromium browser."
          trigger: "A user-enabled local MCP client sends a request."
          data: "Browser state and tool arguments selected by the local client."
          destination: OTHER
        }
        policy {
          cookies_allowed: NO
          setting: "The service requires an explicit feature and command-line opt-in."
          policy_exception_justification: "This is a loopback-only developer feature."
        })");
  return annotation;
}

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
                             std::initializer_list<const char*> required = {}) {
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

class BrowserAgentConsoleObserver : public content::WebContentsObserver {
 public:
  BrowserAgentConsoleObserver(content::WebContents* contents,
                              std::string accept_language)
      : content::WebContentsObserver(contents),
        accept_language_(std::move(accept_language)) {}

  void SetAcceptLanguage(std::string accept_language) {
    accept_language_ = std::move(accept_language);
  }

  void SetNavigationEventCallback(
      base::RepeatingCallback<void(std::string, std::string)> callback) {
    navigation_event_callback_ = std::move(callback);
  }

  base::ListValue ReadAndClear() {
    return std::exchange(messages_, base::ListValue());
  }

  base::ListValue ReadAndClearNetworkEvents() {
    return std::exchange(network_events_, base::ListValue());
  }

  void AddNetworkEvent(base::DictValue event) {
    network_events_.Append(event.Clone());
  }

  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override {
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

  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override {
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

  void DidStopLoading() override {
    if (navigation_event_callback_ && web_contents()) {
      navigation_event_callback_.Run(
          "navigation_finished", web_contents()->GetLastCommittedURL().spec());
    }
  }

  void ResourceLoadComplete(
      content::RenderFrameHost* source_frame,
      const content::GlobalRequestID& request_id,
      const GURL& original_url,
      const blink::mojom::ResourceLoadInfo& info) override {
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

  void OnDidAddMessageToConsole(
      content::RenderFrameHost* source_frame,
      blink::mojom::ConsoleMessageLevel log_level,
      const std::u16string& message,
      int32_t line_no,
      const std::u16string& source_id,
      const std::optional<std::u16string>& untrusted_stack_trace) override {
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

  void WebContentsDestroyed() override { Observe(nullptr); }

 private:
  base::ListValue messages_;
  base::ListValue network_events_;
  std::string accept_language_;
  base::RepeatingCallback<void(std::string, std::string)>
      navigation_event_callback_;
};

BrowserAgentMcpServer::BrowserAgentMcpServer(Profile* profile,
                                             std::string access_token,
                                             base::FilePath audit_log_path,
                                             base::FilePath trace_directory)
    : profile_(profile),
      default_context_id_(base::UnguessableToken::Create().ToString()),
      access_token_(std::move(access_token)),
      audit_log_path_(std::move(audit_log_path)),
      trace_directory_(std::move(trace_directory)),
      ui_task_runner_(content::GetUIThreadTaskRunner({})),
      server_state_(base::MakeRefCounted<BrowserAgentMcpServerState>()) {
  weak_this_ = weak_factory_.GetWeakPtr();
  if (!audit_log_path_.empty()) {
    audit_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT});
  }
  context_handles_.emplace(default_context_id_, profile);
}

BrowserAgentMcpServer::~BrowserAgentMcpServer() {
  if (io_thread_.IsRunning()) {
    io_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&BrowserAgentMcpServer::StopOnIoThread,
                                  base::Unretained(this)));
    io_thread_.Stop();
  }
  if (!access_token_path_.empty()) {
    base::DeleteFile(access_token_path_);
  }
  if (!endpoint_path_.empty()) {
    base::DeleteFile(endpoint_path_);
  }
  for (Profile* context_profile : observed_context_profiles_) {
    context_profile->RemoveObserver(this);
  }
}

void BrowserAgentMcpServer::OnProfileWillBeDestroyed(Profile* profile) {
  profile->RemoveObserver(this);
  std::erase_if(context_handles_, [profile](const auto& entry) {
    return entry.second == profile;
  });
  observed_context_profiles_.erase(profile);
}

bool BrowserAgentMcpServer::Start(int port) {
  const base::FilePath runtime_directory =
      profile_->GetPath().AppendASCII("BrowserAgentRuntime");
  if (!base::CreateDirectory(runtime_directory)) {
    return false;
  }
  if (!trace_directory_.empty() && !base::CreateDirectory(trace_directory_)) {
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(runtime_directory, 0700)) {
    return false;
  }
#endif
  access_token_path_ = runtime_directory.AppendASCII("browser-agent-mcp-token");
  if (!base::WriteFile(access_token_path_, access_token_)) {
    access_token_path_.clear();
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(access_token_path_, 0600)) {
    base::DeleteFile(access_token_path_);
    access_token_path_.clear();
    return false;
  }
#endif

  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  if (!io_thread_.StartWithOptions(std::move(options))) {
    return false;
  }
  io_task_runner_ = io_thread_.task_runner();
  base::WaitableEvent started;
  io_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&BrowserAgentMcpServer::StartOnIoThread,
                                base::Unretained(this), port, &started));
  started.Wait();
  if (!start_succeeded_) {
    return false;
  }
  endpoint_path_ = runtime_directory.AppendASCII("browser-agent-mcp-endpoint");
  const std::string endpoint = base::StrCat(
      {"http://127.0.0.1:", base::NumberToString(port_), kEndpointPath, "\n"});
  if (!base::WriteFile(endpoint_path_, endpoint)) {
    endpoint_path_.clear();
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(endpoint_path_, 0600)) {
    base::DeleteFile(endpoint_path_);
    endpoint_path_.clear();
    return false;
  }
#endif
  LOG(INFO) << "event=browser_agent_mcp_started endpoint=http://127.0.0.1:"
            << port_ << kEndpointPath
            << " token_file=" << access_token_path_.AsUTF8Unsafe();
  return true;
}

void BrowserAgentMcpServer::StartOnIoThread(int port,
                                            base::WaitableEvent* started) {
  auto server_socket =
      std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  net::IPEndPoint address(net::IPAddress::IPv4Localhost(),
                          static_cast<uint16_t>(port));
  if (server_socket->Listen(address, 8, std::nullopt) == net::OK) {
    net::IPEndPoint bound_address;
    if (server_socket->GetLocalAddress(&bound_address) == net::OK) {
      port_ = bound_address.port();
      http_server_ =
          std::make_unique<net::HttpServer>(std::move(server_socket), this);
      server_state_->SetServer(http_server_.get());
      start_succeeded_ = true;
    }
  }
  started->Signal();
}

void BrowserAgentMcpServer::StopOnIoThread() {
  server_state_->SetServer(nullptr);
  http_server_.reset();
}

void BrowserAgentMcpServer::OnConnect(int connection_id) {}

void BrowserAgentMcpServer::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  if (info.path != kEndpointPath || !info.peer.address().IsLoopback() ||
      info.GetHeaderValue(kOriginHeader).size() ||
      info.GetHeaderValue(kHostHeader) !=
          base::StrCat({"127.0.0.1:", base::NumberToString(port_)})) {
    SendJson(connection_id, net::HTTP_FORBIDDEN,
             R"({"error":"local MCP endpoint rejected this request"})");
    return;
  }
  const std::string authorization = info.GetHeaderValue(kAuthorizationHeader);
  constexpr std::string_view kBearerPrefix = "Bearer ";
  if (!authorization.starts_with(kBearerPrefix) ||
      !protocol::IsValidAccessToken(
          access_token_,
          std::string_view(authorization).substr(kBearerPrefix.size()))) {
    SendJson(connection_id, net::HTTP_UNAUTHORIZED,
             R"({"error":"MCP authentication required"})");
    return;
  }
  if (info.method == "GET") {
    const std::string accept =
        base::ToLowerASCII(info.GetHeaderValue("accept"));
    if (accept.find("text/event-stream") == std::string::npos) {
      SendJson(
          connection_id, net::HTTP_NOT_ACCEPTABLE,
          R"({"error":"event stream requires Accept: text/event-stream"})");
      return;
    }
    net::HttpServerResponseInfo response(net::HTTP_OK);
    response.AddHeader("Content-Type", "text/event-stream");
    response.AddHeader("Cache-Control", "no-cache");
    response.AddHeader("Connection", "keep-alive");
    response.AddHeader("Transfer-Encoding", "chunked");
    http_server_->SendResponse(connection_id, response, GetTrafficAnnotation());
    server_state_->OpenEventStream(connection_id);
    ui_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&BrowserAgentMcpServer::StartEventStreamOnUiThread,
                       weak_this_, connection_id));
    return;
  }
  if (info.method != "POST") {
    SendJson(
        connection_id, net::HTTP_METHOD_NOT_ALLOWED,
        R"({"error":"MCP requests must use POST or an event-stream GET"})");
    return;
  }

  if (info.data.size() > kMaximumBodySize) {
    SendJson(connection_id, net::HTTP_REQUEST_ENTITY_TOO_LARGE,
             R"({"error":"request body too large"})");
    return;
  }

  const std::string protocol_version =
      info.GetHeaderValue(kProtocolVersionHeader);
  std::optional<base::Value> value = base::JSONReader::Read(info.data, 0);
  if (!value) {
    SendJson(connection_id, net::HTTP_BAD_REQUEST,
             MakeJsonRpcError(base::Value(), -32700, "Parse error."));
    return;
  }
  if (!value->is_dict()) {
    SendJson(connection_id, net::HTTP_BAD_REQUEST,
             MakeJsonRpcError(base::Value(), -32600, "Invalid request."));
    return;
  }
  const base::DictValue& request = value->GetDict();
  const std::string* method = request.FindString("method");
  if (!method ||
      info.GetHeaderValue(kProtocolVersionHeader) !=
          protocol::kMcpProtocolVersion ||
      info.GetHeaderValue(kMethodHeader) != *method) {
    SendJson(connection_id, net::HTTP_BAD_REQUEST,
             R"({"error":"MCP protocol headers do not match the request"})");
    return;
  }
  const base::DictValue* params = request.FindDict("params");
  const base::DictValue* metadata =
      params ? params->FindDict("_meta") : nullptr;
  if (metadata) {
    const std::string* request_protocol_version =
        metadata->FindString("io.modelcontextprotocol/protocolVersion");
    if (request_protocol_version &&
        *request_protocol_version != protocol_version) {
      SendJson(
          connection_id, net::HTTP_BAD_REQUEST,
          R"({"error":"MCP protocol version header does not match metadata"})");
      return;
    }
  }
  if (*method == "tools/call") {
    const base::DictValue* call_params = request.FindDict("params");
    const std::string* tool_name =
        call_params ? call_params->FindString("name") : nullptr;
    if (!tool_name || info.GetHeaderValue(kNameHeader) != *tool_name) {
      SendJson(connection_id, net::HTTP_BAD_REQUEST,
               R"({"error":"Mcp-Name does not match the tool call"})");
      return;
    }
  }
  if (*method == "resources/read") {
    const base::DictValue* read_params = request.FindDict("params");
    const std::string* resource_uri =
        read_params ? read_params->FindString("uri") : nullptr;
    if (!resource_uri || info.GetHeaderValue(kNameHeader) != *resource_uri) {
      SendJson(connection_id, net::HTTP_BAD_REQUEST,
               R"({"error":"Mcp-Name does not match the resource URI"})");
      return;
    }
  }
  if (audit_task_runner_) {
    base::DictValue event;
    event.Set(
        "time_unix_ms",
        static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
    event.Set("event", "mcp_request");
    event.Set("method", *method);
    if (*method == "tools/call") {
      event.Set("tool", *request.FindDict("params")->FindString("name"));
    }
    std::string line =
        base::WriteJson(base::Value(std::move(event))).value_or("{}");
    line.push_back('\n');
    audit_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::FilePath path, std::string line) {
                         base::File file(path, base::File::FLAG_OPEN_ALWAYS |
                                                   base::File::FLAG_APPEND);
                         if (file.IsValid()) {
                           file.WriteAtCurrentPos(base::as_byte_span(line));
                         }
                       },
                       audit_log_path_, std::move(line)));
  }
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &BrowserAgentMcpServer::HandleRequestOnUiThread, weak_this_,
          request.Clone(), connection_id,
          base::BindPostTask(
              io_task_runner_,
              base::BindOnce(&BrowserAgentMcpServerState::Send, server_state_,
                             connection_id, net::HTTP_OK))));
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

void BrowserAgentMcpServer::PublishBrowserEvent(base::DictValue event) {
  base::DictValue params;
  params.Set("level", "info");
  params.Set("logger", "chromium.browser");
  params.Set("data", std::move(event));
  base::DictValue notification;
  notification.Set("jsonrpc", "2.0");
  notification.Set("method", "notifications/message");
  notification.Set("params", std::move(params));
  std::string serialized =
      base::WriteJson(base::Value(std::move(notification))).value_or("{}");
  io_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<BrowserAgentMcpServerState> server_state,
             std::string message) { server_state->SendNotification(message); },
          server_state_, std::move(serialized)));
}

void BrowserAgentMcpServer::PublishNavigationEvent(std::string page_id,
                                                   std::string event_type,
                                                   std::string url) {
  base::DictValue event;
  event.Set("event_type", std::move(event_type));
  event.Set("page_id", std::move(page_id));
  event.Set("url", std::move(url));
  PublishBrowserEvent(std::move(event));
}

base::RepeatingCallback<void(base::DictValue)>
BrowserAgentMcpServer::CreateNetworkEventCallback(
    content::WebContents* contents) {
  if (!contents) {
    return {};
  }
  for (const auto& [page_id, handle] : page_handles_) {
    tabs::TabInterface* tab = handle.Get();
    if (tab && tab->GetContents() == contents) {
      return base::BindPostTask(
          ui_task_runner_,
          base::BindRepeating(
              &BrowserAgentMcpServer::RecordNetworkEventOnUiThread,
              weak_factory_.GetWeakPtr(), page_id));
    }
  }
  return {};
}

void BrowserAgentMcpServer::RecordNetworkEventOnUiThread(
    std::string page_id,
    base::DictValue event) {
  auto handle = page_handles_.find(page_id);
  if (handle == page_handles_.end()) {
    return;
  }
  tabs::TabInterface* tab = handle->second.Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return;
  }
  auto& observer = console_observers_[page_id];
  if (!observer) {
    observer =
        std::make_unique<BrowserAgentConsoleObserver>(contents, std::string());
  }
  event.Set("page_id", page_id);
  observer->AddNetworkEvent(event.Clone());
  PublishBrowserEvent(std::move(event));
}

std::string BrowserAgentMcpServer::RegisterContext(Profile* profile) {
  for (const auto& [context_id, registered_profile] : context_handles_) {
    if (registered_profile == profile) {
      return context_id;
    }
  }
  if (profile != profile_ &&
      observed_context_profiles_.insert(profile).second) {
    profile->AddObserver(this);
  }
  const std::string context_id = base::UnguessableToken::Create().ToString();
  context_handles_.emplace(context_id, profile);
  return context_id;
}

Profile* BrowserAgentMcpServer::ResolveContext(const std::string& context_id) {
  auto it = context_handles_.find(context_id);
  if (it == context_handles_.end()) {
    return nullptr;
  }
  Profile* target = it->second;
  if (target == profile_) {
    return profile_;
  }
  for (Profile* live_profile : profile_->GetAllOffTheRecordProfiles()) {
    if (live_profile == target) {
      return target;
    }
  }
  context_handles_.erase(it);
  return nullptr;
}

void BrowserAgentMcpServer::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  SendJson(connection_id, net::HTTP_BAD_REQUEST,
           R"({"error":"WebSocket transport is not supported"})");
}

void BrowserAgentMcpServer::OnWebSocketMessage(int connection_id,
                                               std::string data) {
  http_server_->Close(connection_id);
}

void BrowserAgentMcpServer::OnClose(int connection_id) {
  if (server_state_->CloseEventStream(connection_id)) {
    ui_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&BrowserAgentMcpServer::StopEventStreamOnUiThread,
                       weak_this_, connection_id));
  }
  ui_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&BrowserAgentMcpServer::CancelRequestOnUiThread,
                                weak_this_, connection_id));
}

void BrowserAgentMcpServer::SendJson(int connection_id,
                                     net::HttpStatusCode status,
                                     const std::string& body) {
  server_state_->Send(connection_id, status, body);
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
