// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/render_thread_impl.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/debug/leak_annotations.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/memory/discardable_memory.h"
#include "base/metrics/field_trial.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/post_task.h"
#include "base/task/thread_pool/thread_pool.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/thread_task_runner_handle.h"
#include "content/app/mojo/mojo_init.h"
#include "content/common/in_process_child_thread_params.h"
#include "content/common/service_manager/child_connection.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/system_connector.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/service_names.mojom.h"
#include "content/public/renderer/content_renderer_client.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_content_client_initializer.h"
#include "content/public/test/test_launcher.h"
#include "content/public/test/test_service_manager_context.h"
#include "content/renderer/render_process_impl.h"
#include "content/test/mock_render_process.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/gpu_memory_buffer_manager.h"
#include "gpu/config/gpu_switches.h"
#include "ipc/ipc.mojom.h"
#include "ipc/ipc_channel_mojo.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/system/invitation.h"
#include "services/service_manager/public/cpp/constants.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/scheduler/test/renderer_scheduler_test_support.h"
#include "third_party/blink/public/platform/scheduler/test/web_mock_thread_scheduler.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "ui/base/ui_base_switches.h"
#include "ui/gfx/buffer_format_util.h"
#include "ui/gfx/switches.h"

#if defined(USE_SYSTEM_PROPRIETARY_CODECS)
// Need to disable IPC Audio for these tests, or the tests will hang
#include "platform_media/renderer/decoders/ipc_audio_decoder.h"
#endif  // defined(USE_SYSTEM_PROPRIETARY_CODECS)

// IPC messages for testing ----------------------------------------------------

// TODO(mdempsky): Fix properly by moving into a separate
// browsertest_message_generator.cc file.
#undef IPC_IPC_MESSAGE_MACROS_H_
#undef IPC_MESSAGE_EXTRA
#define IPC_MESSAGE_IMPL
#include "ipc/ipc_message_macros.h"
#include "ipc/ipc_message_templates_impl.h"

#undef IPC_MESSAGE_START
#define IPC_MESSAGE_START TestMsgStart
IPC_MESSAGE_CONTROL0(TestMsg_QuitRunLoop)

// -----------------------------------------------------------------------------

// These tests leak memory, this macro disables the test when under the
// LeakSanitizer.
#ifdef LEAK_SANITIZER
#define WILL_LEAK(NAME) DISABLED_##NAME
#else
#define WILL_LEAK(NAME) NAME
#endif

namespace content {

// FIXME: It would be great if there was a reusable mock SingleThreadTaskRunner
class TestTaskCounter : public base::SingleThreadTaskRunner {
 public:
  TestTaskCounter() : count_(0) {}

  // SingleThreadTaskRunner implementation.
  bool PostDelayedTask(const base::Location&,
                       base::OnceClosure,
                       base::TimeDelta) override {
    base::AutoLock auto_lock(lock_);
    count_++;
    return true;
  }

  bool PostNonNestableDelayedTask(const base::Location&,
                                  base::OnceClosure,
                                  base::TimeDelta) override {
    base::AutoLock auto_lock(lock_);
    count_++;
    return true;
  }

  bool RunsTasksInCurrentSequence() const override { return true; }

  int NumTasksPosted() const {
    base::AutoLock auto_lock(lock_);
    return count_;
  }

 private:
  ~TestTaskCounter() override {}

  mutable base::Lock lock_;
  int count_;
};

class QuitOnTestMsgFilter : public IPC::MessageFilter {
 public:
  explicit QuitOnTestMsgFilter(base::OnceClosure quit_closure)
      : origin_task_runner_(
            blink::scheduler::GetSequencedTaskRunnerForTesting()),
        quit_closure_(std::move(quit_closure)) {}

  // IPC::MessageFilter overrides:
  bool OnMessageReceived(const IPC::Message& message) override {
    origin_task_runner_->PostTask(FROM_HERE, std::move(quit_closure_));
    return true;
  }

  bool GetSupportedMessageClasses(
      std::vector<uint32_t>* supported_message_classes) const override {
    supported_message_classes->push_back(TestMsgStart);
    return true;
  }

 private:
  ~QuitOnTestMsgFilter() override {}

  scoped_refptr<base::SequencedTaskRunner> origin_task_runner_;
  base::OnceClosure quit_closure_;
};

class RenderThreadImplBrowserTest : public testing::Test {
 public:
  RenderThreadImplBrowserTest() : field_trial_list_(nullptr) {}

