// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/agent/browser_agent_network_throttle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <array>
#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace browser_agent {

namespace {
constexpr size_t kMaximumCapturedResponseBodyBytes = 256 * 1024;
constexpr size_t kMaximumForwardingPipeCapacityBytes = 1024 * 1024;
constexpr size_t kMaximumSimultaneousBodyCaptures = 8;
std::atomic_size_t g_active_body_captures{0};
}  // namespace

class BrowserAgentResponseBodyTee {
 public:
  using CompletionCallback =
      base::OnceCallback<void(std::string, size_t, size_t, bool, bool, int)>;

  BrowserAgentResponseBodyTee(mojo::ScopedDataPipeConsumerHandle source,
                              mojo::ScopedDataPipeProducerHandle destination,
                              CompletionCallback completion_callback)
      : source_(std::move(source)),
        destination_(std::move(destination)),
        completion_callback_(std::move(completion_callback)) {
    producer_watcher_ = std::make_unique<mojo::SimpleWatcher>(
        FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL,
        base::SequencedTaskRunner::GetCurrentDefault());
    producer_watcher_->Watch(
        destination_.get(),
        MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_WATCH_CONDITION_SATISFIED,
        base::BindRepeating(&BrowserAgentResponseBodyTee::OnWriteAvailable,
                            weak_factory_.GetWeakPtr()));
    source_watcher_ = std::make_unique<mojo::SimpleWatcher>(
        FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL,
        base::SequencedTaskRunner::GetCurrentDefault());
    source_watcher_->Watch(
        source_.get(), MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_WATCH_CONDITION_SATISFIED,
        base::BindRepeating(&BrowserAgentResponseBodyTee::OnPipeSignal,
                            weak_factory_.GetWeakPtr()));
    source_watcher_->ArmOrNotify();
    Pump();
  }

  BrowserAgentResponseBodyTee(const BrowserAgentResponseBodyTee&) = delete;
  BrowserAgentResponseBodyTee& operator=(const BrowserAgentResponseBodyTee&) =
      delete;
  ~BrowserAgentResponseBodyTee() = default;

 private:
  void OnPipeSignal(MojoResult result,
                    const mojo::HandleSignalsState& state) {
    if (result != MOJO_RESULT_OK && result != MOJO_RESULT_FAILED_PRECONDITION) {
      capture_error_ = true;
      capture_error_result_ = result;
    }
    Pump();
  }

  void OnWriteAvailable(MojoResult result,
                        const mojo::HandleSignalsState& state) {
    if (result != MOJO_RESULT_OK) {
      capture_error_ = true;
      capture_error_result_ = result;
      destination_.reset();
      producer_watcher_.reset();
      pending_body_.clear();
      pending_position_ = 0;
      Pump();
      return;
    }
    Pump();
  }

  void Pump() {
    if (pumping_ || finished_) {
      return;
    }
    pumping_ = true;
    while (true) {
      if (destination_.is_valid() && pending_position_ < pending_body_.size()) {
        const auto data = base::as_byte_span(pending_body_).subspan(pending_position_);
        size_t bytes_written = 0;
        const MojoResult result = destination_->WriteData(
            data, MOJO_WRITE_DATA_FLAG_NONE, bytes_written);
        if (result == MOJO_RESULT_SHOULD_WAIT) {
          producer_watcher_->ArmOrNotify();
          pumping_ = false;
          return;
        }
        if (result != MOJO_RESULT_OK) {
          capture_error_ = true;
          capture_error_result_ = result;
          destination_.reset();
          producer_watcher_.reset();
          pending_body_.clear();
          pending_position_ = 0;
          continue;
        }
        pending_position_ += bytes_written;
        forwarded_body_bytes_ += bytes_written;
        if (pending_position_ == pending_body_.size()) {
          pending_body_.clear();
          pending_position_ = 0;
        }
      }
      std::array<uint8_t, 64 * 1024> buffer;
      size_t bytes_read = 0;
      const MojoResult result = source_->ReadData(
          MOJO_READ_DATA_FLAG_NONE, buffer, bytes_read);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        source_watcher_->ArmOrNotify();
        pumping_ = false;
        return;
      }
      if (result == MOJO_RESULT_FAILED_PRECONDITION) {
        pumping_ = false;
        Finish();
        return;
      }
      if (result != MOJO_RESULT_OK || bytes_read == 0) {
        capture_error_ = true;
        capture_error_result_ = result;
        continue;
      }
      const base::span<const uint8_t> data =
          base::span<const uint8_t>(buffer).first(bytes_read);
      total_body_bytes_ += bytes_read;
      const size_t remaining = kMaximumCapturedResponseBodyBytes -
                               captured_body_.size();
      const size_t captured = std::min(remaining, bytes_read);
      captured_body_.append(base::as_string_view(data.first(captured)));
      truncated_ |= captured != bytes_read;
      if (destination_.is_valid()) {
        pending_body_.assign(base::as_string_view(data));
      }
    }
  }

  void Finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    destination_.reset();
    source_.reset();
    producer_watcher_.reset();
    source_watcher_.reset();
    std::move(completion_callback_)
        .Run(std::move(captured_body_), total_body_bytes_,
             forwarded_body_bytes_, truncated_, capture_error_,
             capture_error_result_);
  }

  mojo::ScopedDataPipeConsumerHandle source_;
  mojo::ScopedDataPipeProducerHandle destination_;
  std::unique_ptr<mojo::SimpleWatcher> source_watcher_;
  std::unique_ptr<mojo::SimpleWatcher> producer_watcher_;
  CompletionCallback completion_callback_;
  std::string pending_body_;
  std::string captured_body_;
  size_t pending_position_ = 0;
  size_t total_body_bytes_ = 0;
  size_t forwarded_body_bytes_ = 0;
  bool capture_error_ = false;
  int capture_error_result_ = MOJO_RESULT_OK;
  bool truncated_ = false;
  bool pumping_ = false;
  bool finished_ = false;
  base::WeakPtrFactory<BrowserAgentResponseBodyTee> weak_factory_{this};
};

