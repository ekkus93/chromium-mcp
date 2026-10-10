// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/agent/browser_agent_network_throttle.h"

#include <string_view>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace browser_agent {
namespace {

class CountingThrottleDelegate final
    : public blink::URLLoaderThrottle::Delegate {
 public:
  void CancelWithError(int error_code,
                       std::string_view custom_reason) override {
    ++cancellations;
  }
  void Resume() override { ++resumptions; }
  void InterceptResponse(
      mojo::PendingRemote<network::mojom::URLLoader> new_loader,
      mojo::PendingReceiver<network::mojom::URLLoaderClient>
          new_client_receiver,
      mojo::PendingRemote<network::mojom::URLLoader>* original_loader,
      mojo::PendingReceiver<network::mojom::URLLoaderClient>*
          original_client_receiver,
      mojo::ScopedDataPipeConsumerHandle* body) override {
    ++interceptions;
  }

  int cancellations = 0;
  int resumptions = 0;
  int interceptions = 0;
};

TEST(BrowserAgentNetworkThrottleTest, IneligibleResponseIsNotIntercepted) {
  base::test::TaskEnvironment task_environment;
  int eligibility_checks = 0;
  BrowserAgentNetworkThrottle throttle(
      base::BindRepeating([](base::DictValue) {}),
      base::BindRepeating(
          [](int* eligibility_checks, base::OnceCallback<void(bool)> reply) {
            ++*eligibility_checks;
            std::move(reply).Run(false);
          },
          &eligibility_checks));
  CountingThrottleDelegate delegate;
  throttle.set_delegate(&delegate);

  network::ResourceRequest request;
  request.url = GURL("https://example.test/script.js");
  request.destination = network::mojom::RequestDestination::kScript;
  bool deferred = false;
  throttle.WillStartRequest(&request, &deferred);
  EXPECT_FALSE(deferred);

  auto response_head = network::mojom::URLResponseHead::New();
  throttle.WillProcessResponse(request.url, response_head.get(), &deferred);
  EXPECT_TRUE(deferred);
  task_environment.RunUntilIdle();

  EXPECT_EQ(eligibility_checks, 1);
  EXPECT_EQ(delegate.resumptions, 1);
  EXPECT_EQ(delegate.interceptions, 0);
  EXPECT_EQ(delegate.cancellations, 0);
}

}  // namespace
}  // namespace browser_agent
