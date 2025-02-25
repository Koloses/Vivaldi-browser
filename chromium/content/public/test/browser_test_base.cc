// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/test/browser_test_base.h"

#include <stddef.h>

#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "base/base_switches.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/debug/stack_trace.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/message_loop/message_loop.h"
#include "base/message_loop/message_loop_current.h"
#include "base/rand_util.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/post_task.h"
#include "base/test/bind_test_util.h"
#include "base/test/test_timeouts.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "content/browser/browser_main_loop.h"
#include "content/browser/browser_thread_impl.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/scheduler/browser_task_executor.h"
#include "content/browser/startup_data_impl.h"
#include "content/browser/startup_helper.h"
#include "content/browser/tracing/tracing_controller_impl.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/system_connector.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/main_function_params.h"
#include "content/public/common/network_service_util.h"
#include "content/public/common/service_names.mojom.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/no_renderer_crashes_assertion.h"
#include "content/public/test/test_launcher.h"
#include "content/public/test/test_utils.h"
#include "content/test/content_browser_sanity_checker.h"
#include "gpu/command_buffer/service/gpu_switches.h"
#include "gpu/config/gpu_switches.h"
#include "mojo/public/cpp/bindings/sync_call_restrictions.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/mojom/network_service_test.mojom.h"
#include "services/service_manager/embedder/switches.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/base/platform_window_defaults.h"
#include "ui/compositor/compositor_switches.h"
#include "ui/display/display_switches.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_switches.h"

#if defined(OS_ANDROID)
#include "components/discardable_memory/service/discardable_shared_memory_manager.h"  // nogncheck
#include "content/app/mojo/mojo_init.h"
#include "content/app/service_manager_environment.h"
#include "content/common/url_schemes.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/common/content_paths.h"
#include "testing/android/native_test/native_browser_test_support.h"
#include "ui/base/ui_base_paths.h"

#ifdef V8_USE_EXTERNAL_STARTUP_DATA
#include "gin/v8_initializer.h"
#endif
#endif

#if defined(OS_MACOSX)
#include "ui/events/test/event_generator.h"
#include "ui/views/test/event_generator_delegate_mac.h"
#endif

#if defined(OS_POSIX)
#include "base/process/process_handle.h"
#endif

#if defined(USE_AURA)
#include "content/browser/compositor/image_transport_factory.h"
#include "ui/aura/test/event_generator_delegate_aura.h"  // nogncheck
#endif

#include "app/vivaldi_apptools.h"
#include "base/vivaldi_switches.h"

namespace content {
namespace {

#if defined(OS_POSIX)
// On SIGSEGV or SIGTERM (sent by the runner on timeouts), dump a stack trace
// (to make debugging easier) and also exit with a known error code (so that
// the test framework considers this a failure -- http://crbug.com/57578).
// Note: We only want to do this in the browser process, and not forked
// processes. That might lead to hangs because of locks inside tcmalloc or the
// OS. See http://crbug.com/141302.
int g_browser_process_pid;

void DumpStackTraceSignalHandler(int signal) {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          service_manager::switches::kDisableInProcessStackTraces) &&
      g_browser_process_pid == base::GetCurrentProcId()) {
    std::string message("BrowserTestBase received signal: ");
    message += strsignal(signal);
    message += ". Backtrace:\n";
    logging::RawLog(logging::LOG_ERROR, message.c_str());
    auto stack_trace = base::debug::StackTrace();
    stack_trace.OutputToStream(&std::cerr);
#if defined(OS_ANDROID)
    // Also output the trace to logcat on Android.
    stack_trace.Print();
#endif
  }
  _exit(128 + signal);
}
#endif  // defined(OS_POSIX)

void RunTaskOnRendererThread(base::OnceClosure task,
                             base::OnceClosure quit_task) {
  std::move(task).Run();
  base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                           std::move(quit_task));
}

void TraceStopTracingComplete(const base::Closure& quit,
                                   const base::FilePath& file_path) {
  LOG(ERROR) << "Tracing written to: " << file_path.value();
  quit.Run();
}

