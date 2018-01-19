// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include <string>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/location.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chrome/browser/chromeos/attestation/attestation_key_payload.pb.h"
#include "chrome/browser/chromeos/attestation/attestation_policy_observer.h"
#include "chrome/browser/chromeos/attestation/fake_certificate.h"
#include "chrome/browser/chromeos/settings/scoped_cros_settings_test_helper.h"
#include "chromeos/attestation/mock_attestation_flow.h"
#include "chromeos/dbus/fake_cryptohome_client.h"
#include "chromeos/settings/cros_settings_names.h"
#include "components/policy/core/common/cloud/mock_cloud_policy_client.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::StrictMock;
using testing::WithArgs;

namespace chromeos {
namespace attestation {

namespace {

const int64_t kCertValid = 90;
const int64_t kCertExpiringSoon = 20;
const int64_t kCertExpired = -20;

void CertCallbackSuccess(const AttestationFlow::CertificateCallback& callback) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(callback, true, "fake_cert"));
}

void StatusCallbackSuccess(
    const policy::CloudPolicyClient::StatusCallback& callback) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE,
                                                base::BindOnce(callback, true));
}

}  // namespace

class AttestationPolicyObserverTest : public ::testing::Test {
 public:
  AttestationPolicyObserverTest() {
    settings_helper_.ReplaceProvider(kDeviceAttestationEnabled);
    settings_helper_.SetBoolean(kDeviceAttestationEnabled, true);
    policy_client_.SetDMToken("fake_dm_token");
  }

 protected:
  enum MockOptions {
    MOCK_KEY_EXISTS = 1,           // Configure so a certified key exists.
    MOCK_KEY_UPLOADED = (1 << 1),  // Configure so an upload has occurred.
    MOCK_NEW_KEY = (1 << 2)        // Configure expecting new key generation.
  };

  // Configures mock expectations according to |mock_options|.  If options
  // require that a certificate exists, |certificate| will be used.
  void SetupMocks(int mock_options, const std::string& certificate) {
    bool key_exists = (mock_options & MOCK_KEY_EXISTS);
    // Setup expected key / cert queries.
    if (key_exists) {
      cryptohome_client_.SetTpmAttestationDeviceCertificate(
          kEnterpriseMachineKey, certificate);
    }

    // Setup expected key payload queries.
    bool key_uploaded = (mock_options & MOCK_KEY_UPLOADED);
    cryptohome_client_.SetTpmAttestationDeviceKeyPayload(
        kEnterpriseMachineKey, key_uploaded ? CreatePayload() : std::string());

    // Setup expected key uploads.  Use WillOnce() so StrictMock will trigger an
    // error if our expectations are not met exactly.  We want to verify that
    // during a single run through the observer only one upload operation occurs
    // (because it is costly) and similarly, that the writing of the uploaded
    // status in the key payload matches the upload operation.
    bool new_key = (mock_options & MOCK_NEW_KEY);
    if (new_key || !key_uploaded) {
      EXPECT_CALL(policy_client_,
                  UploadCertificate(new_key ? "fake_cert" : certificate, _))
          .WillOnce(WithArgs<1>(Invoke(StatusCallbackSuccess)));
    }

    // Setup expected key generations.  Again use WillOnce().  Key generation is
    // another costly operation and if it gets triggered more than once during
    // a single pass this indicates a logical problem in the observer.
    if (new_key) {
      EXPECT_CALL(attestation_flow_, GetCertificate(_, _, _, _, _))
          .WillOnce(WithArgs<4>(Invoke(CertCallbackSuccess)));
    }
  }

  void Run() {
    AttestationPolicyObserver observer(&policy_client_,
                                       &cryptohome_client_,
                                       &attestation_flow_);
    observer.set_retry_delay(0);
    base::RunLoop().RunUntilIdle();
  }

  std::string CreatePayload() {
    AttestationKeyPayload proto;
    proto.set_is_certificate_uploaded(true);
    std::string serialized;
    proto.SerializeToString(&serialized);
    return serialized;
  }

