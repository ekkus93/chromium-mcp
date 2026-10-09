// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>

#include "chrome/browser/agent/browser_agent_mcp_server_protocol.h"

namespace browser_agent {

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

}  // namespace browser_agent