// See SetInitialWebContents comment for more information.
class InitialNavigationObserver : public WebContentsObserver {
 public:
  InitialNavigationObserver(WebContents* web_contents,
                            base::OnceClosure callback)
      : WebContentsObserver(web_contents), callback_(std::move(callback)) {}
  // WebContentsObserver implementation:
  void DidStartNavigation(NavigationHandle* navigation_handle) override {
    if (callback_)
      std::move(callback_).Run();
  }

 private:
  base::OnceClosure callback_;

  DISALLOW_COPY_AND_ASSIGN(InitialNavigationObserver);
};

}  // namespace

extern int BrowserMain(const MainFunctionParams&);

BrowserTestBase::BrowserTestBase()
    : expected_exit_code_(0),
      enable_pixel_output_(false),
      use_software_compositing_(false),
      set_up_called_(false) {
  ui::test::EnableTestConfigForPlatformWindows();

#if defined(OS_POSIX)
  handle_sigterm_ = true;
#endif

  // This is called through base::TestSuite initially. It'll also be called
  // inside BrowserMain, so tell the code to ignore the check that it's being
  // called more than once
  base::i18n::AllowMultipleInitializeCallsForTesting();

  embedded_test_server_ = std::make_unique<net::EmbeddedTestServer>();

#if defined(USE_AURA)
  ui::test::EventGeneratorDelegate::SetFactoryFunction(
      base::BindRepeating(&aura::test::EventGeneratorDelegateAura::Create));
#elif defined(OS_MACOSX)
  ui::test::EventGeneratorDelegate::SetFactoryFunction(
      base::BindRepeating(&views::test::CreateEventGeneratorDelegateMac));
#endif
}

BrowserTestBase::~BrowserTestBase() {
  CHECK(set_up_called_ || IsSkipped())
      << "SetUp was not called. This probably means that the "
         "developer has overridden the method and not called "
         "the superclass version. In this case, the test "
         "does not run and reports a false positive result.";
}

