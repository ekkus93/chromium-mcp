// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/agent/browser_agent_protocol.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace browser_agent::protocol {
namespace {

base::DictValue MakeToolCall() {
  base::DictValue params;
  params.Set("name", "page.evaluate");
  params.Set("arguments",
             base::DictValue().Set("expression", "document.title"));
  base::DictValue request;
  request.Set("jsonrpc", "2.0");
  request.Set("id", 7);
  request.Set("method", "tools/call");
  request.Set("params", std::move(params));
  return request;
}

TEST(BrowserAgentProtocolTest, ParsesStandardMcpToolCall) {
  Request parsed;
  Error error;

  ASSERT_TRUE(ParseToolCall(MakeToolCall(), &parsed, &error));
  EXPECT_TRUE(parsed.request_id.is_int());
  EXPECT_EQ(7, parsed.request_id.GetInt());
  EXPECT_EQ("page.evaluate", parsed.tool);
  EXPECT_EQ("document.title", *parsed.arguments.FindString("expression"));
}

TEST(BrowserAgentProtocolTest, RejectsInvalidJsonRpcAndToolArguments) {
  Request parsed;
  Error error;
  base::DictValue invalid_version = MakeToolCall();
  invalid_version.Set("jsonrpc", "1.0");
  EXPECT_FALSE(ParseToolCall(invalid_version, &parsed, &error));
  EXPECT_EQ(-32600, error.code);

  base::DictValue invalid_arguments = MakeToolCall();
  invalid_arguments.FindDict("params")->Set("arguments", "not-an-object");
  EXPECT_FALSE(ParseToolCall(invalid_arguments, &parsed, &error));
  EXPECT_EQ(-32602, error.code);
}

TEST(BrowserAgentProtocolTest, ComparesAccessTokensInConstantTime) {
  EXPECT_TRUE(IsValidAccessToken("token-value", "token-value"));
  EXPECT_FALSE(IsValidAccessToken("token-value", "token-wrong"));
  EXPECT_FALSE(IsValidAccessToken("token-value", "short"));
}

TEST(BrowserAgentProtocolTest, CancellationTokenTracksSource) {
  CancellationSource source;
  CancellationToken token = source.token();

  EXPECT_FALSE(token.IsCancelled());
  source.Cancel();
  EXPECT_TRUE(token.IsCancelled());
}

}  // namespace
}  // namespace browser_agent::protocol
