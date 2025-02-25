// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include "base/command_line.h"
#include "base/process/process.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/multiprocess_test.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/test_timeouts.h"
#include "base/unguessable_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/win_util.h"
#include "chrome/chrome_cleaner/ipc/ipc_test_util.h"
#include "chrome/chrome_cleaner/ipc/proto_chrome_prompt_ipc.h"
#include "chrome/chrome_cleaner/logging/scoped_logging.h"
#include "components/chrome_cleaner/public/constants/constants.h"
#include "components/chrome_cleaner/public/proto/chrome_prompt.pb.h"
#include "components/chrome_cleaner/public/proto/chrome_prompt_for_tests.pb.h"
#include "components/chrome_cleaner/test/test_name_helper.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace chrome_cleaner {
namespace {

using base::win::ScopedHandle;
using testing::Bool;
using testing::Values;
using PromptAcceptance = ProtoChromePromptIPC::PromptAcceptance;

constexpr char kIncludeUwSSwitch[] = "include-uws";
constexpr char kIncludeRegistryKeysSwitch[] = "include-registry-keys";
constexpr char kExpectedPromptResultSwitch[] = "expected-prompt-result";
constexpr char kExpectedChromeDisconnectPointSwitch[] =
    "expected-parent-disconnected";

const base::char16 kInvalidUTF16String[] = {0xDC00, 0xD800, 0xD800, 0xDFFF,
                                            0xDFFF, 0xDBFF, 0};
const base::FilePath kInvalidFilePath(kInvalidUTF16String);
const base::FilePath kNonASCIIFilePath(L"ééààçç");
const base::string16 kInvalidRegistryKey(kInvalidUTF16String);
const base::string16 kInvalidExtensionID(kInvalidUTF16String);

const base::FilePath kBadFilePath(L"/path/to/bad.dll");
const base::string16 kBadRegistryKey(L"HKCU:32\\Software\\ugly-uws\\nasty");

constexpr int kEarlyDisconnectionExitCode = 100;
constexpr int kSuccessExitCode = 0;
constexpr int kFailureExitCode = -1;

// Possible moments when the parent process can disconnect from the IPC to
// check connection error handling in the child process.
enum class ChromeDisconnectPoint {
  // Invalid value to initialize to before reading from the command-line.
  kUnspecified = 0,
  // The parent process will not try to disconnect while the child process
  // is running.
  kNone,
  // The parent process will disconnect before the child process sends
  // the version number.
  kOnStartup,
  // The parent process will disconnect after reading the version.
  kAfterVersion,
  // The parent process will disconnect after receiving the length of the first
  // request.
  kAfterRequestLength,
  // The parent process will disconnect after receiving the length of the close
  // connection message.
  kAfterCloseMessageLength,
  // The parent process will disconnect after receiving a message from the
  // child process and before sending out a response.
  kWhileProcessingChildRequest,
};

std::ostream& operator<<(std::ostream& stream,
                         ChromeDisconnectPoint parent_disconnected) {
  switch (parent_disconnected) {
    case ChromeDisconnectPoint::kUnspecified:
      stream << "Unspecified";
      break;
    case ChromeDisconnectPoint::kNone:
      stream << "NotDisconnected";
      break;
    case ChromeDisconnectPoint::kOnStartup:
      stream << "DisconnectedOnStartup";
      break;
    case ChromeDisconnectPoint::kAfterVersion:
      stream << "DisconnectedAfterVersion";
      break;
    case ChromeDisconnectPoint::kAfterRequestLength:
      stream << "DisconnectedAfterRequestLength";
      break;
    case ChromeDisconnectPoint::kWhileProcessingChildRequest:
      stream << "DisconnectedWhileProcessingChildRequest";
      break;
    case ChromeDisconnectPoint::kAfterCloseMessageLength:
      stream << "DisconnectedAfterCloseMessageLength";
      break;
  }
  return stream;
}

struct TestConfig {
  void EnhanceCommandLine(base::CommandLine* command_line) {
    if (uws_expected) {
      command_line->AppendSwitch(kIncludeUwSSwitch);
    }

    if (with_registry_keys) {
      command_line->AppendSwitch(kIncludeRegistryKeysSwitch);
    }

    command_line->AppendSwitchASCII(
        kExpectedPromptResultSwitch,
        base::NumberToString(static_cast<int>(expected_prompt_acceptance)));

    command_line->AppendSwitchASCII(
        kExpectedChromeDisconnectPointSwitch,
        base::NumberToString(static_cast<int>(expected_disconnection_point)));
  }