void BrowserTestBase::SetUp() {
  set_up_called_ = true;

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  // Features that depend on external factors (e.g. memory pressure monitor) can
  // disable themselves based on the switch below (to ensure that browser tests
  // behave deterministically / do not flakily change behavior based on external
  // factors).
  command_line->AppendSwitch(switches::kBrowserTest);

  // Override the child process connection timeout since tests can exceed that
  // when sharded.
  command_line->AppendSwitchASCII(
      switches::kIPCConnectionTimeout,
      base::NumberToString(TestTimeouts::action_max_timeout().InSeconds()));

  // The tests assume that file:// URIs can freely access other file:// URIs.
  if (AllowFileAccessFromFiles())
    command_line->AppendSwitch(switches::kAllowFileAccessFromFiles);

  command_line->AppendSwitch(switches::kDomAutomationController);

  // It is sometimes useful when looking at browser test failures to know which
  // GPU blacklisting decisions were made.
  command_line->AppendSwitch(switches::kLogGpuControlListDecisions);

  // Make sure software compositing tests don't attempt to force hardware
  // compositing.
  if (use_software_compositing_) {
    command_line->AppendSwitch(switches::kDisableGpu);
    command_line->RemoveSwitch(switches::kDisableSoftwareCompositingFallback);
#if defined(USE_X11)
    // If Vulkan is enabled, make sure it uses SwiftShader instead of native,
    // though only on platforms where it is supported.
    // TODO(samans): Support Swiftshader on more platforms.
    // https://crbug.com/963988
    if (command_line->HasSwitch(switches::kUseVulkan)) {
      command_line->AppendSwitchASCII(
          switches::kUseVulkan, switches::kVulkanImplementationNameSwiftshader);
    }
#endif
  }

  // The layout of windows on screen is unpredictable during tests, so disable
  // occlusion when running browser tests.
  base::CommandLine::ForCurrentProcess()->AppendSwitch(
      switches::kDisableBackgroundingOccludedWindowsForTesting);

#if defined(USE_AURA)
  // Most tests do not need pixel output, so we don't produce any. The command
  // line can override this behaviour to allow for visual debugging.
  if (command_line->HasSwitch(switches::kEnablePixelOutputInTests))
    enable_pixel_output_ = true;

  if (command_line->HasSwitch(switches::kDisableGLDrawingForTests)) {
    NOTREACHED() << "kDisableGLDrawingForTests should not be used as it"
                    "is chosen by tests. Use kEnablePixelOutputInTests "
                    "to enable pixel output.";
  }

  // Don't enable pixel output for browser tests unless they override and force
  // us to, or it's requested on the command line.
  if (!enable_pixel_output_ && !use_software_compositing_)
    command_line->AppendSwitch(switches::kDisableGLDrawingForTests);
#endif

  bool use_software_gl = true;

  // We usually use software GL as this works on all bots. The command
  // line can override this behaviour to use hardware GL.
  if (command_line->HasSwitch(switches::kUseGpuInTests))
    use_software_gl = false;

  // Some bots pass this flag when they want to use hardware GL.
  if (command_line->HasSwitch("enable-gpu"))
    use_software_gl = false;

#if defined(OS_MACOSX)
  // On Mac we always use hardware GL.
  use_software_gl = false;
#endif

#if defined(OS_ANDROID)
  // On Android we always use hardware GL.
  use_software_gl = false;
#endif

#if defined(OS_CHROMEOS)
  // If the test is running on the chromeos envrionment (such as
  // device or vm bots), we use hardware GL.
  if (base::SysInfo::IsRunningOnChromeOS())
    use_software_gl = false;
#endif

  if (use_software_gl && !use_software_compositing_)
    command_line->AppendSwitch(switches::kOverrideUseSoftwareGLForTests);

  // Use an sRGB color profile to ensure that the machine's color profile does
  // not affect the results.
  command_line->AppendSwitchASCII(switches::kForceDisplayColorProfile, "srgb");

  test_host_resolver_ = std::make_unique<TestHostResolver>();

  ContentBrowserSanityChecker scoped_enable_sanity_checks;

  SetUpInProcessBrowserTestFixture();

  // Should not use CommandLine to modify features. Please use ScopedFeatureList
  // instead.
  DCHECK(!command_line->HasSwitch(switches::kEnableFeatures));
  DCHECK(!command_line->HasSwitch(switches::kDisableFeatures));

  // At this point, copy features to the command line, since BrowserMain will
  // wipe out the current feature list.
  std::string enabled_features;
  std::string disabled_features;
  if (base::FeatureList::GetInstance()) {
    base::FeatureList::GetInstance()->GetFeatureOverrides(&enabled_features,
                                                          &disabled_features);
  }

  if (!enabled_features.empty()) {
    command_line->AppendSwitchASCII(switches::kEnableFeatures,
                                    enabled_features);
  }
  if (!disabled_features.empty()) {
    command_line->AppendSwitchASCII(switches::kDisableFeatures,
                                    disabled_features);
  }

  // Always disable the unsandbox GPU process for DX12 and Vulkan Info
  // collection to avoid interference. This GPU process is launched 15
  // seconds after chrome starts.
  command_line->AppendSwitch(
      switches::kDisableGpuProcessForDX12VulkanInfoCollection);

  // The current global field trial list contains any trials that were activated
  // prior to main browser startup. That global field trial list is about to be
  // destroyed below, and will be recreated during the browser_tests browser
  // process startup code. Pass the currently active trials to the subsequent
  // list via the command line.
  std::string field_trial_states;
  base::FieldTrialList::AllStatesToString(&field_trial_states, false);
  if (!field_trial_states.empty()) {
    // Please use ScopedFeatureList to modify feature and field trials at the
    // same time.
    DCHECK(!command_line->HasSwitch(switches::kForceFieldTrials));
    command_line->AppendSwitchASCII(switches::kForceFieldTrials,
                                    field_trial_states);
  }

  // Need to wipe feature list clean, since BrowserMain calls
  // FeatureList::SetInstance, which expects no instance to exist.
  base::FeatureList::ClearInstanceForTesting();

  auto created_main_parts_closure =
      std::make_unique<CreatedMainPartsClosure>(base::Bind(
          &BrowserTestBase::CreatedBrowserMainParts, base::Unretained(this)));

#if defined(OS_ANDROID)
  // For all other platforms, we call ContentMain for browser tests which goes
  // through the normal browser initialization paths. For Android, we must set
  // things up manually. A meager re-implementation of ContentMainRunnerImpl
  // follows.

  base::i18n::AllowMultipleInitializeCallsForTesting();
  base::i18n::InitializeICU();

#ifdef V8_USE_EXTERNAL_STARTUP_DATA
  gin::V8Initializer::LoadV8Snapshot();
  gin::V8Initializer::LoadV8Natives();
#endif

  ContentMainDelegate* delegate = GetContentMainDelegateForTesting();
  // The delegate should have been set by JNI_OnLoad for the test target.
  DCHECK(delegate);

  bool startup_error = delegate->BasicStartupComplete(/*exit_code=*/nullptr);
  DCHECK(!startup_error);

  InitializeMojo();

  {
    SetBrowserClientForTesting(delegate->CreateContentBrowserClient());
    if (command_line->HasSwitch(switches::kSingleProcess))
      SetRendererClientForTesting(delegate->CreateContentRendererClient());

    content::RegisterPathProvider();
    content::RegisterContentSchemes(false);
    ui::RegisterPathProvider();

    delegate->PreSandboxStartup();

    DCHECK(!field_trial_list_);
    if (delegate->ShouldCreateFeatureList()) {
      field_trial_list_ = SetUpFieldTrialsAndFeatureList();
      delegate->PostFieldTrialInitialization();
    }

    base::ThreadPoolInstance::Create("Browser");

    delegate->PreCreateMainMessageLoop();
    BrowserTaskExecutor::Create();
    delegate->PostEarlyInitialization(/*is_running_tests=*/true);

    StartBrowserThreadPool();
    BrowserTaskExecutor::PostFeatureListSetup();
    delegate->PostTaskSchedulerStart();
  }

  auto discardable_shared_memory_manager =
      std::make_unique<discardable_memory::DiscardableSharedMemoryManager>();
  auto service_manager_env = std::make_unique<ServiceManagerEnvironment>(
      BrowserTaskExecutor::CreateIOThread());
  std::unique_ptr<StartupDataImpl> startup_data =
      service_manager_env->CreateBrowserStartupData();

  // ContentMain would normally call RunProcess() on the delegate and fallback
  // to BrowserMain() if it did not run it (or equivalent) itself. On Android,
  // RunProcess() will return 0 so we don't have to fallback to BrowserMain().
  {
    // This loop will wait until Java completes async initializion and the test
    // is ready to run. We must allow nestable tasks so that tasks posted to the
    // UI thread run as well. The loop is created before RunProcess() so that
    // the StartupTaskRunner tasks will be nested inside this loop and able to
    // run.
    base::RunLoop loop{base::RunLoop::Type::kNestableTasksAllowed};

    auto ui_task = std::make_unique<base::Closure>(
        base::Bind(&BrowserTestBase::WaitUntilJavaIsReady,
                   base::Unretained(this), loop.QuitClosure()));

    // The MainFunctionParams must out-live all the startup tasks running.
    MainFunctionParams params(*command_line);
    params.ui_task = ui_task.release();
    params.created_main_parts_closure = created_main_parts_closure.release();
    params.startup_data = startup_data.get();
    // Passing "" as the process type to indicate the browser process.
    int exit_code = delegate->RunProcess("", params);
    DCHECK_EQ(exit_code, 0);

    // Waits for Java to finish initialization, then we can run the test.
    loop.Run();

    // The BrowserMainLoop startup tasks will call DisallowUnresponsiveTasks().
    // So when we run the ProxyRunTestOnMainThreadLoop() we no longer can block,
    // but tests should be allowed to. So we undo that blocking inside here.
    base::ScopedAllowUnresponsiveTasksForTesting allow_unresponsive;
    // Runs the test now that the Java setup is complete. This must be called
    // directly from the same call stack as RUN_ALL_TESTS(), it may not be
    // inside a posted task, or it would prevent NonNestable tasks from running
    // inside tests.
    ProxyRunTestOnMainThreadLoop();
  }

  {
    base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
    // Shutting these down will block the thread.
    service_manager_env.reset();
    discardable_shared_memory_manager.reset();
    spawned_test_server_.reset();
  }
  BrowserTaskExecutor::ResetForTesting();

  // Normally the BrowserMainLoop does this during shutdown but on Android we
  // don't go through shutdown, so this doesn't happen there. We do need it
  // for the test harness to be able to delete temp dirs.
  base::ThreadRestrictions::SetIOAllowed(true);
#else
  auto ui_task = std::make_unique<base::Closure>(base::Bind(
      &BrowserTestBase::ProxyRunTestOnMainThreadLoop, base::Unretained(this)));
  GetContentMainParams()->ui_task = ui_task.release();
  GetContentMainParams()->created_main_parts_closure =
      created_main_parts_closure.release();
  EXPECT_EQ(expected_exit_code_, ContentMain(*GetContentMainParams()));
#endif
  TearDownInProcessBrowserTestFixture();
}