  void SetUp() override {
    content_renderer_client_.reset(new ContentRendererClient());
    SetRendererClientForTesting(content_renderer_client_.get());

    browser_threads_.reset(
        new TestBrowserThreadBundle(TestBrowserThreadBundle::REAL_IO_THREAD));
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner =
        base::CreateSingleThreadTaskRunnerWithTraits({BrowserThread::IO});

    InitializeMojo();
    mojo_ipc_support_.reset(new mojo::core::ScopedIPCSupport(
        io_task_runner, mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST));
    shell_context_.reset(new TestServiceManagerContext);
    mojo::OutgoingInvitation invitation;
    child_connection_ = std::make_unique<ChildConnection>(
        service_manager::Identity(mojom::kRendererServiceName,
                                  service_manager::kSystemInstanceGroup,
                                  base::Token{}, base::Token::CreateRandom()),
        &invitation, GetSystemConnector(), io_task_runner);

    mojo::MessagePipe pipe;
    child_connection_->BindInterface(IPC::mojom::ChannelBootstrap::Name_,
                                     std::move(pipe.handle1));

    channel_ = IPC::ChannelProxy::Create(
        IPC::ChannelMojo::CreateServerFactory(
            std::move(pipe.handle0), io_task_runner,
            blink::scheduler::GetSingleThreadTaskRunnerForTesting()),
        nullptr, io_task_runner,
        blink::scheduler::GetSingleThreadTaskRunnerForTesting());

    mock_process_.reset(new MockRenderProcess);
    test_task_counter_ = base::MakeRefCounted<TestTaskCounter>();

    // RenderThreadImpl expects the browser to pass these flags.
    base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
    base::CommandLine::StringVector old_argv = cmd->argv();

    cmd->AppendSwitchASCII(switches::kLang, "en-US");

    cmd->AppendSwitchASCII(switches::kNumRasterThreads, "1");

    // To avoid creating a GPU channel to query if
    // accelerated_video_decode is blacklisted on older Android system
    // in RenderThreadImpl::Init().
    cmd->AppendSwitch(switches::kIgnoreGpuBlacklist);

    auto main_thread_scheduler =
        blink::scheduler::CreateMockWebMainThreadSchedulerForTests();
    scoped_refptr<base::SingleThreadTaskRunner> test_task_counter(
        test_task_counter_.get());

    base::FieldTrialList::CreateTrialsFromCommandLine(
        *cmd, switches::kFieldTrialHandle, -1);
    thread_ = new RenderThreadImpl(
        InProcessChildThreadParams(io_task_runner, &invitation,
                                   child_connection_->service_token()),
        std::move(main_thread_scheduler));
    cmd->InitFromArgv(old_argv);

    run_loop_ = std::make_unique<base::RunLoop>();
    test_msg_filter_ = base::MakeRefCounted<QuitOnTestMsgFilter>(
        run_loop_->QuitWhenIdleClosure());
    thread_->AddFilter(test_msg_filter_.get());

    main_thread_scheduler_ =
        static_cast<blink::scheduler::WebMockThreadScheduler*>(
            thread_->GetWebMainThreadScheduler());
  }

  void TearDown() override {
    if (base::CommandLine::ForCurrentProcess()->HasSwitch(
            kSingleProcessTestsFlag)) {
      // In a single-process mode, we need to avoid destructing mock_process_
      // because it will call _exit(0) and kill the process before the browser
      // side is ready to exit.
      ANNOTATE_LEAKING_OBJECT_PTR(mock_process_.get());
      mock_process_.release();
    }
  }

 protected:
  IPC::Sender* sender() { return channel_.get(); }

  void SetProcessState(mojom::RenderProcessState process_state) {
    mojom::Renderer* renderer_interface = thread_;
    renderer_interface->SetProcessState(process_state);
  }

  bool RendererIsBackgrounded() { return thread_->RendererIsBackgrounded(); }
  bool RendererIsHidden() { return thread_->RendererIsHidden(); }

  scoped_refptr<TestTaskCounter> test_task_counter_;
  TestContentClientInitializer content_client_initializer_;
  std::unique_ptr<ContentRendererClient> content_renderer_client_;

  std::unique_ptr<TestBrowserThreadBundle> browser_threads_;
  std::unique_ptr<TestServiceManagerContext> shell_context_;
  std::unique_ptr<ChildConnection> child_connection_;
  std::unique_ptr<IPC::ChannelProxy> channel_;
  std::unique_ptr<mojo::core::ScopedIPCSupport> mojo_ipc_support_;

  std::unique_ptr<MockRenderProcess> mock_process_;
  scoped_refptr<QuitOnTestMsgFilter> test_msg_filter_;
#if defined(USE_SYSTEM_PROPRIETARY_CODECS)
  // Need to disable IPC Audio for these tests, or the test will hang
  media::IPCFactory::ScopedDisableForTesting ipc_audio_decoder_disabler_;
#endif

