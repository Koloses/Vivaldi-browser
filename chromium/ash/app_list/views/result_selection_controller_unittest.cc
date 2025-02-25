// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/views/result_selection_controller.h"

#include <gtest/gtest.h>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ash/app_list/test/app_list_test_view_delegate.h"
#include "ash/app_list/views/search_result_actions_view.h"
#include "ash/app_list/views/search_result_actions_view_delegate.h"
#include "ash/app_list/views/search_result_container_view.h"
#include "base/macros.h"
#include "ui/events/event.h"

namespace app_list {
namespace {

class TestResultViewWithActions;

class TestResultView : public SearchResultBaseView {
 public:
  TestResultView() = default;
  ~TestResultView() override = default;

  virtual TestResultViewWithActions* AsResultViewWithActions() {
    return nullptr;
  }

  void ButtonPressed(Button* sender, const ui::Event& event) override {
    // Do nothing for test.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestResultView);
};

class TestResultViewWithActions : public TestResultView,
                                  public SearchResultActionsViewDelegate {
 public:
  TestResultViewWithActions()
      : actions_view_owned_(std::make_unique<SearchResultActionsView>(this)) {
    set_actions_view(actions_view_owned_.get());
  }

  // TestResultView:
  TestResultViewWithActions* AsResultViewWithActions() override { return this; }

  // SearchResultActionsViewDelegate:
  void OnSearchResultActionActivated(size_t index, int event_flags) override {}
  void OnSearchResultActionsUnSelected() override {}
  bool IsSearchResultHoveredOrSelected() override { return selected(); }

  SearchResultActionsView* GetActionsView() {
    return actions_view_owned_.get();
  }

 private:
  std::unique_ptr<SearchResultActionsView> actions_view_owned_;

  DISALLOW_COPY_AND_ASSIGN(TestResultViewWithActions);
};

// Allows immediate invocation of |VerticalTestContainer| and its derivatives,
// by handling the fake delegate's setup.
class TestContainerDelegateHarness {
 public:
  TestContainerDelegateHarness() {
    app_list_test_delegate_ =
        std::make_unique<app_list::test::AppListTestViewDelegate>();
  }

  ~TestContainerDelegateHarness() = default;

 protected:
  std::unique_ptr<app_list::test::AppListTestViewDelegate>
      app_list_test_delegate_;

  DISALLOW_COPY_AND_ASSIGN(TestContainerDelegateHarness);
};

struct TestContainerParams {
  TestContainerParams() = default;
  TestContainerParams(bool horizontal, int result_count)
      : horizontal(horizontal), result_count(result_count) {}
  TestContainerParams(bool horizontal, int result_count, int actions_per_result)
      : horizontal(horizontal),
        result_count(result_count),
        actions_per_result(actions_per_result) {}

  // Whether the contairne is horizontal.
  bool horizontal = false;

  // Number of results in the container.
  int result_count = 0;