  content::TestBrowserThreadBundle test_browser_thread_bundle_;
  ScopedCrosSettingsTestHelper settings_helper_;
  FakeCryptohomeClient cryptohome_client_;
  StrictMock<MockAttestationFlow> attestation_flow_;
  StrictMock<policy::MockCloudPolicyClient> policy_client_;
};

TEST_F(AttestationPolicyObserverTest, FeatureDisabled) {
  settings_helper_.SetBoolean(kDeviceAttestationEnabled, false);
  Run();
}

TEST_F(AttestationPolicyObserverTest, UnregisteredPolicyClient) {
  policy_client_.SetDMToken("");
  Run();
}

TEST_F(AttestationPolicyObserverTest, NewCertificate) {
  SetupMocks(MOCK_NEW_KEY, "");
  Run();
  EXPECT_EQ(CreatePayload(),
            cryptohome_client_.GetTpmAttestationDeviceKeyPayload(
                kEnterpriseMachineKey));
}

TEST_F(AttestationPolicyObserverTest, KeyExistsNotUploaded) {
  std::string certificate;
  ASSERT_TRUE(GetFakeCertificatePEM(base::TimeDelta::FromDays(kCertValid),
                                    &certificate));
  SetupMocks(MOCK_KEY_EXISTS, certificate);
  Run();
  EXPECT_EQ(CreatePayload(),
            cryptohome_client_.GetTpmAttestationDeviceKeyPayload(
                kEnterpriseMachineKey));
}

TEST_F(AttestationPolicyObserverTest, KeyExistsAlreadyUploaded) {
  std::string certificate;
  ASSERT_TRUE(GetFakeCertificatePEM(base::TimeDelta::FromDays(kCertValid),
                                    &certificate));
  SetupMocks(MOCK_KEY_EXISTS | MOCK_KEY_UPLOADED, certificate);
  Run();
}

TEST_F(AttestationPolicyObserverTest, KeyExistsCertExpiringSoon) {
  std::string certificate;
  ASSERT_TRUE(GetFakeCertificatePEM(
      base::TimeDelta::FromDays(kCertExpiringSoon), &certificate));
  SetupMocks(MOCK_KEY_EXISTS | MOCK_KEY_UPLOADED | MOCK_NEW_KEY, certificate);
  Run();
}

TEST_F(AttestationPolicyObserverTest, KeyExistsCertExpired) {
  std::string certificate;
  ASSERT_TRUE(GetFakeCertificatePEM(base::TimeDelta::FromDays(kCertExpired),
                                    &certificate));
  SetupMocks(MOCK_KEY_EXISTS | MOCK_KEY_UPLOADED | MOCK_NEW_KEY, certificate);
  Run();
}

TEST_F(AttestationPolicyObserverTest, IgnoreUnknownCertFormat) {
  SetupMocks(MOCK_KEY_EXISTS | MOCK_KEY_UPLOADED, "unsupported");
  Run();
}

TEST_F(AttestationPolicyObserverTest, DBusFailureRetry) {
  SetupMocks(MOCK_NEW_KEY, "");
  // Simulate a DBus failure.
  cryptohome_client_.SetServiceIsAvailable(false);

  // Emulate delayed service initialization.
  // Run() instantiates an Observer, which synchronously calls
  // TpmAttestationDoesKeyExist() and fails. During this call, we make the
  // service available in the next run, so on retry, it will successfully
  // return the result.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](FakeCryptohomeClient* cryptohome_client) {
                       cryptohome_client->SetServiceIsAvailable(true);
                     },
                     base::Unretained(&cryptohome_client_)));
  Run();
  EXPECT_EQ(CreatePayload(),
            cryptohome_client_.GetTpmAttestationDeviceKeyPayload(
                kEnterpriseMachineKey));
}

}  // namespace attestation
}  // namespace chromeos