  blink::scheduler::WebMockThreadScheduler* main_thread_scheduler_;

  // RenderThreadImpl doesn't currently support a proper shutdown sequence
  // and it's okay when we're running in multi-process mode because renderers
  // get killed by the OS. Memory leaks aren't nice but it's test-only.
  RenderThreadImpl* thread_;

  base::FieldTrialList field_trial_list_;

  std::unique_ptr<base::RunLoop> run_loop_;

 private:
  DISALLOW_COPY_AND_ASSIGN(RenderThreadImplBrowserTest);
};

// Check that InputHandlerManager outlives compositor thread because it uses
// raw pointers to post tasks.
// Disabled under LeakSanitizer due to memory leaks. http://crbug.com/348994
// Disabled on Windows due to flakiness: http://crbug.com/728034.
#if defined(OS_WIN)
#define MAYBE_InputHandlerManagerDestroyedAfterCompositorThread \
  DISABLED_InputHandlerManagerDestroyedAfterCompositorThread
#else
#define MAYBE_InputHandlerManagerDestroyedAfterCompositorThread \
  InputHandlerManagerDestroyedAfterCompositorThread
#endif

// Disabled under LeakSanitizer due to memory leaks.
TEST_F(RenderThreadImplBrowserTest,
       WILL_LEAK(NonResourceDispatchIPCTasksDontGoThroughScheduler)) {
  // This seems to deflake the test on Android.
  browser_threads_->RunIOThreadUntilIdle();

  // NOTE other than not being a resource message, the actual message is
  // unimportant.
  sender()->Send(new TestMsg_QuitRunLoop());

  run_loop_->Run();

  EXPECT_EQ(0, test_task_counter_->NumTasksPosted());
}

TEST_F(RenderThreadImplBrowserTest, RendererIsBackgrounded) {
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  EXPECT_TRUE(RendererIsBackgrounded());

  SetProcessState(mojom::RenderProcessState::kHidden);
  EXPECT_FALSE(RendererIsBackgrounded());

  SetProcessState(mojom::RenderProcessState::kVisible);
  EXPECT_FALSE(RendererIsBackgrounded());
}

TEST_F(RenderThreadImplBrowserTest, RendererIsHidden) {
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  EXPECT_TRUE(RendererIsHidden());

  SetProcessState(mojom::RenderProcessState::kHidden);
  EXPECT_TRUE(RendererIsHidden());

  SetProcessState(mojom::RenderProcessState::kVisible);
  EXPECT_FALSE(RendererIsHidden());
}

TEST_F(RenderThreadImplBrowserTest, RendererStateTransitionVisible) {
  // Going from an unknown to a visible state should mark the renderer as
  // foregrounded and visible.
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true)).Times(0);
  SetProcessState(mojom::RenderProcessState::kVisible);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  // Going from a hidden to a visible state should mark the renderer as visible.
  // A hidden renderer is already foregrounded.
  SetProcessState(mojom::RenderProcessState::kHidden);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true)).Times(0);
  SetProcessState(mojom::RenderProcessState::kVisible);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  // Going from a backgrounded to a visible state should mark the renderer as
  // foregrounded and visible.
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true)).Times(0);
  SetProcessState(mojom::RenderProcessState::kVisible);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  testing::Mock::AllowLeak(main_thread_scheduler_);
}

TEST_F(RenderThreadImplBrowserTest, RendererStateTransitionHidden) {
  // Going from an unknown to a visible state should mark the renderer as
  // foregrounded and hidden.
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false)).Times(0);
  SetProcessState(mojom::RenderProcessState::kHidden);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  // Going from a visible to a hidden state should mark the renderer as hidden.
  // A visible renderer is already foregrounded.
  SetProcessState(mojom::RenderProcessState::kVisible);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true)).Times(0);
  SetProcessState(mojom::RenderProcessState::kHidden);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  // Going from a backgrounded to a hidden state should mark the renderer as
  // foregrounded and hidden.
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false)).Times(0);
  SetProcessState(mojom::RenderProcessState::kHidden);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  testing::Mock::AllowLeak(main_thread_scheduler_);
}

TEST_F(RenderThreadImplBrowserTest, RendererStateTransitionBackgrounded) {
  // Going from an unknown to a backgrounded state should mark the renderer as
  // hidden and backgrounded.
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false)).Times(0);
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  // Going from a visible to a backgrounded state should mark the renderer as
  // hidden and backgrounded.
  SetProcessState(mojom::RenderProcessState::kVisible);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false)).Times(0);
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  // Going from a hidden state to a backgrounded state should mark the renderer
  // backgrounded. A hidden renderer is already marked as hidden.
  SetProcessState(mojom::RenderProcessState::kHidden);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(true));
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(true)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererHidden(false)).Times(0);
  EXPECT_CALL(*main_thread_scheduler_, SetRendererBackgrounded(false)).Times(0);
  SetProcessState(mojom::RenderProcessState::kBackgrounded);
  testing::Mock::VerifyAndClear(main_thread_scheduler_);

  testing::Mock::AllowLeak(main_thread_scheduler_);
}