void BrowserTestBase::TearDown() {
#if defined(USE_AURA) || defined(OS_MACOSX)
  ui::test::EventGeneratorDelegate::SetFactoryFunction(
      ui::test::EventGeneratorDelegate::FactoryFunction());
#endif
}

bool BrowserTestBase::AllowFileAccessFromFiles() {
  return true;
}

void BrowserTestBase::SimulateNetworkServiceCrash() {
  CHECK(base::FeatureList::IsEnabled(network::features::kNetworkService));
  CHECK(!IsInProcessNetworkService())
      << "Can't crash the network service if it's running in-process!";
  network::mojom::NetworkServiceTestPtr network_service_test;
  GetSystemConnector()->BindInterface(mojom::kNetworkServiceName,
                                      &network_service_test);

  base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
  network_service_test.set_connection_error_handler(run_loop.QuitClosure());

  network_service_test->SimulateCrash();
  run_loop.Run();

  // Make sure the cached NetworkServicePtr receives error notification.
  FlushNetworkServiceInstanceForTesting();

  // Need to re-initialize the network process.
  initialized_network_process_ = false;
  InitializeNetworkProcess();
}

#if defined(OS_ANDROID)
void BrowserTestBase::WaitUntilJavaIsReady(base::OnceClosure quit_closure) {
  if (testing::android::JavaAsyncStartupTasksCompleteForBrowserTests()) {
    std::move(quit_closure).Run();
    return;
  }

  base::PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&BrowserTestBase::WaitUntilJavaIsReady,
                     base::Unretained(this), std::move(quit_closure)),
      base::TimeDelta::FromMilliseconds(100));
  return;
}
#endif

