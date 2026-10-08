// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AGENT_BROWSER_AGENT_SERVICE_FACTORY_H_
#define CHROME_BROWSER_AGENT_BROWSER_AGENT_SERVICE_FACTORY_H_

#include "base/no_destructor.h"
#include "chrome/browser/agent/browser_agent_service.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

namespace content {
class BrowserContext;
}

namespace browser_agent {

class BrowserAgentServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static BrowserAgentServiceFactory* GetInstance();
  static BrowserAgentService* GetForProfile(Profile* profile);

  BrowserAgentServiceFactory(const BrowserAgentServiceFactory&) = delete;
  BrowserAgentServiceFactory& operator=(const BrowserAgentServiceFactory&) =
      delete;

  bool ServiceIsCreatedWithBrowserContext() const override;
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;

 private:
  friend class base::NoDestructor<BrowserAgentServiceFactory>;
  BrowserAgentServiceFactory();
  ~BrowserAgentServiceFactory() override;
};

}  // namespace browser_agent

#endif  // CHROME_BROWSER_AGENT_BROWSER_AGENT_SERVICE_FACTORY_H_
