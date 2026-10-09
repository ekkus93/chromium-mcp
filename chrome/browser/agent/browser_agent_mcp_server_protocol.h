// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_PROTOCOL_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_PROTOCOL_H_

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/download/public/common/download_item.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom.h"

namespace browser_agent {

const char* DownloadStateName(download::DownloadItem::DownloadState state);
std::optional<ContentSettingsType> PermissionType(std::string_view name);
std::optional<ContentSetting> PermissionSetting(std::string_view setting);
const char* PermissionSettingName(ContentSetting setting);
const char* PermissionStatusName(blink::mojom::PermissionStatus status);

base::DictValue MakeTool(std::string name,
                         std::string description,
                         base::DictValue input_schema);
base::DictValue EmptyObjectSchema();
base::DictValue StringPropertySchema();
base::DictValue NumberPropertySchema();
base::DictValue BooleanPropertySchema();
base::DictValue IntegerPropertySchema();
base::DictValue SetBooleanContextSchema();
base::DictValue SetLanguageSchema();
base::DictValue ObjectSchema(std::initializer_list<const char*> properties,
                             std::initializer_list<const char*> required = {});
base::DictValue PointSchema();
base::DictValue SetCookiesSchema();
base::DictValue SetStorageSchema();
base::DictValue ViewportSchema();
base::DictValue PrepareFileUploadSchema();

const char* ToolErrorCode(std::string_view message);
std::string MakeToolCallResponse(base::Value request_id,
                                 base::DictValue structured_content,
                                 std::string text,
                                 bool is_error);
std::string MakeJsonRpcError(base::Value request_id,
                             int code,
                             std::string message);
std::string MakeImageToolCallResponse(base::Value request_id,
                                      std::string image_data,
                                      int width,
                                      int height);

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_MCP_SERVER_PROTOCOL_H_
