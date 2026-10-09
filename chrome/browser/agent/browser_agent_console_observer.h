// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_CONSOLE_OBSERVER_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_CONSOLE_OBSERVER_H_

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"
#include "content/public/browser/global_request_id.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "url/gurl.h"

namespace content {
class NavigationHandle;
class RenderFrameHost;
}  // namespace content

namespace blink::mojom {
class ResourceLoadInfo;
}  // namespace blink::mojom

namespace browser_agent {

class BrowserAgentConsoleObserver : public content::WebContentsObserver {
 public:
  BrowserAgentConsoleObserver(content::WebContents* contents,
                              std::string accept_language);
  BrowserAgentConsoleObserver(const BrowserAgentConsoleObserver&) = delete;
  BrowserAgentConsoleObserver& operator=(const BrowserAgentConsoleObserver&) =
      delete;
  ~BrowserAgentConsoleObserver() override;

  void SetAcceptLanguage(std::string accept_language);
  void SetNavigationEventCallback(
      base::RepeatingCallback<void(std::string, std::string)> callback);
  base::ListValue ReadAndClear();
  base::ListValue ReadAndClearNetworkEvents();
  void AddNetworkEvent(base::DictValue event);

  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidStopLoading() override;
  void ResourceLoadComplete(
      content::RenderFrameHost* source_frame,
      const content::GlobalRequestID& request_id,
      const GURL& original_url,
      const blink::mojom::ResourceLoadInfo& info) override;
  void OnDidAddMessageToConsole(
      content::RenderFrameHost* source_frame,
      blink::mojom::ConsoleMessageLevel log_level,
      const std::u16string& message,
      int32_t line_no,
      const std::u16string& source_id,
      const std::optional<std::u16string>& untrusted_stack_trace) override;
  void WebContentsDestroyed() override;

 private:
  base::ListValue messages_;
  base::ListValue network_events_;
  std::deque<size_t> network_event_sizes_;
  std::string accept_language_;
  base::RepeatingCallback<void(std::string, std::string)>
      navigation_event_callback_;
};

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_CONSOLE_OBSERVER_H_