class BrowserAgentNetworkResponseForwarder final
    : public network::mojom::URLLoaderClient {
 public:
  BrowserAgentNetworkResponseForwarder(
      mojo::PendingReceiver<network::mojom::URLLoaderClient> source_receiver,
      mojo::PendingRemote<network::mojom::URLLoaderClient> destination_client,
      BrowserAgentNetworkThrottle::EventCallback event_callback,
      std::string request_id,
      std::string request_url)
      : event_callback_(std::move(event_callback)),
        request_id_(std::move(request_id)),
        request_url_(std::move(request_url)) {
    destination_client_.Bind(std::move(destination_client));
    source_receiver_.Bind(std::move(source_receiver));
  }

  BrowserAgentNetworkResponseForwarder(
      const BrowserAgentNetworkResponseForwarder&) = delete;
  BrowserAgentNetworkResponseForwarder& operator=(
      const BrowserAgentNetworkResponseForwarder&) = delete;
  ~BrowserAgentNetworkResponseForwarder() override = default;

  void StartBodyCapture(mojo::ScopedDataPipeConsumerHandle source_body,
                        mojo::ScopedDataPipeConsumerHandle* response_body) {
    if (!source_body.is_valid()) {
      *response_body = {};
      CompleteBodyCapture(std::string(), 0, 0, false, false,
                          MOJO_RESULT_OK);
      return;
    }

    mojo::ScopedDataPipeProducerHandle response_body_producer;
    MojoCreateDataPipeOptions pipe_options;
    pipe_options.struct_size = sizeof(pipe_options);
    pipe_options.flags = MOJO_CREATE_DATA_PIPE_FLAG_NONE;
    pipe_options.element_num_bytes = 1;
    pipe_options.capacity_num_bytes = kMaximumForwardingPipeCapacityBytes;
    if (mojo::CreateDataPipe(&pipe_options, response_body_producer,
                             *response_body) != MOJO_RESULT_OK) {
      *response_body = std::move(source_body);
      body_capture_complete_ = true;
      base::DictValue body_event;
      body_event.Set("event_type", "subresource_response_body_error");
      body_event.Set("request_id", request_id_);
      body_event.Set("request_url", request_url_);
      body_event.Set("error", "Could not create a response body pipe.");
      event_callback_.Run(std::move(body_event));
      return;
    }

    const size_t active_captures =
        g_active_body_captures.fetch_add(1, std::memory_order_relaxed);
    if (active_captures >= kMaximumSimultaneousBodyCaptures) {
      g_active_body_captures.fetch_sub(1, std::memory_order_relaxed);
      *response_body = std::move(source_body);
      body_capture_complete_ = true;
      base::DictValue body_event;
      body_event.Set("event_type", "subresource_response_body");
      body_event.Set("request_id", request_id_);
      body_event.Set("request_url", request_url_);
      body_event.Set("body_bytes", 0);
      body_event.Set("captured_bytes", 0);
      body_event.Set("total_size_known", false);
      body_event.Set("truncated", true);
      body_event.Set("capture_skipped", true);
      event_callback_.Run(std::move(body_event));
      MaybeForwardCompletion();
      return;
    }
    has_capture_slot_ = true;

    body_tee_ = std::make_unique<BrowserAgentResponseBodyTee>(
        std::move(source_body), std::move(response_body_producer),
        base::BindOnce(
            &BrowserAgentNetworkResponseForwarder::CompleteBodyCapture,
            weak_factory_.GetWeakPtr()));
  }

 private:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr early_hints) override {
    destination_client_->OnReceiveEarlyHints(std::move(early_hints));
  }

  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr response_head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {
    destination_client_->OnReceiveResponse(
        std::move(response_head), std::move(body), std::move(cached_metadata));
  }

  void OnReceiveRedirect(
      const net::RedirectInfo& redirect_info,
      network::mojom::URLResponseHeadPtr response_head) override {
    destination_client_->OnReceiveRedirect(redirect_info,
                                           std::move(response_head));
  }

  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback ack_callback) override {
    destination_client_->OnUploadProgress(current_position, total_size,
                                          std::move(ack_callback));
  }

  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {
    destination_client_->OnTransferSizeUpdated(transfer_size_diff);
  }

  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    completion_status_ = status;
    MaybeForwardCompletion();
  }

  void CompleteBodyCapture(std::string body,
                           size_t total_bytes,
                           size_t forwarded_bytes,
                           bool truncated,
                           bool capture_error,
                           int capture_error_result) {
    body_capture_complete_ = true;
    if (has_capture_slot_) {
      has_capture_slot_ = false;
      g_active_body_captures.fetch_sub(1, std::memory_order_relaxed);
    }
    base::DictValue body_event;
    body_event.Set("event_type", "subresource_response_body");
    body_event.Set("request_id", request_id_);
    body_event.Set("request_url", request_url_);
    body_event.Set("body_bytes", static_cast<double>(body.size()));
    body_event.Set("captured_bytes", static_cast<double>(body.size()));
    body_event.Set("total_size", static_cast<double>(total_bytes));
    body_event.Set("forwarded_bytes", static_cast<double>(forwarded_bytes));
    body_event.Set("total_size_known", true);
    body_event.Set("truncated", truncated || body.size() < total_bytes);
    body_event.Set("capture_error", capture_error);
    body_event.Set("capture_error_result", capture_error_result);
    body_event.Set("body_base64", base::Base64Encode(body));
    event_callback_.Run(std::move(body_event));
    MaybeForwardCompletion();
  }

  void MaybeForwardCompletion() {
    if (!completion_status_ || !body_capture_complete_) {
      return;
    }
    network::URLLoaderCompletionStatus status = std::move(*completion_status_);
    completion_status_.reset();
    destination_client_->OnComplete(status);
  }

  const BrowserAgentNetworkThrottle::EventCallback event_callback_;
  const std::string request_id_;
  const std::string request_url_;
  mojo::Remote<network::mojom::URLLoaderClient> destination_client_;
  mojo::Receiver<network::mojom::URLLoaderClient> source_receiver_{this};
  std::unique_ptr<BrowserAgentResponseBodyTee> body_tee_;
  std::optional<network::URLLoaderCompletionStatus> completion_status_;
  bool body_capture_complete_ = false;
  bool has_capture_slot_ = false;
  base::WeakPtrFactory<BrowserAgentNetworkResponseForwarder> weak_factory_{
      this};
};