  // If set, the container will contain TestResultViewWithActions that
  // have |actions_per_result| actions each.
  base::Optional<int> actions_per_result;
};

class TestContainer : public TestContainerDelegateHarness,
                      public SearchResultContainerView {
 public:
  explicit TestContainer(const TestContainerParams& params)
      : SearchResultContainerView(app_list_test_delegate_.get()) {
    set_horizontally_traversable(params.horizontal);

    for (int i = 0; i < params.result_count; ++i) {
      if (params.actions_per_result.has_value()) {
        auto result = std::make_unique<TestResultViewWithActions>();
        result->GetActionsView()->SetActions(std::vector<SearchResult::Action>(
            params.actions_per_result.value(),
            SearchResult::Action(gfx::ImageSkia(), base::string16(), false)));
        search_result_views_.emplace_back(std::move(result));
      } else {
        search_result_views_.emplace_back(std::make_unique<TestResultView>());
      }

      search_result_views_.back()->set_index_in_container(i);
    }

    Update();
  }
  ~TestContainer() override = default;

  // SearchResultContainerView:
  SearchResultBaseView* GetResultViewAt(size_t index) override {
    DCHECK_LT(index, search_result_views_.size());
    return search_result_views_[index].get();
  }
  SearchResultBaseView* GetFirstResultView() override {
    return GetResultViewAt(0);
  }

 private:
  int DoUpdate() override { return search_result_views_.size(); }

  std::vector<std::unique_ptr<TestResultView>> search_result_views_;

  DISALLOW_COPY_AND_ASSIGN(TestContainer);
};

class ResultSelectionTest : public testing::Test,
                            public testing::WithParamInterface<bool> {
 public:
  ResultSelectionTest() = default;
  ~ResultSelectionTest() override = default;

  void SetUp() override {
    if (testing::UnitTest::GetInstance()->current_test_info()->value_param()) {
      // Setup right to left environment if necessary.
      is_rtl_ = GetParam();
      if (is_rtl_)
        base::i18n::SetICUDefaultLocale("he");
    }

    if (!is_rtl_) {
      // Reset RTL if not needed.
      base::i18n::SetICUDefaultLocale("en");
    }

    result_selection_controller_ =
        std::make_unique<ResultSelectionController>(&containers_);

    testing::Test::SetUp();
  }

 protected:
  std::vector<std::unique_ptr<SearchResultContainerView>> CreateContainerVector(
      int container_count,
      const TestContainerParams& container_params) {
    std::vector<std::unique_ptr<SearchResultContainerView>> containers;
    for (int i = 0; i < container_count; i++) {
      containers.emplace_back(
          std::make_unique<TestContainer>(container_params));
    }
    return containers;
  }

  std::vector<ResultLocationDetails> CreateLocationVector(
      int results,
      bool horizontal,
      int container_count = 1,
      int container_index = 0) {
    std::vector<ResultLocationDetails> locations;
    for (int i = 0; i < results; i++) {
      locations.emplace_back(ResultLocationDetails(
          container_index /*container_index*/,
          container_count /*container_count*/, i /*result_index*/,
          results /*result_count*/, horizontal /*container_is_horizontal*/));
    }
    return locations;
  }

  ResultLocationDetails GetCurrentLocation() const {
    return *result_selection_controller_->selected_location_details();
  }

  TestResultView* GetCurrentSelection() {
    ResultLocationDetails location = GetCurrentLocation();
    SearchResultBaseView* view =
        containers_[location.container_index]->GetResultViewAt(
            location.result_index);
    return static_cast<TestResultView*>(view);
  }

  // Asserts that currently selected result has a result action selected.
  // |action_index| - the index of the selected result action.
  testing::AssertionResult CurrentResultActionSelected(int action_index) {
    TestResultViewWithActions* view =
        GetCurrentSelection()->AsResultViewWithActions();
    if (!view) {
      return testing::AssertionFailure()
             << "Selected view with no action support";
    }

    if (!view->selected())
      return testing::AssertionFailure() << "View not selected";

    if (!view->GetActionsView()->HasSelectedAction())
      return testing::AssertionFailure() << "No selected action";

    int selected_action = view->GetActionsView()->GetSelectedAction();
    if (selected_action != action_index) {
      return testing::AssertionFailure()
             << "Wrong selected action " << selected_action;
    }

    return testing::AssertionSuccess();
  }

  // Asserts that the currently selected result has no selected result actions.
  testing::AssertionResult CurrentResultActionNotSelected() {
    TestResultViewWithActions* view =
        GetCurrentSelection()->AsResultViewWithActions();
    if (!view) {
      return testing::AssertionFailure()
             << "Selected view with no action support";
    }

    if (!view->selected())
      return testing::AssertionFailure() << "View not selected";

    int selected_action = view->GetActionsView()->GetSelectedAction();
    if (view->GetActionsView()->HasSelectedAction()) {
      return testing::AssertionFailure()
             << "Selected action found " << selected_action;
    }

    if (selected_action != -1) {
      return testing::AssertionFailure()
             << "Expected selected action index -1; found " << selected_action;
    }
    return testing::AssertionSuccess();
  }

  void SetContainers(
      const std::vector<std::unique_ptr<SearchResultContainerView>>&
          containers) {
    containers_.clear();
    for (auto& container : containers) {
      containers_.emplace_back(container.get());
    }
  }

  void TestSingleAxisTraversal(ui::KeyEvent* forward, ui::KeyEvent* backward) {
    const bool horizontal =
        result_selection_controller_->selected_location_details()
            ->container_is_horizontal;

    std::vector<ResultLocationDetails> locations =
        CreateLocationVector(4, horizontal);

    // These are divided to give as much detail as possible for a test failure.
    TestSingleAxisForward(forward, locations);
    TestSingleAxisLoop(forward, locations);
    TestSingleAxisLoopBack(backward, locations);
    TestSingleAxisBackward(backward, locations);
  }

  void TestSingleAxisForward(
      ui::KeyEvent* forward,
      const std::vector<ResultLocationDetails>& locations) {
    // Starts at the beginning
    ASSERT_EQ(*result_selection_controller_->selected_location_details(),
              locations[0]);
    for (size_t i = 1; i < locations.size(); i++) {
      result_selection_controller_->MoveSelection(*forward);
      ASSERT_EQ(*result_selection_controller_->selected_location_details(),
                locations[i]);
    }
    // Expect steady traversal to the end
  }

  void TestSingleAxisLoop(ui::KeyEvent* forward,
                          const std::vector<ResultLocationDetails>& locations) {
    // Starts at the end
    ASSERT_EQ(*result_selection_controller_->selected_location_details(),
              locations[3]);

    // Expect loop back to first result.
    result_selection_controller_->MoveSelection(*forward);
    ASSERT_EQ(*result_selection_controller_->selected_location_details(),
              locations[0]);
  }

  void TestSingleAxisLoopBack(
      ui::KeyEvent* backward,
      const std::vector<ResultLocationDetails>& locations) {
    // Starts at the first
    ASSERT_EQ(*result_selection_controller_->selected_location_details(),
              locations[0]);

    // Expect loop back to last result.
    result_selection_controller_->MoveSelection(*backward);
    ASSERT_EQ(*result_selection_controller_->selected_location_details(),
              locations[3]);
  }

  void TestSingleAxisBackward(
      ui::KeyEvent* backward,
      const std::vector<ResultLocationDetails>& locations) {
    ASSERT_FALSE(locations.empty());
    const size_t last_index = locations.size() - 1;

    // Test reverse direction from last result
    ASSERT_EQ(*result_selection_controller_->selected_location_details(),
              locations[last_index]);
    for (size_t i = last_index; i > 0; i--) {
      result_selection_controller_->MoveSelection(*backward);
      ASSERT_EQ(*result_selection_controller_->selected_location_details(),
                locations[i - 1]);
    }
  }

  void TestMultiAxisTraversal(bool tab) {
    ui::KeyEvent* forward;
    ui::KeyEvent* backward;
    ui::KeyEvent* vertical_forward;
    ui::KeyEvent* vertical_backward;
    if (tab) {
      // Set up for tab traversal
      forward = vertical_forward = &tab_key_;
      backward = vertical_backward = &shift_tab_key_;
    } else {
      // Set up for arrow key traversal
      forward = is_rtl_ ? &left_arrow_ : &right_arrow_;
      backward = is_rtl_ ? &right_arrow_ : &left_arrow_;
      vertical_forward = &down_arrow_;
      vertical_backward = &up_arrow_;
    }

    int num_containers = containers_.size();

    for (int i = 0; i < num_containers; i++) {
      const bool horizontal =
          result_selection_controller_->selected_location_details()
              ->container_is_horizontal;

      std::vector<ResultLocationDetails> locations =
          CreateLocationVector(4, horizontal, num_containers, i);

      if (horizontal && !tab) {
        TestSingleAxisForward(forward, locations);
        TestSingleAxisLoop(forward, locations);
        TestSingleAxisLoopBack(backward, locations);
        TestSingleAxisBackward(backward, locations);
        TestSingleAxisLoopBack(backward, locations);
        // End on the last result.
      } else {
        // Tab and Vertical traversal are identical
        TestSingleAxisForward(vertical_forward, locations);
        TestSingleAxisBackward(vertical_backward, locations);
        TestSingleAxisForward(vertical_forward, locations);
        // End on the last result.
      }

      // Change Containers
      result_selection_controller_->MoveSelection(*vertical_forward);
    }
  }

  // Runs test for TAB traversal over containers that contain results with extra
  // result actions.
  void TestTabTraversalWithResultActions(bool horizontal_containers) {
    const int kContainerCount = 2;
    const int kResultsPerContainer = 2;
    const int kActionsPerResult = 2;
    std::vector<std::unique_ptr<SearchResultContainerView>> containers =
        CreateContainerVector(
            kContainerCount,
            TestContainerParams(horizontal_containers, kResultsPerContainer,
                                kActionsPerResult));
    SetContainers(containers);

    auto create_test_location = [horizontal_containers](int container_index,
                                                        int result_index) {
      return ResultLocationDetails(container_index, kContainerCount,
                                   result_index, kResultsPerContainer,
                                   horizontal_containers);
    };

    // Initialize the RSC for test.
    result_selection_controller_->ResetSelection();

    ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionNotSelected());

    // TAB - the result should remain the same, but the selected action is
    // expected to change.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(0));

    // TAB - the result should remain the same, but the selected action is
    // expected to change.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(1));

    // TAB - move to the next result.
    TestResultView* previous_result = GetCurrentSelection();
    EXPECT_TRUE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionNotSelected());
    EXPECT_FALSE(previous_result->selected());

