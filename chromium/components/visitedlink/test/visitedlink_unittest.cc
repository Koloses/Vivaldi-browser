// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/process/process_handle.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/timer/mock_timer.h"
#include "components/visitedlink/browser/visitedlink_delegate.h"
#include "components/visitedlink/browser/visitedlink_event_listener.h"
#include "components/visitedlink/browser/visitedlink_master.h"
#include "components/visitedlink/common/visitedlink.mojom.h"
#include "components/visitedlink/renderer/visitedlink_slave.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_types.h"
#include "content/public/test/mock_render_process_host.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/test_utils.h"
#include "mojo/public/cpp/bindings/binding_set.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

using content::BrowserThread;
using content::MockRenderProcessHost;
using content::RenderViewHostTester;

namespace content {
class SiteInstance;
}

namespace visitedlink {

namespace {

typedef std::vector<GURL> URLs;

// a nice long URL that we can append numbers to to get new URLs
const char g_test_prefix[] =
  "http://www.google.com/products/foo/index.html?id=45028640526508376&seq=";
const int g_test_count = 1000;

// Returns a test URL for index |i|
GURL TestURL(int i) {
  return GURL(base::StringPrintf("%s%d", g_test_prefix, i));
}

std::vector<VisitedLinkSlave*> g_slaves;

class TestVisitedLinkDelegate : public VisitedLinkDelegate {
 public:
  void RebuildTable(const scoped_refptr<URLEnumerator>& enumerator) override;

  void AddURLForRebuild(const GURL& url);

 private:
  URLs rebuild_urls_;
};

void TestVisitedLinkDelegate::RebuildTable(
    const scoped_refptr<URLEnumerator>& enumerator) {
  for (URLs::const_iterator itr = rebuild_urls_.begin();
       itr != rebuild_urls_.end();
       ++itr)
    enumerator->OnURL(*itr);
  enumerator->OnComplete(true);
}

void TestVisitedLinkDelegate::AddURLForRebuild(const GURL& url) {
  rebuild_urls_.push_back(url);
}

class TestURLIterator : public VisitedLinkMaster::URLIterator {
 public:
  explicit TestURLIterator(const URLs& urls);

  const GURL& NextURL() override;
  bool HasNextURL() const override;

 private:
  URLs::const_iterator iterator_;
  URLs::const_iterator end_;
};

TestURLIterator::TestURLIterator(const URLs& urls)
    : iterator_(urls.begin()),
      end_(urls.end()) {
}

const GURL& TestURLIterator::NextURL() {
  return *(iterator_++);
}

bool TestURLIterator::HasNextURL() const {
  return iterator_ != end_;
}

}  // namespace

class TrackingVisitedLinkEventListener : public VisitedLinkMaster::Listener {
 public:
  TrackingVisitedLinkEventListener()
      : reset_count_(0),
        completely_reset_count_(0),
        add_count_(0) {}

  void NewTable(base::ReadOnlySharedMemoryRegion* table_region) override {
    if (table_region->IsValid()) {
      for (std::vector<VisitedLinkSlave>::size_type i = 0;
           i < g_slaves.size(); i++) {
        g_slaves[i]->UpdateVisitedLinks(table_region->Duplicate());
      }
    }
  }
  void Add(VisitedLinkCommon::Fingerprint) override { add_count_++; }
  void Reset(bool invalidate_hashes) override {
    if (invalidate_hashes)
      completely_reset_count_++;
    else
      reset_count_++;
  }

  void SetUp() {
    reset_count_ = 0;
    add_count_ = 0;
  }

  int reset_count() const { return reset_count_; }
  int completely_reset_count() { return completely_reset_count_; }
  int add_count() const { return add_count_; }

