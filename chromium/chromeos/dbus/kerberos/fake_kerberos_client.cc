// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/dbus/kerberos/fake_kerberos_client.h"

#include <utility>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/strings/string_split.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "third_party/cros_system_api/dbus/kerberos/dbus-constants.h"

namespace chromeos {
namespace {

// Fake delay for any asynchronous operation.
constexpr auto kTaskDelay = base::TimeDelta::FromMilliseconds(100);

// Fake validity lifetime for TGTs.
constexpr base::TimeDelta kTgtValidity = base::TimeDelta::FromHours(10);

// Fake renewal lifetime for TGTs.
constexpr base::TimeDelta kTgtRenewal = base::TimeDelta::FromHours(24);

// Blacklist for fake config validation.
const char* const kBlacklistedConfigOptions[] = {
    "allow_weak_crypto",
    "ap_req_checksum_type",
    "ccache_type",
    "default_ccache_name ",
    "default_client_keytab_name",
    "default_keytab_name",
    "default_realm",
    "k5login_authoritative",
    "k5login_directory",
    "kdc_req_checksum_type",
    "plugin_base_dir",
    "realm_try_domains",
    "safe_checksum_type",
    "verify_ap_req_nofail",
    "default_domain",
    "v4_instance_convert",
    "v4_realm",
    "[appdefaults]",
    "[plugins]",
};

// Performs a fake validation of a config line by just checking for some
// non-whitelisted keywords. Returns true if no blacklisted items are contained.
bool ValidateConfigLine(const std::string& line) {
  for (const char* option : kBlacklistedConfigOptions) {
    if (line.find(option) != std::string::npos)
      return false;
  }
  return true;
}

// Posts |callback| on the current thread's task runner, passing it the
// |response| message.
template <class TProto>
void PostProtoResponse(base::OnceCallback<void(const TProto&)> callback,
                       const TProto& response) {
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, base::BindOnce(std::move(callback), response), kTaskDelay);
}

// Similar to PostProtoResponse(), but posts |callback| with a proto containing
// only the given |error|.
template <class TProto>
void PostResponse(base::OnceCallback<void(const TProto&)> callback,
                  kerberos::ErrorType error) {
  TProto response;
  response.set_error(error);
  PostProtoResponse(std::move(callback), response);
}

// Reads the password from the file descriptor |password_fd|.
// Not very efficient, but simple!
std::string ReadPassword(int password_fd) {
  std::string password;
  char c;
  while (base::ReadFromFD(password_fd, &c, 1))
    password.push_back(c);
  return password;
}

}  // namespace

FakeKerberosClient::FakeKerberosClient() = default;

FakeKerberosClient::~FakeKerberosClient() = default;

void FakeKerberosClient::AddAccount(const kerberos::AddAccountRequest& request,
                                    AddAccountCallback callback) {
  auto it = std::find(accounts_.begin(), accounts_.end(),
                      AccountData(request.principal_name()));
  if (it != accounts_.end()) {
    it->is_managed |= request.is_managed();
    PostResponse(std::move(callback), kerberos::ERROR_DUPLICATE_PRINCIPAL_NAME);
    return;
  }

  AccountData data(request.principal_name());
  data.is_managed = request.is_managed();
  accounts_.push_back(data);
  PostResponse(std::move(callback), kerberos::ERROR_NONE);
}

void FakeKerberosClient::RemoveAccount(
    const kerberos::RemoveAccountRequest& request,
    RemoveAccountCallback callback) {
  auto it = std::find(accounts_.begin(), accounts_.end(),
                      AccountData(request.principal_name()));
  if (it == accounts_.end()) {
    PostResponse(std::move(callback), kerberos::ERROR_UNKNOWN_PRINCIPAL_NAME);
    return;
  }
  accounts_.erase(it);
  PostResponse(std::move(callback), kerberos::ERROR_NONE);
}

void FakeKerberosClient::ClearAccounts(
    const kerberos::ClearAccountsRequest& request,
    ClearAccountsCallback callback) {
  std::unordered_set<std::string> keep_list(
      request.principal_names_to_ignore_size());
  for (int n = 0; n < request.principal_names_to_ignore_size(); ++n)
    keep_list.insert(request.principal_names_to_ignore(n));

  for (auto it = accounts_.begin(); it != accounts_.end(); /* empty */) {
    if (base::Contains(keep_list, it->principal_name)) {
      ++it;
      continue;
    }

    switch (DetermineWhatToRemove(request.mode(), *it)) {
      case WhatToRemove::kNothing:
        ++it;
        continue;

      case WhatToRemove::kPassword:
        it->password.clear();
        ++it;
        continue;

      case WhatToRemove::kAccount:
        it = accounts_.erase(it);
        continue;
    }
  }

  PostResponse(std::move(callback), kerberos::ERROR_NONE);
}

void FakeKerberosClient::ListAccounts(
    const kerberos::ListAccountsRequest& request,
    ListAccountsCallback callback) {
  kerberos::ListAccountsResponse response;
  for (const AccountData& data : accounts_) {
    kerberos::Account* account = response.add_accounts();
    account->set_principal_name(data.principal_name);
    account->set_krb5conf(data.krb5conf);
    account->set_tgt_validity_seconds(data.has_tgt ? kTgtValidity.InSeconds()
                                                   : 0);
    account->set_tgt_renewal_seconds(data.has_tgt ? kTgtRenewal.InSeconds()
                                                  : 0);
    account->set_is_managed(data.is_managed);
    account->set_password_was_remembered(!data.password.empty());
    account->set_use_login_password(data.use_login_password);
  }
  response.set_error(kerberos::ERROR_NONE);
  PostProtoResponse(std::move(callback), response);
}