    // Shift-TAB - move back to the previous result, and expect the last action
    // to be selected.
    EXPECT_TRUE(result_selection_controller_->MoveSelection(shift_tab_key_));

    ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(1));

    // TAB - move back to next result.
    previous_result = GetCurrentSelection();
    EXPECT_TRUE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionNotSelected());
    EXPECT_FALSE(previous_result->selected());

    // TAB - stay at the same result, but select next action.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(0));

    // Shift-TAB - same result, but deselects actions.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(shift_tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionNotSelected());

    // TAB - reselect the first action.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(0));

    // TAB - select the next action.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(1));

    // TAB - select a result in the next container.
    previous_result = GetCurrentSelection();
    EXPECT_TRUE(result_selection_controller_->MoveSelection(tab_key_));

    ASSERT_EQ(create_test_location(1, 0), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionNotSelected());
    EXPECT_FALSE(previous_result->selected());

    // Shift-TAB - move to previous result/action.
    previous_result = GetCurrentSelection();
    EXPECT_TRUE(result_selection_controller_->MoveSelection(shift_tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(1));
    EXPECT_FALSE(previous_result->selected());

    // Shift-TAB - move to previous action.
    EXPECT_FALSE(result_selection_controller_->MoveSelection(shift_tab_key_));

    ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
    EXPECT_TRUE(CurrentResultActionSelected(0));
  }