BrowserAgentNetworkThrottle::BrowserAgentNetworkThrottle(
    EventCallback event_callback)
    : event_callback_(std::move(event_callback)) {}

BrowserAgentNetworkThrottle::~BrowserAgentNetworkThrottle() = default;

void BrowserAgentNetworkThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool*) {
  request_url_ = request->url.spec();
  if (request->destination == network::mojom::RequestDestination::kDocument ||
      !event_callback_) {
    return;
  }
  capture_response_ = true;
  request_id_ = base::UnguessableToken::Create().ToString();

  base::DictValue event;
  event.Set("event_type", "subresource_request");
  event.Set("request_id", request_id_);
  event.Set("url", request_url_);
  event.Set("method", request->method);
  event.Set("request_destination", static_cast<int>(request->destination));
  event.Set("referrer", request->referrer.spec());
  base::ListValue headers;
  for (const auto& [name, value] : request->headers.GetHeaderVector()) {
    base::DictValue header;
    header.Set("name", name);
    header.Set("value", value);
    headers.Append(std::move(header));
  }
  event.Set("request_headers", std::move(headers));
  base::ListValue cors_exempt_headers;
  for (const auto& [name, value] :
       request->cors_exempt_headers.GetHeaderVector()) {
    base::DictValue header;
    header.Set("name", name);
    header.Set("value", value);
    cors_exempt_headers.Append(std::move(header));
  }
  event.Set("cors_exempt_request_headers", std::move(cors_exempt_headers));
  event_callback_.Run(std::move(event));
}

