// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_SERVICES_DEVICE_SYNC_CRYPTAUTH_FEATURE_STATUS_GETTER_IMPL_H_
#define CHROMEOS_SERVICES_DEVICE_SYNC_CRYPTAUTH_FEATURE_STATUS_GETTER_IMPL_H_

#include <memory>
#include <string>

#include "base/containers/flat_set.h"
#include "base/macros.h"
#include "base/timer/timer.h"
#include "chromeos/services/device_sync/cryptauth_device_sync_result.h"
#include "chromeos/services/device_sync/cryptauth_feature_status_getter.h"
#include "chromeos/services/device_sync/network_request_error.h"
#include "chromeos/services/device_sync/proto/cryptauth_devicesync.pb.h"

namespace chromeos {

namespace device_sync {

class CryptAuthClient;
class CryptAuthClientFactory;

// An implementation of CryptAuthFeatureStatusGetter, using instances of
// CryptAuthClient to make the BatchGetFeatureStatuses API calls to CryptAuth.
// Timeouts are handled internally, so GetFeatureStatuses() is always guaranteed
// to return.
class CryptAuthFeatureStatusGetterImpl : public CryptAuthFeatureStatusGetter {
 public:
  class Factory {
   public:
    static Factory* Get();
    static void SetFactoryForTesting(Factory* test_factory);
    virtual ~Factory();
    virtual std::unique_ptr<CryptAuthFeatureStatusGetter> BuildInstance(
        CryptAuthClientFactory* client_factory,
        std::unique_ptr<base::OneShotTimer> timer =
            std::make_unique<base::OneShotTimer>());

   private:
    static Factory* test_factory_;
  };

  ~CryptAuthFeatureStatusGetterImpl() override;

 private:
  CryptAuthFeatureStatusGetterImpl(CryptAuthClientFactory* client_factory,
                                   std::unique_ptr<base::OneShotTimer> timer);

  // CryptAuthFeatureStatusGetter:
  void OnAttemptStarted(const cryptauthv2::RequestContext& request_context,
                        const base::flat_set<std::string>& device_ids) override;

  void OnBatchGetFeatureStatusesSuccess(
      const base::flat_set<std::string>& input_device_ids,
      const cryptauthv2::BatchGetFeatureStatusesResponse& feature_response);
  void OnBatchGetFeatureStatusesFailure(NetworkRequestError error);
  void OnBatchGetFeatureStatusesTimeout();

  void FinishAttempt(const CryptAuthDeviceSyncResult::ResultCode& result_code);

  IdToFeatureStatusMap id_to_feature_status_map_;

  // The CryptAuthClient for the latest CryptAuth request. The client can only
  // be used for one call; therefore, for each API call, a new client needs to
  // be generated from |client_factory_|.
  std::unique_ptr<CryptAuthClient> cryptauth_client_;

  CryptAuthClientFactory* client_factory_ = nullptr;
  std::unique_ptr<base::OneShotTimer> timer_;

  DISALLOW_COPY_AND_ASSIGN(CryptAuthFeatureStatusGetterImpl);
};

}  // namespace device_sync

}  // namespace chromeos

#endif  // CHROMEOS_SERVICES_DEVICE_SYNC_CRYPTAUTH_FEATURE_STATUS_GETTER_IMPL_H_