  bool uws_expected = false;
  bool with_registry_keys = false;
  PromptAcceptance expected_prompt_acceptance = PromptAcceptance::DENIED;
  ChromeDisconnectPoint expected_disconnection_point =
      ChromeDisconnectPoint::kNone;
};

enum class ServerPipeDirection {
  kInbound,
  kOutbound,
};

// This function is a taken from
// https://cs.chromium.org/chromium/src/chrome/browser/safe_browsing/chrome_cleaner/chrome_prompt_channel_win.cc
// to get the same behavior.
std::pair<ScopedHandle, ScopedHandle> CreateMessagePipe(
    ServerPipeDirection server_direction) {
  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  // Use this process's default access token.
  security_attributes.lpSecurityDescriptor = nullptr;
  // Handles to inherit will be added to the LaunchOptions explicitly.
  security_attributes.bInheritHandle = false;

  base::string16 pipe_name = base::UTF8ToUTF16(
      base::StrCat({"\\\\.\\pipe\\chrome-cleaner-",
                    base::UnguessableToken::Create().ToString()}));

  // Create the server end of the pipe.
  DWORD direction_flag = server_direction == ServerPipeDirection::kInbound
                             ? PIPE_ACCESS_INBOUND
                             : PIPE_ACCESS_OUTBOUND;
  ScopedHandle server_handle(::CreateNamedPipe(
      pipe_name.c_str(), direction_flag | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      /*nMaxInstances=*/1, /*nOutBufferSize=*/0, /*nInBufferSize=*/0,
      /*nDefaultTimeOut=*/0, &security_attributes));
  if (!server_handle.IsValid()) {
    PLOG(ERROR) << "Error creating server pipe";
    return std::make_pair(ScopedHandle(), ScopedHandle());
  }

  // The client pipe's read/write permissions are the opposite of the server's.
  DWORD client_mode = server_direction == ServerPipeDirection::kInbound
                          ? GENERIC_WRITE
                          : GENERIC_READ;

  // Create the client end of the pipe.
  ScopedHandle client_handle(::CreateFile(
      pipe_name.c_str(), client_mode, /*dwShareMode=*/0,
      /*lpSecurityAttributes=*/nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS,
      /*hTemplateFile=*/nullptr));
  if (!client_handle.IsValid()) {
    PLOG(ERROR) << "Error creating client pipe";
    return std::make_pair(ScopedHandle(), ScopedHandle());
  }

  // Wait for the client end to connect (this should return
  // ERROR_PIPE_CONNECTED immediately since it's already connected).
  if (::ConnectNamedPipe(server_handle.Get(), /*lpOverlapped=*/nullptr)) {
    LOG(ERROR) << "ConnectNamedPipe got an unexpected connection";
    return std::make_pair(ScopedHandle(), ScopedHandle());
  }
  const auto error = ::GetLastError();
  if (error != ERROR_PIPE_CONNECTED) {
    LOG(ERROR) << "ConnectNamedPipe returned unexpected error: "
               << logging::SystemErrorCodeToString(error);
    return std::make_pair(ScopedHandle(), ScopedHandle());
  }

  return std::make_pair(std::move(server_handle), std::move(client_handle));
}

void AppendHandleToCommandLine(base::CommandLine* command_line,
                               const std::string& switch_string,
                               HANDLE handle) {
  ASSERT_NE(command_line, nullptr);
  command_line->AppendSwitchASCII(
      switch_string, base::NumberToString(base::win::HandleToUint32(handle)));
}

// In this test the class representing the cleaner is contained in the parent
// process so it needs the server ends of the pipes. The class that represents
// chrome is in the child process and gets the client end of the pipes.
bool PrepareForCleaner(base::CommandLine* command_line,
                       base::HandlesToInheritVector* handles_to_inherit,
                       ScopedHandle* request_read_handle_,
                       ScopedHandle* request_write_handle_,
                       ScopedHandle* response_read_handle_,
                       ScopedHandle* response_write_handle_) {
  // Requests flow from the cleaner to Chrome.
  std::tie(*request_write_handle_, *request_read_handle_) =
      CreateMessagePipe(ServerPipeDirection::kOutbound);

  // Responses flow from Chrome to the cleaner.
  std::tie(*response_read_handle_, *response_write_handle_) =
      CreateMessagePipe(ServerPipeDirection::kInbound);

  if (!request_read_handle_->IsValid() || !request_write_handle_->IsValid() ||
      !response_read_handle_->IsValid() || !response_write_handle_->IsValid()) {
    return false;
  }

  DCHECK(command_line);
  DCHECK(handles_to_inherit);
  AppendHandleToCommandLine(command_line,
                            chrome_cleaner::kChromeWriteHandleSwitch,
                            response_write_handle_->Get());
  handles_to_inherit->push_back(response_write_handle_->Get());
  AppendHandleToCommandLine(command_line,
                            chrome_cleaner::kChromeReadHandleSwitch,
                            request_read_handle_->Get());
  handles_to_inherit->push_back(request_read_handle_->Get());
  return true;
}

// Provides the same kind of inputs and outputs on the pipes that Chrome would
// during the prompt process.
class MockChrome {
 public:
  MockChrome(base::win::ScopedHandle request_read_handle,
             base::win::ScopedHandle response_write_handle)
      : request_read_handle_(std::move(request_read_handle)),
        response_write_handle_(std::move(response_write_handle)) {}