  std::unique_ptr<ResultSelectionController> result_selection_controller_;
  std::vector<SearchResultContainerView*> containers_;

  // Set up key events for test. These will never be marked as 'handled'.
  ui::KeyEvent down_arrow_ =
      ui::KeyEvent(ui::ET_KEY_PRESSED, ui::VKEY_DOWN, ui::EF_NONE);
  ui::KeyEvent up_arrow_ =
      ui::KeyEvent(ui::ET_KEY_PRESSED, ui::VKEY_UP, ui::EF_NONE);
  ui::KeyEvent left_arrow_ =
      ui::KeyEvent(ui::ET_KEY_PRESSED, ui::VKEY_LEFT, ui::EF_NONE);
  ui::KeyEvent right_arrow_ =
      ui::KeyEvent(ui::ET_KEY_PRESSED, ui::VKEY_RIGHT, ui::EF_NONE);
  ui::KeyEvent tab_key_ =
      ui::KeyEvent(ui::ET_KEY_PRESSED, ui::VKEY_TAB, ui::EF_NONE);
  ui::KeyEvent shift_tab_key_ =
      ui::KeyEvent(ui::ET_KEY_PRESSED, ui::VKEY_TAB, ui::EF_SHIFT_DOWN);

  bool is_rtl_ = false;