namespace {

std::string GetDefaultTraceFilaneme() {
  std::string test_suite_name = ::testing::UnitTest::GetInstance()
                                    ->current_test_info()
                                    ->test_suite_name();
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  // Add random number to the trace file to distinguish traces from different
  // test runs.
  // We don't use timestamp here to avoid collisions with parallel runs of the
  // same test.
  std::string random_seed = base::NumberToString(base::RandInt(1e7, 1e8 - 1));
  std::string status = ::testing::UnitTest::GetInstance()
                               ->current_test_info()
                               ->result()
                               ->Passed()
                           ? "OK"
                           : "FAIL";
  return "trace_test_" + test_suite_name + "_" + test_name + "_" + random_seed +
         "_" + status + ".json";
}

}  // namespace

void BrowserTestBase::ProxyRunTestOnMainThreadLoop() {
  // Install a RunLoop timeout if none is present but do not override tests that
  // set a ScopedRunTimeoutForTest from their fixture's constructor (which
  // happens as part of setting up the test factory in gtest while
  // ProxyRunTestOnMainThreadLoop() happens later as part of SetUp()).
  base::Optional<base::RunLoop::ScopedRunTimeoutForTest> scoped_run_timeout;
  if (!base::RunLoop::ScopedRunTimeoutForTest::Current()) {
    // TODO(https://crbug.com/918724): determine whether the timeout can be
    // reduced from action_max_timeout() to action_timeout().
    scoped_run_timeout.emplace(TestTimeouts::action_max_timeout(),
                               base::MakeExpectedNotRunClosure(
                                   FROM_HERE, "RunLoop::Run() timed out."));
  }

#if defined(OS_POSIX)
  g_browser_process_pid = base::GetCurrentProcId();
  signal(SIGSEGV, DumpStackTraceSignalHandler);

  if (handle_sigterm_)
    signal(SIGTERM, DumpStackTraceSignalHandler);
#endif  // defined(OS_POSIX)

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableTracing)) {
    base::trace_event::TraceConfig trace_config(
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            switches::kEnableTracing),
        base::trace_event::RECORD_CONTINUOUSLY);
    TracingController::GetInstance()->StartTracing(
        trace_config,
        TracingController::StartTracingDoneCallback());
  }

  {
    // This can be called from a posted task. Allow nested tasks here, because
    // otherwise the test body will have to do it in order to use RunLoop for
    // waiting.
    base::MessageLoopCurrent::ScopedNestableTaskAllower allow;

#if !defined(OS_ANDROID)
    // Fail the test if a renderer crashes while the test is running.
    //
    // This cannot be enabled on Android, because of renderer kills triggered
    // aggressively by the OS itself.
    no_renderer_crashes_assertion_ =
        std::make_unique<NoRendererCrashesAssertion>();
#endif

    PreRunTestOnMainThread();
    std::unique_ptr<InitialNavigationObserver> initial_navigation_observer;
    if (initial_web_contents_ &&
        base::FeatureList::IsEnabled(network::features::kNetworkService)) {
      // Some tests may add host_resolver() rules in their SetUpOnMainThread
      // method and navigate inside of it. This is a best effort to catch that
      // and sync the host_resolver() rules to the network process in that case,
      // to avoid navigations silently failing. This won't catch all cases, i.e.
      // if the test creates a new window or tab and navigates that.
      initial_navigation_observer = std::make_unique<InitialNavigationObserver>(
          initial_web_contents_,
          base::BindOnce(&BrowserTestBase::InitializeNetworkProcess,
                         base::Unretained(this)));
    }
    initial_web_contents_ = nullptr;
    SetUpOnMainThread();
    initial_navigation_observer.reset();

    // Tests would have added their host_resolver() rules by now, so copy them
    // to the network process if it's in use.
    InitializeNetworkProcess();

    bool old_io_allowed_value = false;
    old_io_allowed_value = base::ThreadRestrictions::SetIOAllowed(false);
    RunTestOnMainThread();
    base::ThreadRestrictions::SetIOAllowed(old_io_allowed_value);
    TearDownOnMainThread();
  }

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableTracing)) {
    base::FilePath trace_file =
        base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
            switches::kEnableTracingOutput);
    // If there was no file specified, put a hardcoded one in the current
    // working directory.
    if (trace_file.empty())
      trace_file = base::FilePath().AppendASCII(GetDefaultTraceFilaneme());

    // Wait for tracing to collect results from the renderers.
    base::RunLoop run_loop;
    TracingController::GetInstance()->StopTracing(
        TracingControllerImpl::CreateFileEndpoint(
            trace_file, base::Bind(&TraceStopTracingComplete,
                                   run_loop.QuitClosure(), trace_file)));
    run_loop.Run();
  }

  PostRunTestOnMainThread();
}