  // This is needed for single process tests to avoid blocking on incomplete
  // operations.
  void CancelAllOperations() {
    ::CancelIoEx(request_read_handle_.Get(), nullptr);
    ::CancelIoEx(response_write_handle_.Get(), nullptr);
  }

  // Read and validate the version sent by the cleaner in a blocking way.
  template <typename ValueType>
  bool ReadValue(ValueType* value) {
    uint32_t read_size = sizeof(*value);

    DWORD bytes_read = 0;
    bool success = ::ReadFile(request_read_handle_.Get(), value, read_size,
                              &bytes_read, nullptr);
    if (!success) {
      PLOG(ERROR) << "Could not read value.";
      return false;
    }

    if (bytes_read != read_size) {
      LOG(ERROR) << "Read the wrong number of bytes: " << bytes_read
                 << ". Should have been: " << read_size;
      return false;
    }

    return true;
  }

  bool ReadRequest(uint32_t request_length,
                   chrome_cleaner::ChromePromptRequest* request) {
    DCHECK(request_read_handle_.IsValid());

    DWORD bytes_read = 0;
    std::string request_content;
    // Read the request.
    bool success =
        ::ReadFile(request_read_handle_.Get(),
                   base::WriteInto(&request_content, request_length + 1),
                   request_length, &bytes_read, nullptr);

    if (!success) {
      PLOG(ERROR) << "Could not read request.";
      return false;
    }

    if (bytes_read != request_length) {
      LOG(ERROR) << "Read the wrong number of bytes: " << bytes_read
                 << ". Should have been: " << request_length;
      return false;
    }

    if (!request->ParseFromString(request_content)) {
      LOG(ERROR) << "Could not parse request.";
      return false;
    }

    return true;
  }

  // Blocking write of anything by value on the pipe.
  template <typename T>
  bool WriteByValue(T value) {
    DWORD bytes_written = 0;
    bool success = ::WriteFile(response_write_handle_.Get(), &value,
                               sizeof(value), &bytes_written, nullptr);

    if (!success) {
      PLOG(ERROR) << "Could not write to pipe.";
      return false;
    }

    if (bytes_written != sizeof(value)) {
      LOG(ERROR) << "Wrote the wrong number of bytes";
      return false;
    }

    return true;
  }

  // Blocking write of anything by pointer on the pipe.
  // Does not own the memory.
  // Set |should_succeed|=false when testing a failed operation.
  template <typename T>
  bool WriteByPointer(const T* ptr, uint32_t size, bool should_succeed = true) {
    DWORD bytes_written = 0;
    bool success = ::WriteFile(response_write_handle_.Get(), ptr, size,
                               &bytes_written, nullptr);

    if (should_succeed && !success) {
      PLOG(ERROR) << "Could not write to pipe.";
      return false;
    }

    // We should not validate |bytes_written| if we know the call will fail.
    if (should_succeed) {
      if (bytes_written != size) {
        LOG(ERROR) << "Wrote the wrong number of bytes";
        return false;
      }
    }

    return true;
  }

  bool SendMessage(google::protobuf::MessageLite& message) {
    std::string message_content;
    if (!message.SerializeToString(&message_content)) {
      LOG(ERROR) << "Could not serialize message for sending";
      return false;
    }

    uint32_t message_size = message_content.size();
    if (!WriteByValue(message_size)) {
      return false;
    }

    if (!WriteByPointer(message_content.data(), message_content.size())) {
      return false;
    }

    return true;
  }