enum NativeBufferFlag { kDisableNativeBuffers, kEnableNativeBuffers };

class RenderThreadImplGpuMemoryBufferBrowserTest
    : public ContentBrowserTest,
      public testing::WithParamInterface<
          ::testing::tuple<NativeBufferFlag, gfx::BufferFormat>> {
 public:
  RenderThreadImplGpuMemoryBufferBrowserTest() {}
  ~RenderThreadImplGpuMemoryBufferBrowserTest() override {}

  gpu::GpuMemoryBufferManager* memory_buffer_manager() {
    return memory_buffer_manager_;
  }

 private:
  void SetUpOnRenderThread() {
    memory_buffer_manager_ =
        RenderThreadImpl::current()->GetGpuMemoryBufferManager();
  }

  // Overridden from BrowserTestBase:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitch(switches::kSingleProcess);
    NativeBufferFlag native_buffer_flag = ::testing::get<0>(GetParam());
    if (native_buffer_flag == kEnableNativeBuffers) {
      command_line->AppendSwitch(switches::kEnableNativeGpuMemoryBuffers);
    }
  }

  void SetUpOnMainThread() override {
    NavigateToURL(shell(), GURL(url::kAboutBlankURL));
    PostTaskToInProcessRendererAndWait(base::BindOnce(
        &RenderThreadImplGpuMemoryBufferBrowserTest::SetUpOnRenderThread,
        base::Unretained(this)));
  }

  gpu::GpuMemoryBufferManager* memory_buffer_manager_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(RenderThreadImplGpuMemoryBufferBrowserTest);
};

// https://crbug.com/652531
IN_PROC_BROWSER_TEST_P(RenderThreadImplGpuMemoryBufferBrowserTest,
                       DISABLED_Map) {
  gfx::BufferFormat format = ::testing::get<1>(GetParam());
  gfx::Size buffer_size(4, 4);

  std::unique_ptr<gfx::GpuMemoryBuffer> buffer =
      memory_buffer_manager()->CreateGpuMemoryBuffer(
          buffer_size, format, gfx::BufferUsage::GPU_READ_CPU_READ_WRITE,
          gpu::kNullSurfaceHandle);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(format, buffer->GetFormat());

  // Map buffer planes.
  ASSERT_TRUE(buffer->Map());

  // Write to buffer and check result.
  size_t num_planes = gfx::NumberOfPlanesForLinearBufferFormat(format);
  for (size_t plane = 0; plane < num_planes; ++plane) {
    ASSERT_TRUE(buffer->memory(plane));
    ASSERT_TRUE(buffer->stride(plane));
    size_t row_size_in_bytes =
        gfx::RowSizeForBufferFormat(buffer_size.width(), format, plane);
    EXPECT_GT(row_size_in_bytes, 0u);

    std::unique_ptr<char[]> data(new char[row_size_in_bytes]);
    memset(data.get(), 0x2a + plane, row_size_in_bytes);
    size_t height = buffer_size.height() /
                    gfx::SubsamplingFactorForBufferFormat(format, plane);
    for (size_t y = 0; y < height; ++y) {
      // Copy |data| to row |y| of |plane| and verify result.
      memcpy(
          static_cast<char*>(buffer->memory(plane)) + y * buffer->stride(plane),
          data.get(), row_size_in_bytes);
      EXPECT_EQ(0, memcmp(static_cast<char*>(buffer->memory(plane)) +
                              y * buffer->stride(plane),
                          data.get(), row_size_in_bytes));
    }
  }

  buffer->Unmap();
}

INSTANTIATE_TEST_SUITE_P(
    RenderThreadImplGpuMemoryBufferBrowserTests,
    RenderThreadImplGpuMemoryBufferBrowserTest,
    ::testing::Combine(::testing::Values(kDisableNativeBuffers,
                                         kEnableNativeBuffers),
                       // These formats are guaranteed to work on all platforms.
                       ::testing::Values(gfx::BufferFormat::R_8,
                                         gfx::BufferFormat::BGR_565,
                                         gfx::BufferFormat::RGBA_4444,
                                         gfx::BufferFormat::RGBA_8888,
                                         gfx::BufferFormat::BGRA_8888,
                                         gfx::BufferFormat::YVU_420)));

}  // namespace content
