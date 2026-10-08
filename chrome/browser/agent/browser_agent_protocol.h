// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_PROTOCOL_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_PROTOCOL_H_

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "base/values.h"

namespace browser_agent::protocol {

inline constexpr char kMcpProtocolVersion[] = "2026-07-28";

struct Error {
  int code = -32602;
  std::string message;
};

struct Request {
  base::Value request_id;
  std::string tool;
  base::DictValue arguments;
};

bool ParseToolCall(const base::DictValue& value,
                   Request* request,
                   Error* error);
bool IsValidAccessToken(std::string_view expected, std::string_view supplied);

// Shared, thread-safe cancellation state for an in-flight protocol action.
class CancellationToken {
 public:
  CancellationToken(const CancellationToken&);
  CancellationToken& operator=(const CancellationToken&);
  ~CancellationToken();

  bool IsCancelled() const;

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> cancelled);

  std::shared_ptr<std::atomic_bool> cancelled_;
};

class CancellationSource {
 public:
  CancellationSource();
  CancellationSource(const CancellationSource&) = delete;
  CancellationSource& operator=(const CancellationSource&) = delete;
  ~CancellationSource();

  CancellationToken token() const { return CancellationToken(cancelled_); }
  void Cancel();

 private:
  std::shared_ptr<std::atomic_bool> cancelled_;
};

}  // namespace browser_agent::protocol

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_PROTOCOL_H_