  DISALLOW_COPY_AND_ASSIGN(ResultSelectionTest);
};

INSTANTIATE_TEST_SUITE_P(RTL, ResultSelectionTest, testing::Bool());

}  // namespace

TEST_F(ResultSelectionTest, VerticalTraversalOneContainerArrowKeys) {
  std::unique_ptr<TestContainer> vertical_container =
      std::make_unique<TestContainer>(TestContainerParams(false, 4));
  // The vertical container is not horizontally traversable
  ASSERT_FALSE(vertical_container->horizontally_traversable());

  containers_.clear();
  containers_.emplace_back(vertical_container.get());

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestSingleAxisTraversal(&down_arrow_, &up_arrow_);
}

TEST_F(ResultSelectionTest, VerticalTraversalOneContainerTabKey) {
  std::unique_ptr<TestContainer> vertical_container =
      std::make_unique<TestContainer>(TestContainerParams(false, 4));

  // The vertical container is not horizontally traversable
  ASSERT_FALSE(vertical_container->horizontally_traversable());

  containers_.clear();
  containers_.emplace_back(vertical_container.get());

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestSingleAxisTraversal(&tab_key_, &shift_tab_key_);
}

TEST_P(ResultSelectionTest, HorizontalTraversalOneContainerArrowKeys) {
  ui::KeyEvent* forward = is_rtl_ ? &left_arrow_ : &right_arrow_;
  ui::KeyEvent* backward = is_rtl_ ? &right_arrow_ : &left_arrow_;

  std::unique_ptr<TestContainer> horizontal_container =
      std::make_unique<TestContainer>(TestContainerParams(true, 4));

  // The horizontal container is horizontally traversable
  ASSERT_TRUE(horizontal_container->horizontally_traversable());

  containers_.clear();
  containers_.emplace_back(horizontal_container.get());

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestSingleAxisTraversal(forward, backward);
}

TEST_P(ResultSelectionTest, HorizontalVerticalArrowKeys) {
  std::unique_ptr<TestContainer> horizontal_container =
      std::make_unique<TestContainer>(TestContainerParams(true, 4));
  std::unique_ptr<TestContainer> vertical_container =
      std::make_unique<TestContainer>(TestContainerParams(false, 4));

  containers_.clear();
  containers_.emplace_back(horizontal_container.get());
  containers_.emplace_back(vertical_container.get());

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, HorizontalVerticalTab) {
  std::unique_ptr<TestContainer> horizontal_container =
      std::make_unique<TestContainer>(TestContainerParams(true, 4));
  std::unique_ptr<TestContainer> vertical_container =
      std::make_unique<TestContainer>(TestContainerParams(false, 4));

  containers_.clear();
  containers_.emplace_back(horizontal_container.get());
  containers_.emplace_back(vertical_container.get());

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(true);
}