void BrowserAgentNetworkThrottle::WillRedirectRequest(
    net::RedirectInfo*,
    const network::mojom::URLResponseHead&,
    bool*,
    network::HttpRequestHeadersUpdateParams*) {
  has_redirected_ = true;
}

void BrowserAgentNetworkThrottle::WillProcessResponse(
    const GURL& response_url,
    network::mojom::URLResponseHead* response_head,
    bool* defer) {
  if (!capture_response_ || request_url_.empty() || !event_callback_) {
    return;
  }

  base::DictValue event;
  event.Set("event_type", "subresource_response");
  event.Set("request_id", request_id_);
  event.Set("request_url", request_url_);
  event.Set("url", response_url.spec());
  if (response_head->headers) {
    event.Set("http_status", response_head->headers->response_code());
    base::ListValue headers;
    size_t iterator = 0;
    std::string name;
    std::string value;
    while (response_head->headers->EnumerateHeaderLines(&iterator, &name,
                                                        &value)) {
      base::DictValue header;
      header.Set("name", name);
      header.Set("value", value);
      headers.Append(std::move(header));
    }
    event.Set("response_headers", std::move(headers));
  }
  event_callback_.Run(std::move(event));

  if (has_redirected_) {
    base::DictValue body_event;
    body_event.Set("event_type", "subresource_response_body");
    body_event.Set("request_id", request_id_);
    body_event.Set("request_url", request_url_);
    body_event.Set("body_bytes", 0);
    body_event.Set("captured_bytes", 0);
    body_event.Set("total_size_known", false);
    body_event.Set("truncated", true);
    body_event.Set("capture_skipped", true);
    body_event.Set("capture_skip_reason", "redirected_response");
    event_callback_.Run(std::move(body_event));
    capture_response_ = false;
    return;
  }

  if (!delegate_) {
    return;
  }

  *defer = true;
  capture_response_ = false;
  auto response_head_copy = response_head->Clone();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&BrowserAgentNetworkThrottle::OnResponseDeferred,
                     weak_factory_.GetWeakPtr(),
                     std::move(response_head_copy)));
}

void BrowserAgentNetworkThrottle::OnResponseDeferred(
    network::mojom::URLResponseHeadPtr response_head_copy) {
  if (!delegate_) {
    return;
  }

  mojo::PendingRemote<network::mojom::URLLoader> replacement_loader;
  mojo::PendingReceiver<network::mojom::URLLoader> replacement_loader_receiver =
      replacement_loader.InitWithNewPipeAndPassReceiver();
  mojo::PendingRemote<network::mojom::URLLoaderClient> replacement_client;
  mojo::PendingReceiver<network::mojom::URLLoaderClient>
      replacement_client_receiver =
          replacement_client.InitWithNewPipeAndPassReceiver();
  mojo::PendingRemote<network::mojom::URLLoader> original_loader;
  mojo::PendingReceiver<network::mojom::URLLoaderClient>
      original_client_receiver;
  mojo::ScopedDataPipeConsumerHandle source_body;
  delegate_->InterceptResponse(
      std::move(replacement_loader), std::move(replacement_client_receiver),
      &original_loader, &original_client_receiver, &source_body);
  if (!mojo::FusePipes(std::move(replacement_loader_receiver),
                       std::move(original_loader))) {
    delegate_->CancelWithError(net::ERR_FAILED);
    return;
  }

  mojo::ScopedDataPipeConsumerHandle response_body;
  response_forwarder_ = std::make_unique<BrowserAgentNetworkResponseForwarder>(
      std::move(original_client_receiver), std::move(replacement_client),
      event_callback_, request_id_, request_url_);
  response_forwarder_->StartBodyCapture(std::move(source_body), &response_body);

  delegate_->UpdateDeferredResponseHead(std::move(response_head_copy),
                                        std::move(response_body));
  delegate_->Resume();
}

}  // namespace browser_agent