  // Send a response to the cleaner with the expected values.
  bool SendResponse(PromptAcceptance prompt_acceptance) {
    DCHECK(response_write_handle_.IsValid());

    chrome_cleaner::PromptUserResponse response;

    switch (prompt_acceptance) {
      case PromptAcceptance::DENIED:
        response.set_prompt_acceptance(
            chrome_cleaner::PromptUserResponse::DENIED);
        break;
      case PromptAcceptance::ACCEPTED_WITH_LOGS:
        response.set_prompt_acceptance(
            chrome_cleaner::PromptUserResponse::ACCEPTED_WITH_LOGS);
        break;
      case PromptAcceptance::ACCEPTED_WITHOUT_LOGS:
        response.set_prompt_acceptance(
            chrome_cleaner::PromptUserResponse::ACCEPTED_WITHOUT_LOGS);
        break;
      case PromptAcceptance::UNSPECIFIED:
      default:
        response.set_prompt_acceptance(
            chrome_cleaner::PromptUserResponse::UNSPECIFIED);
        break;
    }

    return SendMessage(response);
  }

 private:
  base::win::ScopedHandle request_read_handle_;
  base::win::ScopedHandle response_write_handle_;
};

// This class mocks the Chrome side of the IPC.
// A note on logging: We want errors to be logged using LOG(*) calls so they are
// captured by the ScopedLogging member. This in turn ensures that all messages
// are visible on buildbot outputs. This is why LOG(*) statements are used even
// though it would have been more readable to use the facilities provided by
// Gtest to log.
class ChildProcess {
 public:
  ChildProcess()
      : scopped_logging_(
            std::make_unique<ScopedLogging>(internal::GetLogPathSuffix())) {
    mock_chrome_ = std::make_unique<MockChrome>(
        ExtractHandleFromCommandLine(chrome_cleaner::kChromeReadHandleSwitch),
        ExtractHandleFromCommandLine(chrome_cleaner::kChromeWriteHandleSwitch));

    expected_disconnect_point_ = GetEnumFromCommandLine<ChromeDisconnectPoint>(
        kExpectedChromeDisconnectPointSwitch);

    expected_prompt_acceptance_ =
        GetEnumFromCommandLine<PromptAcceptance>(kExpectedPromptResultSwitch);
  }

  base::win::ScopedHandle ExtractHandleFromCommandLine(
      const std::string& handle_switch) {
    uint32_t handle_value = 0;
    if (!base::StringToUint(command_line_->GetSwitchValueNative(handle_switch),
                            &handle_value)) {
      LOG(ERROR) << handle_switch << " not found on commandline";
      return base::win::ScopedHandle();
    }
    HANDLE handle = base::win::Uint32ToHandle(handle_value);
    return base::win::ScopedHandle(handle);
  }

  template <typename Enum>
  Enum GetEnumFromCommandLine(const std::string& flag) {
    int val = 0;
    if (!base::StringToInt(command_line_->GetSwitchValueASCII(flag), &val)) {
      LOG(ERROR) << "Could not get flag:" << flag << " from the command line";
    }
    return static_cast<Enum>(val);
  }

  // This simulates the pipes getting cut for any reason.
  void CloseConnectionIfDisconectionPointReached(
      ChromeDisconnectPoint disconnect_point) {
    if (expected_disconnect_point_ == disconnect_point) {
      // Immediately exit the child process. Destructors do not get called so we
      // do not properly clean up. This mimics a real spurious disconnect.
      exit(kEarlyDisconnectionExitCode);
    }
  }