 private:
  int reset_count_;
  int completely_reset_count_;
  int add_count_;
};

class VisitedLinkTest : public testing::Test {
 protected:
  // Initializes the visited link objects. Pass in the size that you want a
  // freshly created table to be. 0 means use the default.
  //
  // |suppress_rebuild| is set when we're not testing rebuilding, see
  // the VisitedLinkMaster constructor.
  //
  // |wait_for_io_complete| wait for result of async loading.
  bool InitVisited(int initial_size,
                   bool suppress_rebuild,
                   bool wait_for_io_complete) {
    // Initialize the visited link system.
    master_.reset(new VisitedLinkMaster(new TrackingVisitedLinkEventListener(),
                                        &delegate_,
                                        true,
                                        suppress_rebuild, visited_file_,
                                        initial_size));
    bool result = master_->Init();
    if (result && wait_for_io_complete) {
      // Wait for all pending file I/O to be completed.
      content::RunAllTasksUntilIdle();
    }
    return result;
  }

  // May be called multiple times (some tests will do this to clear things,
  // and TearDown will do this to make sure eveything is shiny before quitting.
  void ClearDB() {
    master_.reset(nullptr);

    // Wait for all pending file I/O to be completed.
    content::RunAllTasksUntilIdle();
  }

  // Loads the database from disk and makes sure that the same URLs are present
  // as were generated by TestIO_Create(). This also checks the URLs with a
  // slave to make sure it reads the data properly.
  void Reload() {
    // Clean up after our caller, who may have left the database open.
    ClearDB();

    ASSERT_TRUE(InitVisited(0, true, true));

    master_->DebugValidate();

    // check that the table has the proper number of entries
    int used_count = master_->GetUsedCount();
    ASSERT_EQ(used_count, g_test_count);

    // Create a slave database.
    VisitedLinkSlave slave;
    slave.UpdateVisitedLinks(master_->mapped_table_memory().region.Duplicate());
    g_slaves.push_back(&slave);

    bool found;
    for (int i = 0; i < g_test_count; i++) {
      GURL cur = TestURL(i);
      found = master_->IsVisited(cur);
      EXPECT_TRUE(found) << "URL " << i << "not found in master.";

      found = slave.IsVisited(cur);
      EXPECT_TRUE(found) << "URL " << i << "not found in slave.";
    }

    // test some random URL so we know that it returns false sometimes too
    found = master_->IsVisited(GURL("http://unfound.site/"));
    ASSERT_FALSE(found);
    found = slave.IsVisited(GURL("http://unfound.site/"));
    ASSERT_FALSE(found);

    master_->DebugValidate();

    g_slaves.clear();
  }

  // testing::Test
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());

    history_dir_ = temp_dir_.GetPath().AppendASCII("VisitedLinkTest");
    ASSERT_TRUE(base::CreateDirectory(history_dir_));

    visited_file_ = history_dir_.Append(FILE_PATH_LITERAL("VisitedLinks"));
  }

  void TearDown() override { ClearDB(); }

  base::ScopedTempDir temp_dir_;

  // Filenames for the services;
  base::FilePath history_dir_;
  base::FilePath visited_file_;

  std::unique_ptr<VisitedLinkMaster> master_;
  TestVisitedLinkDelegate delegate_;
  content::TestBrowserThreadBundle thread_bundle_;
};

// This test creates and reads some databases to make sure the data is
// preserved throughout those operations.
TEST_F(VisitedLinkTest, DatabaseIO) {
  ASSERT_TRUE(InitVisited(0, true, true));

  for (int i = 0; i < g_test_count; i++)
    master_->AddURL(TestURL(i));

  // Test that the database was written properly
  Reload();
}