void BrowserTestBase::CreateTestServer(const base::FilePath& test_server_base) {
  CHECK(!spawned_test_server_.get());
  spawned_test_server_ = std::make_unique<net::SpawnedTestServer>(
      net::SpawnedTestServer::TYPE_HTTP, test_server_base);
  embedded_test_server()->AddDefaultHandlers(test_server_base);
}

void BrowserTestBase::PostTaskToInProcessRendererAndWait(
    base::OnceClosure task) {
  CHECK(base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kSingleProcess));

  scoped_refptr<base::SingleThreadTaskRunner> renderer_task_runner =
      RenderProcessHostImpl::GetInProcessRendererThreadTaskRunnerForTesting();
  CHECK(renderer_task_runner);

  base::RunLoop run_loop;
  renderer_task_runner->PostTask(
      FROM_HERE, base::BindOnce(&RunTaskOnRendererThread, std::move(task),
                                run_loop.QuitClosure()));
  run_loop.Run();
}

void BrowserTestBase::EnablePixelOutput() { enable_pixel_output_ = true; }

void BrowserTestBase::UseSoftwareCompositing() {
  use_software_compositing_ = true;
}

bool BrowserTestBase::UsingSoftwareGL() const {
  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  return cmd->GetSwitchValueASCII(switches::kUseGL) ==
         gl::GetGLImplementationName(gl::GetSoftwareGLImplementation());
}