  // Execute all steps of the prompt according to passed in test config.
  bool Run() {
    DCHECK_NE(expected_disconnect_point_, ChromeDisconnectPoint::kUnspecified);
    DCHECK_NE(expected_prompt_acceptance_, PromptAcceptance::UNSPECIFIED);

    CloseConnectionIfDisconectionPointReached(
        ChromeDisconnectPoint::kOnStartup);

    // Read the incoming version number, this is NOT echoed back.
    constexpr uint8_t kExpectedVersion = 1;
    uint8_t version = 0;
    if (!mock_chrome_->ReadValue(&version)) {
      return false;
    }

    if (version != kExpectedVersion) {
      LOG(ERROR) << "Wrong version received: " << version;
      return false;
    }

    CloseConnectionIfDisconectionPointReached(
        ChromeDisconnectPoint::kAfterVersion);

    uint32_t request_length = 0;
    if (!mock_chrome_->ReadValue(&request_length)) {
      return false;
    }

    CloseConnectionIfDisconectionPointReached(
        ChromeDisconnectPoint::kAfterRequestLength);

    chrome_cleaner::ChromePromptRequest request;
    if (!mock_chrome_->ReadRequest(request_length, &request)) {
      return false;
    }

    CloseConnectionIfDisconectionPointReached(
        ChromeDisconnectPoint::kWhileProcessingChildRequest);

    int32_t expected_files_to_delete_size =
        command_line_->HasSwitch(kIncludeUwSSwitch) ? 1 : 0;
    if (request.prompt_user().files_to_delete_size() !=
        expected_files_to_delete_size) {
      LOG(ERROR) << "Wrong number of files to delete received.";
      return false;
    }
    if (expected_files_to_delete_size == 1) {
      std::string file_path_utf8;
      base::UTF16ToUTF8(kBadFilePath.value().c_str(),
                        kBadFilePath.value().size(), &file_path_utf8);
      if (request.prompt_user().files_to_delete(0) != file_path_utf8) {
        LOG(ERROR) << "Wrong value for file to delete";
        return false;
      }
    }

    int32_t expecteed_registry_keys_size =
        command_line_->HasSwitch(kIncludeRegistryKeysSwitch) ? 1 : 0;
    if (request.prompt_user().registry_keys_size() !=
        expecteed_registry_keys_size) {
      LOG(ERROR) << "Wrong number of registry keys to delete";
      return false;
    }
    if (expecteed_registry_keys_size == 1) {
      if (request.prompt_user().registry_keys(0) !=
          base::UTF16ToUTF8(kBadRegistryKey)) {
        LOG(ERROR) << "Wrong value for registry key";
        return false;
      }
    }

    if (request.prompt_user().extension_ids_size() != 0) {
      LOG(ERROR) << "Cleaning of UwsE not supported. None should be present in "
                    "message";
      return false;
    }

    // Send back a success message.
    if (!mock_chrome_->SendResponse(expected_prompt_acceptance_)) {
      return false;
    }

    // Receive the close connection message.
    uint32_t close_message_length = 0;
    if (!mock_chrome_->ReadValue(&close_message_length)) {
      return false;
    }

    CloseConnectionIfDisconectionPointReached(
        ChromeDisconnectPoint::kAfterCloseMessageLength);

    chrome_cleaner::ChromePromptRequest close_message;
    if (!mock_chrome_->ReadRequest(close_message_length, &close_message)) {
      return false;
    }

    if (!close_message.has_close_connection()) {
      LOG(ERROR) << "Wrong close connection message type";
      return false;
    }

    return true;
  }

 private:
  std::unique_ptr<MockChrome> mock_chrome_;
  std::unique_ptr<ScopedLogging> scopped_logging_;

  ChromeDisconnectPoint expected_disconnect_point_ =
      ChromeDisconnectPoint::kUnspecified;

  PromptAcceptance expected_prompt_acceptance_ = PromptAcceptance::UNSPECIFIED;
  const base::CommandLine* command_line_ =
      base::CommandLine::ForCurrentProcess();
};

// This mimics the Chrome side of the IPC.
MULTIPROCESS_TEST_MAIN(ProtoChromePromptIPCClientMain) {
  base::test::ScopedTaskEnvironment scoped_task_environment;

  ChildProcess child_process;
  if (!child_process.Run()) {
    return kFailureExitCode;
  }

  return ::testing::Test::HasFailure() ? kFailureExitCode : kSuccessExitCode;
}

class ProtoChromePromptIPCTest
    : public ::testing::TestWithParam<
          std::tuple<bool, bool, PromptAcceptance, ChromeDisconnectPoint>> {
 private:
  base::test::ScopedTaskEnvironment scoped_task_environment;
};

class ParentProcess {
 public:
  bool Initialize() {
    // Inject the flags related to the the config in the command line.
    test_config_.EnhanceCommandLine(&command_line_);

    return PrepareForCleaner(&command_line_,
                             &launch_options_.handles_to_inherit,
                             &request_read_handle_, &request_write_handle_,
                             &response_read_handle_, &response_write_handle_);
  }

  void ValidateAcceptance(PromptAcceptance prompt_acceptance) {
    EXPECT_EQ(prompt_acceptance, test_config_.expected_prompt_acceptance);
    main_runloop_.Quit();
  }

  // A call to this function indicates that the connection was closed
  // prematurely which signifies an error.
  void ConnectionWasClosed() {
    error_occurred_ = true;
    main_runloop_.Quit();
  }

  void ConnectionWasClosedAfterDone() {
    main_runloop_.Quit();
    FAIL() << "ConnectionWasClosedAfterDone should only be called in the Mojo "
              "IPC implementation";
  }