// Checks that we can delete things properly when there are collisions.
TEST_F(VisitedLinkTest, Delete) {
  static const int32_t kInitialSize = 17;
  ASSERT_TRUE(InitVisited(kInitialSize, true, true));

  // Add a cluster from 14-17 wrapping around to 0. These will all hash to the
  // same value.
  const VisitedLinkCommon::Fingerprint kFingerprint0 = kInitialSize * 0 + 14;
  const VisitedLinkCommon::Fingerprint kFingerprint1 = kInitialSize * 1 + 14;
  const VisitedLinkCommon::Fingerprint kFingerprint2 = kInitialSize * 2 + 14;
  const VisitedLinkCommon::Fingerprint kFingerprint3 = kInitialSize * 3 + 14;
  const VisitedLinkCommon::Fingerprint kFingerprint4 = kInitialSize * 4 + 14;
  master_->AddFingerprint(kFingerprint0, false);  // @14
  master_->AddFingerprint(kFingerprint1, false);  // @15
  master_->AddFingerprint(kFingerprint2, false);  // @16
  master_->AddFingerprint(kFingerprint3, false);  // @0
  master_->AddFingerprint(kFingerprint4, false);  // @1

  // Deleting 14 should move the next value up one slot (we do not specify an
  // order).
  EXPECT_EQ(kFingerprint3, master_->hash_table_[0]);
  master_->DeleteFingerprint(kFingerprint3, false);
  VisitedLinkCommon::Fingerprint zero_fingerprint = 0;
  EXPECT_EQ(zero_fingerprint, master_->hash_table_[1]);
  EXPECT_NE(zero_fingerprint, master_->hash_table_[0]);

  // Deleting the other four should leave the table empty.
  master_->DeleteFingerprint(kFingerprint0, false);
  master_->DeleteFingerprint(kFingerprint1, false);
  master_->DeleteFingerprint(kFingerprint2, false);
  master_->DeleteFingerprint(kFingerprint4, false);

  EXPECT_EQ(0, master_->used_items_);
  for (int i = 0; i < kInitialSize; i++)
    EXPECT_EQ(zero_fingerprint, master_->hash_table_[i]) <<
        "Hash table has values in it.";
}

// When we delete more than kBigDeleteThreshold we trigger different behavior
// where the entire file is rewritten.
TEST_F(VisitedLinkTest, BigDelete) {
  ASSERT_TRUE(InitVisited(16381, true, true));

  // Add the base set of URLs that won't be deleted.
  // Reload() will test for these.
  for (int32_t i = 0; i < g_test_count; i++)
    master_->AddURL(TestURL(i));

  // Add more URLs than necessary to trigger this case.
  const int kTestDeleteCount = VisitedLinkMaster::kBigDeleteThreshold + 2;
  URLs urls_to_delete;
  for (int32_t i = g_test_count; i < g_test_count + kTestDeleteCount; i++) {
    GURL url(TestURL(i));
    master_->AddURL(url);
    urls_to_delete.push_back(url);
  }

  TestURLIterator iterator(urls_to_delete);
  master_->DeleteURLs(&iterator);
  master_->DebugValidate();

  Reload();
}

TEST_F(VisitedLinkTest, DeleteAll) {
  ASSERT_TRUE(InitVisited(0, true, true));

  {
    VisitedLinkSlave slave;
    slave.UpdateVisitedLinks(master_->mapped_table_memory().region.Duplicate());
    g_slaves.push_back(&slave);

    // Add the test URLs.
    for (int i = 0; i < g_test_count; i++) {
      master_->AddURL(TestURL(i));
      ASSERT_EQ(i + 1, master_->GetUsedCount());
    }
    master_->DebugValidate();

    // Make sure the slave picked up the adds.
    for (int i = 0; i < g_test_count; i++)
      EXPECT_TRUE(slave.IsVisited(TestURL(i)));

    // Clear the table and make sure the slave picked it up.
    master_->DeleteAllURLs();
    EXPECT_EQ(0, master_->GetUsedCount());
    for (int i = 0; i < g_test_count; i++) {
      EXPECT_FALSE(master_->IsVisited(TestURL(i)));
      EXPECT_FALSE(slave.IsVisited(TestURL(i)));
    }

    // Close the database.
    g_slaves.clear();
    ClearDB();
  }

  // Reopen and validate.
  ASSERT_TRUE(InitVisited(0, true, true));
  master_->DebugValidate();
  EXPECT_EQ(0, master_->GetUsedCount());
  for (int i = 0; i < g_test_count; i++)
    EXPECT_FALSE(master_->IsVisited(TestURL(i)));
}