void BrowserTestBase::SetInitialWebContents(WebContents* web_contents) {
  DCHECK(!initial_web_contents_);
  initial_web_contents_ = web_contents;
}

void BrowserTestBase::InitializeNetworkProcess() {
  if (initialized_network_process_)
    return;

  initialized_network_process_ = true;
  host_resolver()->DisableModifications();

  // Send the host resolver rules to the network service if it's in use. No need
  // to do this if it's running in the browser process though.
  if (!IsOutOfProcessNetworkService())
    return;

  net::RuleBasedHostResolverProc::RuleList rules = host_resolver()->GetRules();
  std::vector<network::mojom::RulePtr> mojo_rules;
  for (const auto& rule : rules) {
    // For now, this covers all the rules used in content's tests.
    // TODO(jam: expand this when we try to make browser_tests and
    // components_browsertests work.
    if (rule.resolver_type ==
        net::RuleBasedHostResolverProc::Rule::kResolverTypeFail) {
      // The host "wpad" is added automatically in TestHostResolver, so we don't
      // need to send it to NetworkServiceTest.
      if (rule.host_pattern != "wpad") {
        network::mojom::RulePtr mojo_rule = network::mojom::Rule::New();
        mojo_rule->resolver_type =
            network::mojom::ResolverType::kResolverTypeFail;
        mojo_rule->host_pattern = rule.host_pattern;
        mojo_rules.push_back(std::move(mojo_rule));
      }
      continue;
    }

    if ((rule.resolver_type !=
             net::RuleBasedHostResolverProc::Rule::kResolverTypeSystem &&
         rule.resolver_type !=
             net::RuleBasedHostResolverProc::Rule::kResolverTypeIPLiteral) ||
        rule.address_family != net::AddressFamily::ADDRESS_FAMILY_UNSPECIFIED ||
        !!rule.latency_ms)
      continue;
    network::mojom::RulePtr mojo_rule = network::mojom::Rule::New();
    if (rule.resolver_type ==
        net::RuleBasedHostResolverProc::Rule::kResolverTypeSystem) {
      mojo_rule->resolver_type =
          rule.replacement.empty()
              ? network::mojom::ResolverType::kResolverTypeDirectLookup
              : network::mojom::ResolverType::kResolverTypeSystem;
    } else {
      mojo_rule->resolver_type =
          network::mojom::ResolverType::kResolverTypeIPLiteral;
    }
    mojo_rule->host_pattern = rule.host_pattern;
    mojo_rule->replacement = rule.replacement;
    mojo_rules.push_back(std::move(mojo_rule));
  }

  if (mojo_rules.empty())
    return;

  network::mojom::NetworkServiceTestPtr network_service_test;
  GetSystemConnector()->BindInterface(mojom::kNetworkServiceName,
                                      &network_service_test);

  // Send the DNS rules to network service process. Android needs the RunLoop
  // to dispatch a Java callback that makes network process to enter native
  // code.
  base::RunLoop loop{base::RunLoop::Type::kNestableTasksAllowed};
  network_service_test->AddRules(std::move(mojo_rules), loop.QuitClosure());
  loop.Run();
}

}  // namespace content