  void Run() {
    ASSERT_TRUE(internal::DeleteChildProcessLogs());

    // Pass the command to the child process and launch the child process.
    base::Process child_process = base::SpawnMultiProcessTestChild(
        "ProtoChromePromptIPCClientMain", command_line_, launch_options_);
    ASSERT_TRUE(child_process.IsRunning());

    // Close our references to the handles as they are now handled by the child
    // process.
    request_read_handle_.Close();
    response_write_handle_.Close();

    ProtoChromePromptIPC chrome_prompt_ipc(std::move(response_read_handle_),
                                           std::move(request_write_handle_));

    // Send the protocol version, blocking the child process until it is read.
    auto error_handler = std::make_unique<ChromePromptIPCTestErrorHandler>(
        base::BindOnce(&ParentProcess::ConnectionWasClosed,
                       base::Unretained(this)),
        base::BindOnce(&ParentProcess::ConnectionWasClosedAfterDone,
                       base::Unretained(this)));

    chrome_prompt_ipc.Initialize(error_handler.get());

    std::vector<base::FilePath> files_to_delete;
    if (test_config_.uws_expected) {
      files_to_delete.push_back(kBadFilePath);
    }

    std::vector<base::string16> registry_keys;
    if (test_config_.with_registry_keys) {
      registry_keys.push_back(kBadRegistryKey);
    }

    // Send the user prompt, blocking the child process until it is read.
    // The test thread will block too until a response is received.
    chrome_prompt_ipc.PostPromptUserTask(
        files_to_delete, registry_keys, {},
        base::BindOnce(&ParentProcess::ValidateAcceptance,
                       base::Unretained(this)));

    main_runloop_.Run();

    // During a normal execution where the full prompt exchange takes place
    // there should be no errors.
    bool should_have_errors = test_config_.expected_disconnection_point !=
                              ChromeDisconnectPoint::kNone;
    EXPECT_EQ(error_occurred_, should_have_errors);

    WaitForChildProcess(child_process);
  }

  void WaitForChildProcess(const base::Process& child_process) {
    // Expect the return code that can indicate one of three outcomes
    // 1: Success
    // 2: Early disconnection which is expected if we simulate Chrome crashing.
    // 3: Failure which will be triggered if any EXPECT or ASSERT call fails in
    // the child process.
    int rv = -1;
    bool success = base::WaitForMultiprocessTestChildExit(
        child_process, TestTimeouts::action_timeout(), &rv);
    ASSERT_TRUE(success);

    int expected_exit_code = kSuccessExitCode;
    if (test_config_.expected_disconnection_point !=
        ChromeDisconnectPoint::kNone) {
      expected_exit_code = kEarlyDisconnectionExitCode;
    }

    EXPECT_EQ(expected_exit_code, rv);

    if (!success || rv != 0) {
      internal::PrintChildProcessLogs();
    }
  }

  TestConfig& GetTestConfig() { return test_config_; }

 private:
  // Handles for Chrome.
  ScopedHandle request_read_handle_;
  ScopedHandle response_write_handle_;

  // Handles for the cleaner.
  ScopedHandle request_write_handle_;
  ScopedHandle response_read_handle_;

  TestConfig test_config_;

  base::CommandLine command_line_ =
      base::GetMultiProcessTestChildBaseCommandLine();

  base::LaunchOptions launch_options_;

  // Blocks until we receive the response from Chrome or an error occurs.
  base::RunLoop main_runloop_;
  bool error_occurred_ = false;
};

// This contains calls to the chrome_cleaner_ipc implementation.
TEST_P(ProtoChromePromptIPCTest, Communication) {
  ParentProcess parent_process;
  TestConfig& test_config = parent_process.GetTestConfig();
  std::tie(test_config.uws_expected, test_config.with_registry_keys,
           test_config.expected_prompt_acceptance,
           test_config.expected_disconnection_point) = GetParam();

  ASSERT_TRUE(parent_process.Initialize());
  parent_process.Run();
}

INSTANTIATE_TEST_SUITE_P(NoUwSPresent,
                         ProtoChromePromptIPCTest,
                         testing::Combine(
                             /*[>uws_expected=<]*/ Values(false),
                             /*[>with_registry_keys=<]*/ Values(false),
                             Values(PromptAcceptance::DENIED),
                             Values(ChromeDisconnectPoint::kNone,
                                    ChromeDisconnectPoint::kOnStartup)),
                         GetParamNameForTest());

INSTANTIATE_TEST_SUITE_P(
    UwSPresent,
    ProtoChromePromptIPCTest,
    testing::Combine(
        /*uws_expected=*/Values(true),
        /*with_registry_keys=*/Bool(),
        Values(PromptAcceptance::ACCEPTED_WITH_LOGS,
               PromptAcceptance::ACCEPTED_WITHOUT_LOGS,
               PromptAcceptance::DENIED),
        Values(ChromeDisconnectPoint::kNone,
               ChromeDisconnectPoint::kOnStartup,
               ChromeDisconnectPoint::kAfterVersion,
               ChromeDisconnectPoint::kAfterRequestLength,
               ChromeDisconnectPoint::kAfterCloseMessageLength,
               ChromeDisconnectPoint::kWhileProcessingChildRequest)),
    GetParamNameForTest());

class ProtoChromePromptSameProcessTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Requests flow from the cleaner to Chrome.
    base::win::ScopedHandle request_read_handle;
    base::win::ScopedHandle request_write_handle;
    std::tie(request_write_handle, request_read_handle) =
        CreateMessagePipe(ServerPipeDirection::kOutbound);

