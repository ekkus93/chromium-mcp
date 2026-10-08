// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_protocol.h"

#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "crypto/secure_util.h"

namespace browser_agent::protocol {
namespace {

bool IsValidJsonRpcId(const base::Value* id) {
  return id && (id->is_int() || id->is_double() || id->is_string());
}

bool Fail(Error* error, int code, const char* message) {
  if (error) {
    error->code = code;
    error->message = message;
  }
  return false;
}

}  // namespace

bool ParseToolCall(const base::DictValue& value,
                   Request* request,
                   Error* error) {
  CHECK(request);
  const std::string* version = value.FindString("jsonrpc");
  if (!version || *version != "2.0" || !IsValidJsonRpcId(value.Find("id"))) {
    return Fail(error, -32600, "Invalid JSON-RPC request.");
  }
  const std::string* method = value.FindString("method");
  if (!method || *method != "tools/call") {
    return Fail(error, -32601, "Method not found.");
  }
  const base::DictValue* params = value.FindDict("params");
  const std::string* tool = params ? params->FindString("name") : nullptr;
  if (!tool || tool->empty()) {
    return Fail(error, -32602, "The tools/call name is missing.");
  }

  Request candidate;
  candidate.request_id = value.Find("id")->Clone();
  candidate.tool = *tool;
  if (const base::DictValue* arguments = params->FindDict("arguments")) {
    candidate.arguments = arguments->Clone();
  } else if (params->Find("arguments")) {
    return Fail(error, -32602, "Tool arguments must be an object.");
  }
  *request = std::move(candidate);
  return true;
}

bool IsValidAccessToken(std::string_view expected, std::string_view supplied) {
  if (expected.size() != supplied.size()) {
    return false;
  }
  return crypto::SecureMemEqual(base::as_byte_span(expected),
                                base::as_byte_span(supplied));
}

CancellationToken::CancellationToken(
    std::shared_ptr<std::atomic_bool> cancelled)
    : cancelled_(std::move(cancelled)) {}

CancellationToken::CancellationToken(const CancellationToken&) = default;
CancellationToken& CancellationToken::operator=(const CancellationToken&) =
    default;
CancellationToken::~CancellationToken() = default;

bool CancellationToken::IsCancelled() const {
  return cancelled_->load(std::memory_order_acquire);
}

CancellationSource::CancellationSource()
    : cancelled_(std::make_shared<std::atomic_bool>(false)) {}

CancellationSource::~CancellationSource() = default;

void CancellationSource::Cancel() {
  cancelled_->store(true, std::memory_order_release);
}

}  // namespace browser_agent::protocol