// This tests that the master correctly resizes its tables when it gets too
// full, notifies its slaves of the change, and updates the disk.
TEST_F(VisitedLinkTest, Resizing) {
  // Create a very small database.
  const int32_t initial_size = 17;
  ASSERT_TRUE(InitVisited(initial_size, true, true));

  // ...and a slave
  VisitedLinkSlave slave;
  slave.UpdateVisitedLinks(master_->mapped_table_memory().region.Duplicate());
  g_slaves.push_back(&slave);

  int32_t used_count = master_->GetUsedCount();
  ASSERT_EQ(used_count, 0);

  for (int i = 0; i < g_test_count; i++) {
    master_->AddURL(TestURL(i));
    used_count = master_->GetUsedCount();
    ASSERT_EQ(i + 1, used_count);
  }

  // Verify that the table got resized sufficiently.
  int32_t table_size;
  VisitedLinkCommon::Fingerprint* table;
  master_->GetUsageStatistics(&table_size, &table);
  used_count = master_->GetUsedCount();
  ASSERT_GT(table_size, used_count);
  ASSERT_EQ(used_count, g_test_count) <<
                "table count doesn't match the # of things we added";

  // Verify that the slave got the resize message and has the same
  // table information.
  int32_t child_table_size;
  VisitedLinkCommon::Fingerprint* child_table;
  slave.GetUsageStatistics(&child_table_size, &child_table);
  ASSERT_EQ(table_size, child_table_size);
  for (int32_t i = 0; i < table_size; i++) {
    ASSERT_EQ(table[i], child_table[i]);
  }

  master_->DebugValidate();
  g_slaves.clear();

  // This tests that the file is written correctly by reading it in using
  // a new database.
  Reload();
}

// Tests that if the database doesn't exist, it will be rebuilt from history.
TEST_F(VisitedLinkTest, Rebuild) {
  // Add half of our URLs to history. This needs to be done before we
  // initialize the visited link DB.
  int history_count = g_test_count / 2;
  for (int i = 0; i < history_count; i++)
    delegate_.AddURLForRebuild(TestURL(i));

  // Initialize the visited link DB. Since the visited links file doesn't exist
  // and we don't suppress history rebuilding, this will load from history.
  ASSERT_TRUE(InitVisited(0, false, false));

  // While the table is rebuilding, add the rest of the URLs to the visited
  // link system. This isn't guaranteed to happen during the rebuild, so we
  // can't be 100% sure we're testing the right thing, but in practice is.
  // All the adds above will generally take some time queuing up on the
  // history thread, and it will take a while to catch up to actually
  // processing the rebuild that has queued behind it. We will generally
  // finish adding all of the URLs before it has even found the first URL.
  for (int i = history_count; i < g_test_count; i++)
    master_->AddURL(TestURL(i));

  // Add one more and then delete it.
  master_->AddURL(TestURL(g_test_count));
  URLs urls_to_delete;
  urls_to_delete.push_back(TestURL(g_test_count));
  TestURLIterator iterator(urls_to_delete);
  master_->DeleteURLs(&iterator);

  // Wait for the rebuild to complete. The task will terminate the message
  // loop when the rebuild is done. There's no chance that the rebuild will
  // complete before we set the task because the rebuild completion message
  // is posted to the message loop; until we Run() it, rebuild can not
  // complete.
  base::RunLoop run_loop;
  master_->set_rebuild_complete_task(run_loop.QuitClosure());
  run_loop.Run();

  // Test that all URLs were written to the database properly.
  Reload();

  // Make sure the extra one was *not* written (Reload won't test this).
  EXPECT_FALSE(master_->IsVisited(TestURL(g_test_count)));
}