TEST_F(ResultSelectionTest, TestVerticalStackArrows) {
  std::vector<std::unique_ptr<SearchResultContainerView>> vertical_containers =
      CreateContainerVector(4, TestContainerParams(false, 4));
  SetContainers(vertical_containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, TestVerticalStackTab) {
  std::vector<std::unique_ptr<SearchResultContainerView>> vertical_containers =
      CreateContainerVector(4, TestContainerParams(false, 4));
  SetContainers(vertical_containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(true);
}

TEST_P(ResultSelectionTest, TestHorizontalStackArrows) {
  std::vector<std::unique_ptr<SearchResultContainerView>>
      horizontal_containers =
          CreateContainerVector(4, TestContainerParams(true, 4));
  SetContainers(horizontal_containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, TestHorizontalStackTab) {
  std::vector<std::unique_ptr<SearchResultContainerView>>
      horizontal_containers =
          CreateContainerVector(4, TestContainerParams(true, 4));
  SetContainers(horizontal_containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(true);
}

TEST_P(ResultSelectionTest, TestHorizontalStackWithResultActionsArrows) {
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(4, TestContainerParams(true, 4, 2));
  SetContainers(containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, TestVerticalStackWithResultActionsArrows) {
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(4, TestContainerParams(false, 4, 2));
  SetContainers(containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, TestVerticalStackWithEmptyResultActionsTab) {
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(4, TestContainerParams(false, 4, 0));
  SetContainers(containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, TestHorizontalStackWithEmptyResultActionsTab) {
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(4, TestContainerParams(false, 4, 0));
  SetContainers(containers);

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  TestMultiAxisTraversal(false);
}

TEST_F(ResultSelectionTest, TestHorizontalStackWithResultActionsTab) {
  TestTabTraversalWithResultActions(true /*horizontal_container*/);
}

TEST_F(ResultSelectionTest, TestVerticalStackWithResultActionsTab) {
  TestTabTraversalWithResultActions(false /*horizontal_container*/);
}

TEST_P(ResultSelectionTest,
       TestHorizontalStackWithResultActions_ForwardBackWithActionSelected) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  const int kActionsPerResult = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(
          kContainerCount,
          TestContainerParams(true, kResultsPerContainer, kActionsPerResult));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 true /*container_is_horizontal*/);
  };

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  ui::KeyEvent* forward = is_rtl_ ? &left_arrow_ : &right_arrow_;
  ui::KeyEvent* backward = is_rtl_ ? &right_arrow_ : &left_arrow_;

  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // TAB to select an action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));

  // Forward selects the next result.
  TestResultView* previous_result = GetCurrentSelection();
  EXPECT_TRUE(result_selection_controller_->MoveSelection(*forward));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());
  EXPECT_FALSE(previous_result->selected());

  // TAB to select an action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));

  // Backward selects the previous result.
  previous_result = GetCurrentSelection();
  EXPECT_TRUE(result_selection_controller_->MoveSelection(*backward));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());
  EXPECT_FALSE(previous_result->selected());
}

TEST_F(ResultSelectionTest,
       TestVerticalStackWithResultActions_UpDownWithActionSelected) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  const int kActionsPerResult = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(
          kContainerCount,
          TestContainerParams(false, kResultsPerContainer, kActionsPerResult));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 false /*container_is_horizontal*/);
  };

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // TAB to select an action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));

  // DOWN selects the next result.
  TestResultView* previous_result = GetCurrentSelection();
  EXPECT_TRUE(result_selection_controller_->MoveSelection(down_arrow_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());
  EXPECT_FALSE(previous_result->selected());

  // TAB to select an action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));

  // UP selects the previous result.
  previous_result = GetCurrentSelection();
  EXPECT_TRUE(result_selection_controller_->MoveSelection(up_arrow_));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());
  EXPECT_FALSE(previous_result->selected());
}

TEST_F(ResultSelectionTest, ResetWhileFirstResultActionSelected) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  const int kActionsPerResult = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(
          kContainerCount,
          TestContainerParams(false, kResultsPerContainer, kActionsPerResult));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 false /*container_is_horizontal*/);
  };

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // TAB to select an action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));

  // Reset selection - reset selects the new first result, i.e. the same result
  // as before reset. The selected action should remain the same.
  TestResultView* pre_reset_selection = GetCurrentSelection();
  result_selection_controller_->ResetSelection();
  EXPECT_EQ(pre_reset_selection, GetCurrentSelection());
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));
}

TEST_F(ResultSelectionTest, ResetWhileResultActionSelected) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  const int kActionsPerResult = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(
          kContainerCount,
          TestContainerParams(false, kResultsPerContainer, kActionsPerResult));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 false /*container_is_horizontal*/);
  };

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // DOWN to select another result.
  EXPECT_TRUE(result_selection_controller_->MoveSelection(down_arrow_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // TAB to select an action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(0));

  // Reset selection.
  TestResultView* pre_reset_selection = GetCurrentSelection();
  result_selection_controller_->ResetSelection();
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());
  EXPECT_FALSE(pre_reset_selection->selected());
}