    // Responses flow from Chrome to the cleaner.
    base::win::ScopedHandle response_read_handle;
    base::win::ScopedHandle response_write_handle;
    std::tie(response_read_handle, response_write_handle) =
        CreateMessagePipe(ServerPipeDirection::kInbound);

    EXPECT_TRUE(request_read_handle.IsValid());
    EXPECT_TRUE(request_write_handle.IsValid());
    EXPECT_TRUE(response_read_handle.IsValid());
    EXPECT_TRUE(response_write_handle.IsValid());

    mock_chrome_ = std::make_unique<MockChrome>(
        std::move(request_read_handle), std::move(response_write_handle));

    chrome_prompt_ipc_ = std::make_unique<ProtoChromePromptIPC>(
        std::move(response_read_handle), std::move(request_write_handle));

    error_handler_ = std::make_unique<ChromePromptIPCTestErrorHandler>(
        base::BindOnce(&ProtoChromePromptSameProcessTest ::ConnectionWasClosed,
                       base::Unretained(this)),
        base::BindOnce(
            &ProtoChromePromptSameProcessTest::ConnectionWasClosedAfterDone,
            base::Unretained(this)));
  }

  void InitCommunication() {
    chrome_prompt_ipc_->Initialize(error_handler_.get());
    uint8_t version = 0;
    EXPECT_TRUE(mock_chrome_->ReadValue(&version));
  }

  // A call to this function indicates that the connection was closed
  // prematurely which signifies an error.
  void ConnectionWasClosed() {
    // Unblock the main test thread is there are pending operations.
    mock_chrome_->CancelAllOperations();

    error_occurred_ = true;
    main_runloop_.Quit();
  }

  void ConnectionWasClosedAfterDone() {
    main_runloop_.Quit();
    FAIL() << "ConnectionWasClosedAfterDone should only be called in the Mojo "
              "IPC implementation";
  }

  void ExpectMessage() {
    uint32_t request_length = 0;
    EXPECT_TRUE(mock_chrome_->ReadValue(&request_length));

    chrome_cleaner::ChromePromptRequest request;
    EXPECT_TRUE(mock_chrome_->ReadRequest(request_length, &request));
  }

  void ValidateAcceptance(PromptAcceptance expected_prompt_acceptance,
                          PromptAcceptance prompt_acceptance) {
    EXPECT_EQ(prompt_acceptance, expected_prompt_acceptance);
    main_runloop_.Quit();
  }

 protected:
  base::test::ScopedTaskEnvironment scoped_task_environment;

  std::unique_ptr<MockChrome> mock_chrome_;
  std::unique_ptr<ProtoChromePromptIPC> chrome_prompt_ipc_;
  std::unique_ptr<ChromePromptIPCTestErrorHandler> error_handler_;
  bool error_occurred_ = false;

  // Blocks until we receive the response from Chrome.
  base::RunLoop main_runloop_;
};