// Test that importing a large number of URLs will work
TEST_F(VisitedLinkTest, BigImport) {
  ASSERT_TRUE(InitVisited(0, false, false));

  // Before the table rebuilds, add a large number of URLs
  int total_count = VisitedLinkMaster::kDefaultTableSize + 10;
  for (int i = 0; i < total_count; i++)
    master_->AddURL(TestURL(i));

  // Wait for the rebuild to complete.
  base::RunLoop run_loop;
  master_->set_rebuild_complete_task(run_loop.QuitClosure());
  run_loop.Run();

  // Ensure that the right number of URLs are present
  int used_count = master_->GetUsedCount();
  ASSERT_EQ(used_count, total_count);
}

TEST_F(VisitedLinkTest, Listener) {
  ASSERT_TRUE(InitVisited(0, true, true));

  TrackingVisitedLinkEventListener* listener =
      static_cast<TrackingVisitedLinkEventListener*>(master_->GetListener());

  // Verify that VisitedLinkMaster::Listener::Reset(true) was never called when
  // the table was created.
  EXPECT_EQ(0, listener->completely_reset_count());

  // Add test URLs.
  for (int i = 0; i < g_test_count; i++) {
    master_->AddURL(TestURL(i));
    ASSERT_EQ(i + 1, master_->GetUsedCount());
  }

  // Delete an URL.
  URLs urls_to_delete;
  urls_to_delete.push_back(TestURL(0));
  TestURLIterator iterator(urls_to_delete);
  master_->DeleteURLs(&iterator);

  // ... and all of the remaining ones.
  master_->DeleteAllURLs();

  // Verify that VisitedLinkMaster::Listener::Add was called for each added URL.
  EXPECT_EQ(g_test_count, listener->add_count());
  // Verify that VisitedLinkMaster::Listener::Reset was called both when one and
  // all URLs are deleted.
  EXPECT_EQ(2, listener->reset_count());

  ClearDB();

  ASSERT_TRUE(InitVisited(0, true, true));

  listener =
      static_cast<TrackingVisitedLinkEventListener*>(master_->GetListener());
  // Verify that VisitedLinkMaster::Listener::Reset(true) was called when the
  // table was loaded.
  EXPECT_EQ(1, listener->completely_reset_count());
}

class VisitCountingContext : public mojom::VisitedLinkNotificationSink {
 public:
  VisitCountingContext()
      : add_count_(0),
        add_event_count_(0),
        reset_event_count_(0),
        completely_reset_event_count_(0),
        new_table_count_(0) {}

  void Bind(mojo::ScopedMessagePipeHandle handle) {
    binding_.AddBinding(
        this, mojom::VisitedLinkNotificationSinkRequest(std::move(handle)));
  }

  void WaitForUpdate() {
    base::RunLoop run_loop;
    quit_closure_ = run_loop.QuitClosure();
    run_loop.Run();
  }

  void WaitForNoUpdate() { binding_.FlushForTesting(); }

  mojo::BindingSet<mojom::VisitedLinkNotificationSink>& binding() {
    return binding_;
  }

  void NotifyUpdate() {
    if (!quit_closure_.is_null())
      std::move(quit_closure_).Run();
  }

  void UpdateVisitedLinks(
      base::ReadOnlySharedMemoryRegion table_region) override {
    new_table_count_++;
    NotifyUpdate();
  }

  void AddVisitedLinks(const std::vector<uint64_t>& link_hashes) override {
    add_count_ += link_hashes.size();
    add_event_count_++;
    NotifyUpdate();
  }

  void ResetVisitedLinks(bool invalidate_cached_hashes) override {
    if (invalidate_cached_hashes)
      completely_reset_event_count_++;
    else
      reset_event_count_++;
    NotifyUpdate();
  }