void FakeKerberosClient::SetConfig(const kerberos::SetConfigRequest& request,
                                   SetConfigCallback callback) {
  AccountData* data = GetAccountData(request.principal_name());
  if (!data) {
    PostResponse(std::move(callback), kerberos::ERROR_UNKNOWN_PRINCIPAL_NAME);
    return;
  }

  data->krb5conf = request.krb5conf();
  PostResponse(std::move(callback), kerberos::ERROR_NONE);
}

void FakeKerberosClient::ValidateConfig(
    const kerberos::ValidateConfigRequest& request,
    ValidateConfigCallback callback) {
  kerberos::ConfigErrorInfo error_info;
  error_info.set_code(kerberos::CONFIG_ERROR_NONE);

  std::vector<std::string> lines = base::SplitString(
      request.krb5conf(), "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
    if (!ValidateConfigLine(lines[line_index])) {
      error_info.set_code(kerberos::CONFIG_ERROR_KEY_NOT_SUPPORTED);
      error_info.set_line_index(static_cast<int>(line_index));
      break;
    }
  }

  kerberos::ValidateConfigResponse response;
  response.set_error(error_info.code() != kerberos::CONFIG_ERROR_NONE
                         ? kerberos::ERROR_BAD_CONFIG
                         : kerberos::ERROR_NONE);
  *response.mutable_error_info() = std::move(error_info);
  PostProtoResponse(std::move(callback), response);
}

void FakeKerberosClient::AcquireKerberosTgt(
    const kerberos::AcquireKerberosTgtRequest& request,
    int password_fd,
    AcquireKerberosTgtCallback callback) {
  AccountData* data = GetAccountData(request.principal_name());
  if (!data) {
    PostResponse(std::move(callback), kerberos::ERROR_UNKNOWN_PRINCIPAL_NAME);
    return;
  }

  // Remember whether to use the login password.
  data->use_login_password = request.use_login_password();

  std::string password;
  if (request.use_login_password()) {
    // "Retrieve" login password.
    password = "fake_login_password";

    // Erase a previously remembered password.
    data->password.clear();
  } else {
    // Remember password.
    password = ReadPassword(password_fd);
    if (!password.empty() && request.remember_password())
      data->password = password;

    // Use remembered password.
    if (password.empty())
      password = data->password;

    // Erase a previously remembered password.
    if (!request.remember_password())
      data->password.clear();
  }

  // Reject empty passwords.
  if (password.empty()) {
    PostResponse(std::move(callback), kerberos::ERROR_BAD_PASSWORD);
    return;
  }

  // It worked! Magic!
  data->has_tgt = true;
  PostResponse(std::move(callback), kerberos::ERROR_NONE);
}

void FakeKerberosClient::GetKerberosFiles(
    const kerberos::GetKerberosFilesRequest& request,
    GetKerberosFilesCallback callback) {
  AccountData* data = GetAccountData(request.principal_name());
  if (!data) {
    PostResponse(std::move(callback), kerberos::ERROR_UNKNOWN_PRINCIPAL_NAME);
    return;
  }

  kerberos::GetKerberosFilesResponse response;
  if (data->has_tgt) {
    response.mutable_files()->set_krb5cc("Fake Kerberos credential cache");
    response.mutable_files()->set_krb5conf("Fake Kerberos configuration");
  }
  response.set_error(kerberos::ERROR_NONE);
  PostProtoResponse(std::move(callback), response);
}

void FakeKerberosClient::ConnectToKerberosFileChangedSignal(
    KerberosFilesChangedCallback callback) {
  DCHECK(!kerberos_files_changed_callback_);
  kerberos_files_changed_callback_ = callback;
}

void FakeKerberosClient::ConnectToKerberosTicketExpiringSignal(
    KerberosTicketExpiringCallback callback) {
  DCHECK(!kerberos_ticket_expiring_callback_);
  kerberos_ticket_expiring_callback_ = callback;
}

FakeKerberosClient::AccountData* FakeKerberosClient::GetAccountData(
    const std::string& principal_name) {
  auto it = std::find(accounts_.begin(), accounts_.end(),
                      AccountData(principal_name));
  return it != accounts_.end() ? &*it : nullptr;
}

FakeKerberosClient::AccountData::AccountData(const std::string& principal_name)
    : principal_name(principal_name) {}

FakeKerberosClient::AccountData::AccountData(const AccountData& other) =
    default;

bool FakeKerberosClient::AccountData::operator==(
    const AccountData& other) const {
  return principal_name == other.principal_name;
}

bool FakeKerberosClient::AccountData::operator!=(
    const AccountData& other) const {
  return !(*this == other);
}

// static
FakeKerberosClient::WhatToRemove FakeKerberosClient::DetermineWhatToRemove(
    kerberos::ClearMode mode,
    const AccountData& data) {
  switch (mode) {
    case kerberos::CLEAR_ALL:
      return WhatToRemove::kAccount;

    case kerberos::CLEAR_ONLY_MANAGED_ACCOUNTS:
      return data.is_managed ? WhatToRemove::kAccount : WhatToRemove::kNothing;

    case kerberos::CLEAR_ONLY_UNMANAGED_ACCOUNTS:
      return !data.is_managed ? WhatToRemove::kAccount : WhatToRemove::kNothing;

    case kerberos::CLEAR_ONLY_UNMANAGED_REMEMBERED_PASSWORDS:
      return !data.is_managed ? WhatToRemove::kPassword
                              : WhatToRemove::kNothing;
  }
  return WhatToRemove::kNothing;
}

}  // namespace chromeos