TEST_F(ProtoChromePromptSameProcessTest, InvalidUTF16Path) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kInvalidFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::DENIED));

  // Providing an invalid file path will trigger an immediate denial from the
  // cleaner side. No communication will happen with Chrome so we do not call
  // ExpectMessage() as it would timeout waiting for the prompt message.
  main_runloop_.Run();
  // This is not considered an error but validation of user provided input.
  ASSERT_FALSE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, InvalidUTF16RegistryKey) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {}, {kInvalidRegistryKey}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::DENIED));

  // Providing an invalid registry key will trigger an immediate denial from the
  // cleaner side. No communication will happen with Chrome so we do not call
  // ExpectMessage() as it would timeout waiting for the prompt message.
  main_runloop_.Run();

  // This is not considered an error but validation of user provided input.
  ASSERT_FALSE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, InvalidUTF16ExtensionID) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {}, {}, {kInvalidExtensionID},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::DENIED));

  // Providing an invalid extension id will trigger an immediate denial from the
  // cleaner side. No communication will happen with Chrome so we do not call
  // ExpectMessage() as it would timeout waiting for the prompt message.
  main_runloop_.Run();

  // This is not considered an error but validation of user provided input.
  ASSERT_FALSE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, ValidNonASCIIPath) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kNonASCIIFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::ACCEPTED_WITH_LOGS));

  // Expect the prompt message.
  ExpectMessage();

  // Send back the response.
  EXPECT_TRUE(mock_chrome_->SendResponse(PromptAcceptance::ACCEPTED_WITH_LOGS));

  // Expect the close connection message.
  ExpectMessage();

  main_runloop_.Run();
  // There are no errors here. Non-ASCII characters are supported.
  ASSERT_FALSE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, ReponseSizeOverMax) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kNonASCIIFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::ACCEPTED_WITH_LOGS));

  // Expect the prompt message.
  ExpectMessage();

  // Size over max.
  uint32_t invalid_size = ProtoChromePromptIPC::kMaxMessageLength + 1;
  EXPECT_TRUE(mock_chrome_->WriteByValue(invalid_size));

  // Notice the absence of ExpectMessage() here. The cleaner will never get to
  // sending a close connection message.

  main_runloop_.Run();

  // This is an error scenario.
  ASSERT_TRUE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, ReponseSizeZero) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kNonASCIIFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::ACCEPTED_WITH_LOGS));

  // Expect the prompt message.
  ExpectMessage();

  // Size of zero.
  uint32_t invalid_size = 0;
  EXPECT_TRUE(mock_chrome_->WriteByValue(invalid_size));

  // Notice the absence of ExpectMessage() here. The cleaner will never get to
  // sending a close connection message.

  main_runloop_.Run();

  // This is an error scenario.
  ASSERT_TRUE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, ReponseSizeSentTooSmall) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kNonASCIIFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::ACCEPTED_WITH_LOGS));

  // Expect the prompt message.
  ExpectMessage();

  chrome_cleaner::PromptUserResponse response;
  response.set_prompt_acceptance(
      chrome_cleaner::PromptUserResponse::ACCEPTED_WITH_LOGS);

  std::string response_content;
  response.SerializeToString(&response_content);

  // Size too small.
  uint32_t invalid_size = response_content.size() - 1;
  EXPECT_TRUE(mock_chrome_->WriteByValue(invalid_size));

  // Send the correct data.
  EXPECT_TRUE(mock_chrome_->WriteByPointer(response_content.data(),
                                           response_content.size(),
                                           /*should_succeed=*/false));

  // Notice the absence of ExpectMessage() here. The cleaner will never get to
  // sending a close connection message.

  main_runloop_.Run();

  // This is an error scenario.
  ASSERT_TRUE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, ReponseSizeSentTooBig) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kNonASCIIFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::ACCEPTED_WITH_LOGS));

  // Expect the prompt message.
  ExpectMessage();

  chrome_cleaner::PromptUserResponse response;
  response.set_prompt_acceptance(
      chrome_cleaner::PromptUserResponse::ACCEPTED_WITH_LOGS);

  std::string response_content;
  response.SerializeToString(&response_content);

  // Size too big.
  uint32_t invalid_size = response_content.size() + 1;
  EXPECT_TRUE(mock_chrome_->WriteByValue(invalid_size));

  // Send the correct data.
  EXPECT_TRUE(mock_chrome_->WriteByPointer(response_content.data(),
                                           response_content.size()));

  // Notice the absence of ExpectMessage() here. The cleaner will never get to
  // sending a close connection message.

  main_runloop_.Run();

  // This is an error scenario.
  ASSERT_TRUE(error_occurred_);
}

TEST_F(ProtoChromePromptSameProcessTest, OutOfRangeAcceptance) {
  InitCommunication();

  chrome_prompt_ipc_->PostPromptUserTask(
      {kNonASCIIFilePath}, {}, {},
      base::BindOnce(&ProtoChromePromptSameProcessTest::ValidateAcceptance,
                     base::Unretained(this),
                     chrome_cleaner::PromptAcceptance::UNSPECIFIED));

  // Expect the prompt message.
  ExpectMessage();

  // Send back the response with an out of range acceptance.
  chrome_cleaner_test_only::PromptUserResponse response;
  response.set_prompt_acceptance(
      chrome_cleaner_test_only::
          PromptUserResponse_PromptAcceptance_FOR_TESTS_ONLY);
  mock_chrome_->SendMessage(response);

  // Expect the close connection message.
  ExpectMessage();

  main_runloop_.Run();

  // There are no errors here. An out of range PromptAcceptance value means
  // that the protocol evolved and parsing it as UNSPECIFIED will not trigger
  // a cleaning that the user actually denied.
  ASSERT_FALSE(error_occurred_);
}

}  // namespace
}  // namespace chrome_cleaner