  int add_count() const { return add_count_; }
  int add_event_count() const { return add_event_count_; }
  int reset_event_count() const { return reset_event_count_; }
  int completely_reset_event_count() const {
    return completely_reset_event_count_;
  }
  int new_table_count() const { return new_table_count_; }

 private:
  int add_count_;
  int add_event_count_;
  int reset_event_count_;
  int completely_reset_event_count_;
  int new_table_count_;

  base::Closure quit_closure_;
  mojo::BindingSet<mojom::VisitedLinkNotificationSink> binding_;

  DISALLOW_COPY_AND_ASSIGN(VisitCountingContext);
};

// Stub out as little as possible, borrowing from RenderProcessHost.
class VisitRelayingRenderProcessHost : public MockRenderProcessHost {
 public:
  explicit VisitRelayingRenderProcessHost(
      content::BrowserContext* browser_context,
      VisitCountingContext* context)
      : MockRenderProcessHost(browser_context) {
    OverrideBinderForTesting(
        mojom::VisitedLinkNotificationSink::Name_,
        base::Bind(&VisitCountingContext::Bind, base::Unretained(context)));
    content::NotificationService::current()->Notify(
        content::NOTIFICATION_RENDERER_PROCESS_CREATED,
        content::Source<RenderProcessHost>(this),
        content::NotificationService::NoDetails());
  }
  ~VisitRelayingRenderProcessHost() override {
    content::NotificationService::current()->Notify(
        content::NOTIFICATION_RENDERER_PROCESS_TERMINATED,
        content::Source<content::RenderProcessHost>(this),
        content::NotificationService::NoDetails());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(VisitRelayingRenderProcessHost);
};

class VisitedLinkRenderProcessHostFactory
    : public content::RenderProcessHostFactory {
 public:
  VisitedLinkRenderProcessHostFactory() : context_(new VisitCountingContext) {}
  content::RenderProcessHost* CreateRenderProcessHost(
      content::BrowserContext* browser_context,
      content::SiteInstance* site_instance) override {
    return new VisitRelayingRenderProcessHost(browser_context, context_.get());
  }

  VisitCountingContext* context() { return context_.get(); }

 private:
  std::unique_ptr<VisitCountingContext> context_;
  DISALLOW_COPY_AND_ASSIGN(VisitedLinkRenderProcessHostFactory);
};

class VisitedLinkEventsTest : public content::RenderViewHostTestHarness {
 public:
  void SetUp() override {
    SetRenderProcessHostFactory(&vc_rph_factory_);
    content::RenderViewHostTestHarness::SetUp();
  }

  void TearDown() override {
    // Explicitly destroy the master before proceeding with the rest
    // of teardown because it posts a task to close a file handle, and
    // we need to make sure we've finished all file related work
    // before our superclass sets about destroying the scoped temp
    // directory.
    master_.reset();
    RenderViewHostTestHarness::TearDown();
  }

  content::BrowserContext* CreateBrowserContext() override {
    content::BrowserContext* context = new content::TestBrowserContext();
    CreateVisitedLinkMaster(context);
    return context;
  }

  VisitCountingContext* context() { return vc_rph_factory_.context(); }

  VisitedLinkMaster* master() const {
    return master_.get();
  }

 protected:
  void CreateVisitedLinkMaster(content::BrowserContext* browser_context) {
    timer_.reset(new base::MockOneShotTimer());
    master_.reset(new VisitedLinkMaster(browser_context, &delegate_, true));
    static_cast<VisitedLinkEventListener*>(master_->GetListener())
        ->SetCoalesceTimerForTest(timer_.get());
    master_->Init();
  }

  VisitedLinkRenderProcessHostFactory vc_rph_factory_;

