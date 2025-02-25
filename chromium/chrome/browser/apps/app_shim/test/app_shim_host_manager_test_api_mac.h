// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_APPS_APP_SHIM_TEST_APP_SHIM_HOST_MANAGER_TEST_API_MAC_H_
#define CHROME_BROWSER_APPS_APP_SHIM_TEST_APP_SHIM_HOST_MANAGER_TEST_API_MAC_H_

#include <memory>

#include "base/macros.h"

class AppShimHostManager;

namespace base {
class FilePath;
}

namespace apps {
class ExtensionAppShimHandler;
class MachBootstrapAcceptor;
}

namespace test {

class AppShimHostManagerTestApi {
 public:
  explicit AppShimHostManagerTestApi(AppShimHostManager* host_manager);

  apps::MachBootstrapAcceptor* mach_acceptor();

  const base::FilePath& directory_in_tmp();

  void SetExtensionAppShimHandler(
      std::unique_ptr<apps::ExtensionAppShimHandler> handler);

 private:
  AppShimHostManager* host_manager_;  // Not owned.

  DISALLOW_COPY_AND_ASSIGN(AppShimHostManagerTestApi);
};

}  // namespace test

#endif  // CHROME_BROWSER_APPS_APP_SHIM_TEST_APP_SHIM_HOST_MANAGER_TEST_API_MAC_H_