TEST_F(ResultSelectionTest, ActionRemovedWhileSelected) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  const int kActionsPerResult = 3;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(
          kContainerCount,
          TestContainerParams(false, kResultsPerContainer, kActionsPerResult));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 false /*container_is_horizontal*/);
  };

  // Initialize the RSC for test.
  result_selection_controller_->ResetSelection();

  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // DOWN to select another result.
  EXPECT_TRUE(result_selection_controller_->MoveSelection(down_arrow_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // TAB to select the last action.
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  EXPECT_FALSE(result_selection_controller_->MoveSelection(tab_key_));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(2));

  TestResultView* selected_view = GetCurrentSelection();
  ASSERT_TRUE(selected_view);
  ASSERT_TRUE(selected_view->AsResultViewWithActions());
  // Remove two trailing actions - the result action is de-selected.
  selected_view->AsResultViewWithActions()->GetActionsView()->SetActions(
      std::vector<SearchResult::Action>(
          1, SearchResult::Action(gfx::ImageSkia(), base::string16(), false)));
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionNotSelected());

  // Shift-TAB move selection to the previous result.
  EXPECT_TRUE(result_selection_controller_->MoveSelection(shift_tab_key_));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(CurrentResultActionSelected(2));
}

TEST_F(ResultSelectionTest, ResetSelectionWithSelectionChangesBlocked) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(kContainerCount,
                            TestContainerParams(false, kResultsPerContainer));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 false /*container_is_horizontal*/);
  };

  // Set up non default selection,
  result_selection_controller_->ResetSelection();
  result_selection_controller_->MoveSelection(down_arrow_);
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());

  // Test that calling reset selection while selection changes are blocked does
  // not change the selected result.
  result_selection_controller_->set_block_selection_changes(true);

  result_selection_controller_->ResetSelection();
  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(result_selection_controller_->selected_result());

  // Reset should be enabled once selection changes are unblocked.
  result_selection_controller_->set_block_selection_changes(false);

  result_selection_controller_->ResetSelection();

  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(result_selection_controller_->selected_result());
}

TEST_F(ResultSelectionTest, InitialResetSelectionWithSelectionChangesBlocked) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(kContainerCount,
                            TestContainerParams(false, kResultsPerContainer));
  SetContainers(containers);

  // Test that calling reset selection while selection changes are blocked does
  // not set the selected result.
  result_selection_controller_->set_block_selection_changes(true);
  result_selection_controller_->ResetSelection();

  EXPECT_FALSE(result_selection_controller_->selected_result());
  EXPECT_FALSE(result_selection_controller_->selected_location_details());

  // Reset should be enabled once selection changes are unblocked.
  result_selection_controller_->set_block_selection_changes(false);

  result_selection_controller_->ResetSelection();

  EXPECT_TRUE(result_selection_controller_->selected_result());
  EXPECT_TRUE(result_selection_controller_->selected_location_details());
}

TEST_F(ResultSelectionTest, MoveSelectionWithSelectionChangesBlocked) {
  const int kContainerCount = 2;
  const int kResultsPerContainer = 2;
  std::vector<std::unique_ptr<SearchResultContainerView>> containers =
      CreateContainerVector(kContainerCount,
                            TestContainerParams(false, kResultsPerContainer));
  SetContainers(containers);

  auto create_test_location = [](int container_index, int result_index) {
    return ResultLocationDetails(container_index, kContainerCount, result_index,
                                 kResultsPerContainer,
                                 false /*container_is_horizontal*/);
  };

  result_selection_controller_->ResetSelection();
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());

  // Test that calling move selection while selection chages are blocked does
  // not change the selected result.
  result_selection_controller_->set_block_selection_changes(true);

  EXPECT_FALSE(result_selection_controller_->MoveSelection(down_arrow_));
  ASSERT_EQ(create_test_location(0, 0), GetCurrentLocation());
  EXPECT_TRUE(result_selection_controller_->selected_result());

  // Move should succeed once selection changes are unblocked.
  result_selection_controller_->set_block_selection_changes(false);
  EXPECT_TRUE(result_selection_controller_->MoveSelection(down_arrow_));

  ASSERT_EQ(create_test_location(0, 1), GetCurrentLocation());
  EXPECT_TRUE(result_selection_controller_->selected_result());
}

}  // namespace app_list