  TestVisitedLinkDelegate delegate_;
  std::unique_ptr<base::MockOneShotTimer> timer_;
  std::unique_ptr<VisitedLinkMaster> master_;
};

TEST_F(VisitedLinkEventsTest, Coalescence) {
  // Waiting complete rebuild the table.
  content::RunAllTasksUntilIdle();

  // After rebuild table expect reset event.
  EXPECT_EQ(1, context()->reset_event_count());

  // add some URLs to master.
  // Add a few URLs.
  master()->AddURL(GURL("http://acidtests.org/"));
  master()->AddURL(GURL("http://google.com/"));
  master()->AddURL(GURL("http://chromium.org/"));
  // Just for kicks, add a duplicate URL. This shouldn't increase the resulting
  master()->AddURL(GURL("http://acidtests.org/"));
  ASSERT_TRUE(timer_->IsRunning());
  timer_->Fire();

  context()->WaitForUpdate();

  // We now should have 3 entries added in 1 event.
  EXPECT_EQ(3, context()->add_count());
  EXPECT_EQ(1, context()->add_event_count());

  // Test whether the coalescing continues by adding a few more URLs.
  master()->AddURL(GURL("http://google.com/chrome/"));
  master()->AddURL(GURL("http://webkit.org/"));
  master()->AddURL(GURL("http://acid3.acidtests.org/"));

  ASSERT_TRUE(timer_->IsRunning());
  timer_->Fire();
  context()->WaitForUpdate();

  // We should have 6 entries added in 2 events.
  EXPECT_EQ(6, context()->add_count());
  EXPECT_EQ(2, context()->add_event_count());

  // Test whether duplicate entries produce add events.
  master()->AddURL(GURL("http://acidtests.org/"));
  EXPECT_FALSE(timer_->IsRunning());
  context()->WaitForNoUpdate();

  // We should have no change in results.
  EXPECT_EQ(6, context()->add_count());
  EXPECT_EQ(2, context()->add_event_count());

  // Ensure that the coalescing does not resume after resetting.
  master()->AddURL(GURL("http://build.chromium.org/"));
  EXPECT_TRUE(timer_->IsRunning());
  master()->DeleteAllURLs();
  EXPECT_FALSE(timer_->IsRunning());
  context()->WaitForNoUpdate();

  // We should have no change in results except for one new reset event.
  EXPECT_EQ(6, context()->add_count());
  EXPECT_EQ(2, context()->add_event_count());
  EXPECT_EQ(2, context()->reset_event_count());
}

TEST_F(VisitedLinkEventsTest, Basics) {
  RenderViewHostTester::For(rvh())->CreateTestRenderView(
      base::string16(), MSG_ROUTING_NONE, MSG_ROUTING_NONE, false);

  // Waiting complete rebuild the table.
  content::RunAllTasksUntilIdle();

  // After rebuild table expect reset event.
  EXPECT_EQ(1, context()->reset_event_count());

  // Add a few URLs.
  master()->AddURL(GURL("http://acidtests.org/"));
  master()->AddURL(GURL("http://google.com/"));
  master()->AddURL(GURL("http://chromium.org/"));
  ASSERT_TRUE(timer_->IsRunning());
  timer_->Fire();
  context()->WaitForUpdate();

  // We now should have 1 add event.
  EXPECT_EQ(1, context()->add_event_count());
  EXPECT_EQ(1, context()->reset_event_count());

  master()->DeleteAllURLs();

  EXPECT_FALSE(timer_->IsRunning());
  context()->WaitForNoUpdate();

  // We should have no change in add results, plus one new reset event.
  EXPECT_EQ(1, context()->add_event_count());
  EXPECT_EQ(2, context()->reset_event_count());
}

TEST_F(VisitedLinkEventsTest, TabVisibility) {
  RenderViewHostTester::For(rvh())->CreateTestRenderView(
      base::string16(), MSG_ROUTING_NONE, MSG_ROUTING_NONE, false);

  // Waiting complete rebuild the table.
  content::RunAllTasksUntilIdle();

  // After rebuild table expect reset event.
  EXPECT_EQ(1, context()->reset_event_count());

  // Simulate tab becoming inactive.
  RenderViewHostTester::For(rvh())->SimulateWasHidden();

  // Add a few URLs.
  master()->AddURL(GURL("http://acidtests.org/"));
  master()->AddURL(GURL("http://google.com/"));
  master()->AddURL(GURL("http://chromium.org/"));
  ASSERT_TRUE(timer_->IsRunning());
  timer_->Fire();
  context()->WaitForNoUpdate();

  // We shouldn't have any events.
  EXPECT_EQ(0, context()->add_event_count());
  EXPECT_EQ(1, context()->reset_event_count());

  // Simulate the tab becoming active.
  RenderViewHostTester::For(rvh())->SimulateWasShown();
  context()->WaitForUpdate();

  // We should now have 3 add events, still no reset events.
  EXPECT_EQ(1, context()->add_event_count());
  EXPECT_EQ(1, context()->reset_event_count());

  // Deactivate the tab again.
  RenderViewHostTester::For(rvh())->SimulateWasHidden();

  // Add a bunch of URLs (over 50) to exhaust the link event buffer.
  for (int i = 0; i < 100; i++)
    master()->AddURL(TestURL(i));

  ASSERT_TRUE(timer_->IsRunning());
  timer_->Fire();
  context()->WaitForNoUpdate();

  // Again, no change in events until tab is active.
  EXPECT_EQ(1, context()->add_event_count());
  EXPECT_EQ(1, context()->reset_event_count());

  // Activate the tab.
  RenderViewHostTester::For(rvh())->SimulateWasShown();
  EXPECT_FALSE(timer_->IsRunning());
  context()->WaitForUpdate();

  // We should have only one more reset event.
  EXPECT_EQ(1, context()->add_event_count());
  EXPECT_EQ(2, context()->reset_event_count());
}

// Tests that VisitedLink ignores renderer process creation notification for a
// different context.
TEST_F(VisitedLinkEventsTest, IgnoreRendererCreationFromDifferentContext) {
  content::TestBrowserContext different_context;
  VisitCountingContext counting_context;
  VisitRelayingRenderProcessHost different_process_host(&different_context,
                                                        &counting_context);

  size_t old_size = counting_context.binding().size();
  content::NotificationService::current()->Notify(
      content::NOTIFICATION_RENDERER_PROCESS_CREATED,
      content::Source<content::RenderProcessHost>(&different_process_host),
      content::NotificationService::NoDetails());
  size_t new_size = counting_context.binding().size();
  EXPECT_EQ(old_size, new_size);
}

class VisitedLinkCompletelyResetEventTest : public VisitedLinkEventsTest {
 public:
  content::BrowserContext* CreateBrowserContext() override {
    content::BrowserContext* context = new content::TestBrowserContext();
    CreateVisitedLinkFile(context);
    CreateVisitedLinkMaster(context);
    return context;
  }

  void CreateVisitedLinkFile(content::BrowserContext* browser_context) {
    base::FilePath visited_file =
        browser_context->GetPath().Append(FILE_PATH_LITERAL("Visited Links"));
    std::unique_ptr<VisitedLinkMaster> master(
        new VisitedLinkMaster(new TrackingVisitedLinkEventListener(),
                              &delegate_, true, true, visited_file, 0));
    master->Init();
    // Waiting complete create the table.
    content::RunAllTasksUntilIdle();

    master.reset();
    // Wait for all pending file I/O to be completed.
    content::RunAllTasksUntilIdle();
  }
};

TEST_F(VisitedLinkCompletelyResetEventTest, LoadTable) {
  // Waiting complete loading the table.
  content::RunAllTasksUntilIdle();

  context()->binding().FlushForTesting();

  // After load table expect completely reset event.
  EXPECT_EQ(1, context()->completely_reset_event_count());
}

}  // namespace visitedlink
