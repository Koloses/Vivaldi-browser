// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/wm/overview/overview_window_drag_controller.h"

#include <memory>

#include "ash/display/mouse_cursor_event_filter.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/presentation_time_recorder.h"
#include "ash/screen_util.h"
#include "ash/shell.h"
#include "ash/wm/desks/desk_preview_view.h"
#include "ash/wm/desks/desks_bar_view.h"
#include "ash/wm/desks/desks_util.h"
#include "ash/wm/overview/overview_constants.h"
#include "ash/wm/overview/overview_controller.h"
#include "ash/wm/overview/overview_grid.h"
#include "ash/wm/overview/overview_item.h"
#include "ash/wm/overview/overview_session.h"
#include "ash/wm/overview/overview_utils.h"
#include "ash/wm/splitview/split_view_constants.h"
#include "ash/wm/splitview/split_view_drag_indicators.h"
#include "ash/wm/splitview/split_view_utils.h"
#include "ash/wm/window_positioning_utils.h"
#include "ash/wm/window_util.h"
#include "base/numerics/ranges.h"
#include "ui/aura/window.h"
#include "ui/wm/core/coordinate_conversion.h"

namespace ash {

namespace {

// The amount of distance from the start of drag the item needs to be dragged
// vertically for it to be closed on release.
constexpr float kDragToCloseDistanceThresholdDp = 160.f;

// The minimum distance that will be considered as a drag event.
constexpr float kMinimumDragDistanceDp = 5.f;
// Items dragged to within |kDistanceFromEdgeDp| of the screen will get snapped
// even if they have not moved by |kMinimumDragDistanceDp|.
constexpr float kDistanceFromEdgeDp = 16.f;
// The minimum distance that an item must be moved before it is snapped. This
// prevents accidental snaps.
constexpr float kMinimumDragToSnapDistanceDp = 96.f;
// The minimum distance that an item must be moved before it is considered a
// drag event, if the drag starts in one of the snap regions.
constexpr float kMinimumDragDistanceAlreadyInSnapRegionDp = 48.f;

// Flings with less velocity than this will not close the dragged item.
constexpr float kFlingToCloseVelocityThreshold = 2000.f;
constexpr float kItemMinOpacity = 0.4f;

// The UMA histogram that records presentation time for window dragging
// operation in overview mode.
constexpr char kOverviewWindowDragHistogram[] =
    "Ash.Overview.WindowDrag.PresentationTime.TabletMode";
constexpr char kOverviewWindowDragMaxLatencyHistogram[] =
    "Ash.Overview.WindowDrag.PresentationTime.MaxLatency.TabletMode";

void UnpauseOcclusionTracker() {
  Shell::Get()->overview_controller()->UnpauseOcclusionTracker(
      kOcclusionPauseDurationForDragMs);
}

// Returns the scaled-down size of the dragged item that should be used when
// it's dragged over the DesksBarView.
gfx::SizeF GetItemSizeWhenOnDesksBar(OverviewItem* item) {
  DCHECK(item);

  // Scale the original window's size down such that it fits within the bounds
  // of the DeskPreviewView.
  const aura::Window* window = item->GetWindow();
  DCHECK(window);
  const float root_height = window->GetRootWindow()->bounds().height();
  const float scale_factor = DeskPreviewView::GetHeight() / float{root_height};
  const gfx::SizeF window_size(window->bounds().size());
  gfx::SizeF scaled_size = gfx::ScaleSize(window_size, scale_factor);
  // Add the margins overview mode adds around the window's contents.
  scaled_size.Enlarge(2 * kWindowMargin, 2 * kWindowMargin + kHeaderHeightDp);
  return scaled_size;
}

float GetManhattanDistanceX(float point_x, const gfx::RectF& rect) {
  return std::max(rect.x() - point_x, point_x - rect.right());
}

float GetManhattanDistanceY(float point_y, const gfx::RectF& rect) {
  return std::max(rect.y() - point_y, point_y - rect.bottom());
}

bool GetVirtualDesksBarEnabled(OverviewItem* item) {
  return desks_util::ShouldDesksBarBeCreated() &&
         item->overview_grid()->IsDesksBarViewActive();
}

}  // namespace

OverviewWindowDragController::OverviewWindowDragController(
    OverviewSession* overview_session,
    OverviewItem* item,
    bool allow_drag_to_close)
    : overview_session_(overview_session),
      split_view_controller_(Shell::Get()->split_view_controller()),
      item_(item),
      on_desks_bar_item_size_(GetItemSizeWhenOnDesksBar(item)),
      display_count_(Shell::GetAllRootWindows().size()),
      should_allow_drag_to_close_(allow_drag_to_close),
      should_allow_split_view_(ShouldAllowSplitView()),
      virtual_desks_bar_enabled_(GetVirtualDesksBarEnabled(item)) {}

OverviewWindowDragController::~OverviewWindowDragController() = default;

void OverviewWindowDragController::InitiateDrag(
    const gfx::PointF& location_in_screen) {
  initial_event_location_ = location_in_screen;
  initial_centerpoint_ = item_->target_bounds().CenterPoint();
  original_opacity_ = item_->GetOpacity();
  if (should_allow_split_view_) {
    started_in_snap_region_ =
        GetSnapPosition(location_in_screen) != SplitViewController::NONE;
  }
  current_drag_behavior_ = DragBehavior::kUndefined;
  Shell::Get()->overview_controller()->PauseOcclusionTracker();
  DCHECK(!presentation_time_recorder_);

  presentation_time_recorder_ = CreatePresentationTimeHistogramRecorder(
      item_->root_window()->layer()->GetCompositor(),
      kOverviewWindowDragHistogram, kOverviewWindowDragMaxLatencyHistogram);
}

void OverviewWindowDragController::Drag(const gfx::PointF& location_in_screen) {
  if (!did_move_) {
    gfx::Vector2dF distance = location_in_screen - initial_event_location_;
    // Do not start dragging if the distance from |location_in_screen| to
    // |initial_event_location_| is not greater than |kMinimumDragDistanceDp|.
    if (std::abs(distance.x()) < kMinimumDragDistanceDp &&
        std::abs(distance.y()) < kMinimumDragDistanceDp) {
      return;
    }

    if (should_allow_drag_to_close_ &&
        (std::abs(distance.x()) < std::abs(distance.y()))) {
      StartDragToCloseMode();
    } else if (should_allow_split_view_ || virtual_desks_bar_enabled_) {
      StartNormalDragMode(location_in_screen);
    } else {
      return;
    }
  }

  if (current_drag_behavior_ == DragBehavior::kDragToClose)
    ContinueDragToClose(location_in_screen);
  else if (current_drag_behavior_ == DragBehavior::kNormalDrag)
    ContinueNormalDrag(location_in_screen);

  if (presentation_time_recorder_)
    presentation_time_recorder_->RequestNext();
}

OverviewWindowDragController::DragResult
OverviewWindowDragController::CompleteDrag(
    const gfx::PointF& location_in_screen) {
  DragResult result = DragResult::kNeverDisambiguated;
  switch (current_drag_behavior_) {
    case DragBehavior::kNoDrag:
      NOTREACHED();
      break;

    case DragBehavior::kUndefined:
      ActivateDraggedWindow();
      break;

    case DragBehavior::kNormalDrag:
      result = CompleteNormalDrag(location_in_screen);
      break;

    case DragBehavior::kDragToClose:
      result = CompleteDragToClose(location_in_screen);
      break;
  }

  did_move_ = false;
  item_ = nullptr;
  current_drag_behavior_ = DragBehavior::kNoDrag;
  UnpauseOcclusionTracker();
  presentation_time_recorder_.reset();
  return result;
}

void OverviewWindowDragController::StartNormalDragMode(
    const gfx::PointF& location_in_screen) {
  DCHECK(should_allow_split_view_ || virtual_desks_bar_enabled_);

  did_move_ = true;
  current_drag_behavior_ = DragBehavior::kNormalDrag;
  if (AreMultiDisplayOverviewAndSplitViewEnabled()) {
    Shell::Get()->mouse_cursor_filter()->ShowSharedEdgeIndicator(
        item_->root_window());
  }
  item_->ScaleUpSelectedItem(
      OVERVIEW_ANIMATION_LAYOUT_OVERVIEW_ITEMS_IN_OVERVIEW);
  original_scaled_size_ = item_->target_bounds().size();
  item_->overview_grid()->AddDropTargetForDraggingFromOverview(item_);

  if (should_allow_split_view_) {
    overview_session_->SetSplitViewDragIndicatorsIndicatorState(
        CanSnapInSplitview(item_->GetWindow()) ? IndicatorState::kDragArea
                                               : IndicatorState::kCannotSnap,
        gfx::ToRoundedPoint(location_in_screen));

    // Update the split view divider bar status if necessary. If splitview is
    // active when dragging the overview window, the split divider bar should be
    // placed below the dragged window during dragging.
    split_view_controller_->OnWindowDragStarted(item_->GetWindow());
  }

  if (virtual_desks_bar_enabled_) {
    // Calculate the item bounds minus the header and margins (which are
    // invisible). Use this for the shrink bounds so that the item starts
    // shrinking when the visible top-edge of the item aligns with the
    // bottom-edge of the desks bar (may be different edges if we are dragging
    // from different directions).
    gfx::SizeF item_no_header_size = original_scaled_size_;
    item_no_header_size.Enlarge(float{-kWindowMargin * 2},
                                float{-kWindowMargin * 2 - kHeaderHeightDp});

    // Calculate cached values for usage during drag.
    desks_bar_bounds_ = gfx::RectF(
        item_->overview_grid()->desks_bar_view()->GetBoundsInScreen());
    shrink_bounds_ = desks_bar_bounds_;
    shrink_bounds_.Inset(-item_no_header_size.width() / 2,
                         -item_no_header_size.height() / 2);
    shrink_region_distance_ =
        desks_bar_bounds_.origin() - shrink_bounds_.origin();
  }
}

OverviewWindowDragController::DragResult OverviewWindowDragController::Fling(
    const gfx::PointF& location_in_screen,
    float velocity_x,
    float velocity_y) {
  if (current_drag_behavior_ == DragBehavior::kDragToClose ||
      current_drag_behavior_ == DragBehavior::kUndefined) {
    if (std::abs(velocity_y) > kFlingToCloseVelocityThreshold) {
      item_->AnimateAndCloseWindow(
          (location_in_screen - initial_event_location_).y() < 0);
      did_move_ = false;
      item_ = nullptr;
      current_drag_behavior_ = DragBehavior::kNoDrag;
      UnpauseOcclusionTracker();
      return DragResult::kSuccessfulDragToClose;
    }
  }

  // If the fling velocity was not high enough, or flings should be ignored,
  // treat it as a scroll end event.
  return CompleteDrag(location_in_screen);
}

void OverviewWindowDragController::ActivateDraggedWindow() {
  // If no drag was initiated (e.g., a click/tap on the overview window),
  // activate the window. If the split view is active and has a left window,
  // snap the current window to right. If the split view is active and has a
  // right window, snap the current window to left. If split view is active
  // and the selected window cannot be snapped, exit splitview and activate
  // the selected window, and also exit the overview.
  SplitViewState split_state = split_view_controller_->state();
  if (!should_allow_split_view_ || split_state == SplitViewState::kNoSnap) {
    overview_session_->SelectWindow(item_);
  } else if (CanSnapInSplitview(item_->GetWindow())) {
    SnapWindow(split_state == SplitViewState::kLeftSnapped
                   ? SplitViewController::RIGHT
                   : SplitViewController::LEFT);
  } else {
    split_view_controller_->EndSplitView();
    overview_session_->SelectWindow(item_);
    ShowAppCannotSnapToast();
  }
  current_drag_behavior_ = DragBehavior::kNoDrag;
  UnpauseOcclusionTracker();
}

void OverviewWindowDragController::ResetGesture() {
  if (current_drag_behavior_ == DragBehavior::kNormalDrag) {
    DCHECK(item_->overview_grid()->drop_target_widget());

    if (AreMultiDisplayOverviewAndSplitViewEnabled()) {
      Shell::Get()->mouse_cursor_filter()->HideSharedEdgeIndicator();
      item_->DestroyPhantomsForDragging();
    }
    item_->overview_grid()->RemoveDropTarget();
    if (should_allow_split_view_) {
      overview_session_->SetSplitViewDragIndicatorsIndicatorState(
          IndicatorState::kNone, gfx::Point());
    }
  }
  overview_session_->PositionWindows(/*animate=*/true);
  // This function gets called after a long press release, which bypasses
  // CompleteDrag but stops dragging as well, so reset |item_|.
  item_ = nullptr;
  current_drag_behavior_ = DragBehavior::kNoDrag;
  UnpauseOcclusionTracker();
}

void OverviewWindowDragController::ResetOverviewSession() {
  overview_session_ = nullptr;
}

void OverviewWindowDragController::StartDragToCloseMode() {
  DCHECK(should_allow_drag_to_close_);

  did_move_ = true;
  current_drag_behavior_ = DragBehavior::kDragToClose;
  overview_session_->GetGridWithRootWindow(item_->root_window())
      ->StartNudge(item_);
}

void OverviewWindowDragController::ContinueDragToClose(
    const gfx::PointF& location_in_screen) {
  DCHECK_EQ(current_drag_behavior_, DragBehavior::kDragToClose);

  // Update the dragged |item_|'s bounds accordingly. The distance from the new
  // location to the new centerpoint should be the same it was initially.
  gfx::RectF bounds(item_->target_bounds());
  const gfx::PointF centerpoint =
      location_in_screen - (initial_event_location_ - initial_centerpoint_);

  // If the drag location intersects with the desk bar, then we should cancel
  // the drag-to-close mode and start the normal drag mode.
  if (virtual_desks_bar_enabled_ &&
      item_->overview_grid()->IntersectsWithDesksBar(
          gfx::ToRoundedPoint(location_in_screen),
          /*update_desks_bar_drag_details=*/false, /*for_drop=*/false)) {
    item_->SetOpacity(original_opacity_);
    StartNormalDragMode(location_in_screen);
    ContinueNormalDrag(location_in_screen);
    return;
  }

  // Update |item_|'s opacity based on its distance. |item_|'s x coordinate
  // should not change while in drag to close state.
  float val = std::abs(location_in_screen.y() - initial_event_location_.y()) /
              kDragToCloseDistanceThresholdDp;
  overview_session_->GetGridWithRootWindow(item_->root_window())
      ->UpdateNudge(item_, val);
  val = base::ClampToRange(val, 0.f, 1.f);
  float opacity = original_opacity_;
  if (opacity > kItemMinOpacity)
    opacity = original_opacity_ - val * (original_opacity_ - kItemMinOpacity);
  item_->SetOpacity(opacity);

  // When dragging to close, only update the y component.
  bounds.set_y(centerpoint.y() - bounds.height() / 2.f);
  item_->SetBounds(bounds, OVERVIEW_ANIMATION_NONE);
}

OverviewWindowDragController::DragResult
OverviewWindowDragController::CompleteDragToClose(
    const gfx::PointF& location_in_screen) {
  DCHECK_EQ(current_drag_behavior_, DragBehavior::kDragToClose);

  // Close the window if it has been dragged enough, otherwise reposition it and
  // set its opacity back to its original value.
  overview_session_->GetGridWithRootWindow(item_->root_window())->EndNudge();
  const float y_distance = (location_in_screen - initial_event_location_).y();
  if (std::abs(y_distance) > kDragToCloseDistanceThresholdDp) {
    item_->AnimateAndCloseWindow(/*up=*/y_distance < 0);
    return DragResult::kSuccessfulDragToClose;
  }

  item_->SetOpacity(original_opacity_);
  overview_session_->PositionWindows(/*animate=*/true);
  return DragResult::kCanceledDragToClose;
}

void OverviewWindowDragController::ContinueNormalDrag(
    const gfx::PointF& location_in_screen) {
  DCHECK_EQ(current_drag_behavior_, DragBehavior::kNormalDrag);

  // Update the dragged |item_|'s bounds accordingly. The distance from the new
  // location to the new centerpoint should be the same it was initially unless
  // the item is over the DeskBarView, in which case we scale it down and center
  // it around the drag location.
  gfx::RectF bounds(item_->target_bounds());
  gfx::PointF centerpoint =
      location_in_screen - (initial_event_location_ - initial_centerpoint_);

  // If virtual desks is enabled, we want to gradually shrink the dragged item
  // as it gets closer to get dropped into a desk mini view.
  if (virtual_desks_bar_enabled_) {
    // TODO(sammiequon): There is a slight jump especially if we drag from the
    // corner of a larger overview item, but this is necessary for the time
    // being to prevent jumps from happening while shrinking. Investigate if we
    // can satisfy all cases.
    centerpoint = location_in_screen;
    // To make the dragged window contents appear centered around the drag
    // location, we need to take into account the margins applied on the
    // target bounds, and offset up the centerpoint by half that amount, so
    // that the transformed bounds of the window contents move up to be
    // centered around the cursor.
    centerpoint.Offset(0, (-kWindowMargin - kHeaderHeightDp) / 2);

    if (shrink_bounds_.Contains(location_in_screen)) {
      // Update the mini views borders by checking if |location_in_screen|
      // intersects.
      item_->overview_grid()->IntersectsWithDesksBar(
          gfx::ToRoundedPoint(location_in_screen),
          /*update_desks_bar_drag_details=*/true, /*for_drop=*/false);

      float value = 0.f;
      if (centerpoint.y() < desks_bar_bounds_.y() ||
          centerpoint.y() > desks_bar_bounds_.y()) {
        // Coming vertically, this is the main use case. This is a ratio of the
        // distance from |centerpoint| to the closest edge of |desk_bar_bounds|
        // to the distance from |shrink_bounds| to |desk_bar_bounds|.
        value = GetManhattanDistanceY(centerpoint.y(), desks_bar_bounds_) /
                shrink_region_distance_.y();
      } else if (centerpoint.x() < desks_bar_bounds_.x() ||
                 centerpoint.x() > desks_bar_bounds_.right()) {
        // Coming horizontally, this only happens if we are in landscape split
        // view and someone drags an item to the other half, then up, then into
        // the desks bar. Works same as vertically except using x-coordinates.
        value = GetManhattanDistanceX(centerpoint.x(), desks_bar_bounds_) /
                shrink_region_distance_.x();
      }
      value = base::ClampToRange(value, 0.f, 1.f);
      const gfx::SizeF size_value = gfx::Tween::SizeFValueBetween(
          1.f - value, original_scaled_size_, on_desks_bar_item_size_);
      bounds.set_size(size_value);
    } else {
      bounds.set_size(original_scaled_size_);
    }
  }

  if (should_allow_split_view_)
    UpdateDragIndicatorsAndOverviewGrid(location_in_screen);
  item_->overview_grid()->UpdateDropTargetBackgroundVisibility(
      item_, location_in_screen);

  bounds.set_x(centerpoint.x() - bounds.width() / 2.f);
  bounds.set_y(centerpoint.y() - bounds.height() / 2.f);
  item_->SetBounds(bounds, OVERVIEW_ANIMATION_NONE);
  if (AreMultiDisplayOverviewAndSplitViewEnabled() && display_count_ > 1u)
    item_->UpdatePhantomsForDragging(location_in_screen);
}

OverviewWindowDragController::DragResult
OverviewWindowDragController::CompleteNormalDrag(
    const gfx::PointF& location_in_screen) {
  DCHECK_EQ(current_drag_behavior_, DragBehavior::kNormalDrag);
  DCHECK(item_->overview_grid()->drop_target_widget());
  if (AreMultiDisplayOverviewAndSplitViewEnabled()) {
    Shell::Get()->mouse_cursor_filter()->HideSharedEdgeIndicator();
    item_->DestroyPhantomsForDragging();
  }
  item_->overview_grid()->RemoveDropTarget();

  const gfx::Point rounded_screen_point =
      gfx::ToRoundedPoint(location_in_screen);
  if (should_allow_split_view_) {
    // Update the split view divider bar stuatus if necessary. The divider bar
    // should be placed above the dragged window after drag ends. Note here the
    // passed parameters |snap_position_| and |location_in_screen| won't be used
    // in this function for this case, but they are passed in as placeholders.
    split_view_controller_->OnWindowDragEnded(
        item_->GetWindow(), snap_position_, rounded_screen_point);

    // Update window grid bounds and |snap_position_| in case the screen
    // orientation was changed.
    UpdateDragIndicatorsAndOverviewGrid(location_in_screen);
    overview_session_->SetSplitViewDragIndicatorsIndicatorState(
        IndicatorState::kNone, gfx::Point());
  }

  // Attempt to move a window to a different desk.
  if (virtual_desks_bar_enabled_) {
    item_->SetOpacity(original_opacity_);

    if (item_->overview_grid()->MaybeDropItemOnDeskMiniView(
            rounded_screen_point, item_)) {
      // Window was successfully moved to another desk, and |item_| was
      // removed from the grid. It may never be accessed after this.
      item_ = nullptr;
      overview_session_->PositionWindows(/*animate=*/true);
      return DragResult::kSuccessfulDragToDesk;
    }
  }

  // Attempt to snap a window if SplitView is enabled.
  DCHECK(item_);
  if (should_allow_split_view_) {
    // If the window was dragged around but should not be snapped, move it
    // back to overview window grid.
    if (!ShouldUpdateDragIndicatorsOrSnap(location_in_screen) ||
        snap_position_ == SplitViewController::NONE) {
      item_->set_should_restack_on_animation_end(true);
      overview_session_->PositionWindows(/*animate=*/true);
      return DragResult::kCanceledDragToSnap;
    }

    SnapWindow(snap_position_);
    return DragResult::kSuccessfulDragToSnap;
  }

  item_->set_should_restack_on_animation_end(true);
  overview_session_->PositionWindows(/*animate=*/true);
  return DragResult::kNeverDisambiguated;
}

void OverviewWindowDragController::UpdateDragIndicatorsAndOverviewGrid(
    const gfx::PointF& location_in_screen) {
  DCHECK(should_allow_split_view_);
  if (!ShouldUpdateDragIndicatorsOrSnap(location_in_screen))
    return;

  IndicatorState indicator_state;
  if (CanSnapInSplitview(item_->GetWindow())) {
    snap_position_ = GetSnapPosition(location_in_screen);
    switch (snap_position_) {
      case SplitViewController::NONE:
        indicator_state = IndicatorState::kDragArea;
        break;
      case SplitViewController::LEFT:
        indicator_state = IndicatorState::kPreviewAreaLeft;
        break;
      case SplitViewController::RIGHT:
        indicator_state = IndicatorState::kPreviewAreaRight;
        break;
    }
  } else {
    snap_position_ = SplitViewController::NONE;
    indicator_state = IndicatorState::kCannotSnap;
  }
  item_->overview_grid()->RearrangeDuringDrag(item_->GetWindow(),
                                              indicator_state);
  overview_session_->SetSplitViewDragIndicatorsIndicatorState(indicator_state,
                                                              gfx::Point());
}

bool OverviewWindowDragController::ShouldUpdateDragIndicatorsOrSnap(
    const gfx::PointF& event_location) {
  auto snap_position = GetSnapPosition(event_location);
  const bool inverted = !IsCurrentScreenOrientationPrimary();
  // Note: in some orientations SplitViewController::LEFT is not physically on
  // the left/top.
  const bool on_the_left_or_top =
      (!inverted && snap_position == SplitViewController::LEFT) ||
      (inverted && snap_position == SplitViewController::RIGHT);

  // Snap the window if it is less than |kDistanceFromEdgeDp| from the edge.
  const bool landscape = IsCurrentScreenOrientationLandscape();
  gfx::Rect area(
      screen_util::GetDisplayWorkAreaBoundsInParent(item_->GetWindow()));
  ::wm::ConvertRectToScreen(item_->GetWindow()->GetRootWindow(), &area);
  area.Inset(kDistanceFromEdgeDp, kDistanceFromEdgeDp);
  const gfx::Point event_location_i = gfx::ToRoundedPoint(event_location);
  if ((landscape && (event_location_i.x() < area.x() ||
                     event_location_i.x() > area.right())) ||
      (!landscape && (event_location_i.y() < area.y() ||
                      event_location_i.y() > area.bottom()))) {
    return true;
  }

  // The drag indicators can update or the item can snap even if the drag events
  // are in the snap region, if the event has travelled past the threshold in
  // the direction of the attempted snap region.
  const gfx::Vector2dF distance = event_location - initial_event_location_;
  // Check the x-axis distance for landscape, y-axis distance for portrait.
  const float distance_scalar = landscape ? distance.x() : distance.y();

  // If not started in a snap region, snap if the item has been dragged
  // |kMinimumDragDistanceDp|. This prevents accidental snaps.
  if (!started_in_snap_region_ &&
      std::abs(distance_scalar) > kMinimumDragToSnapDistanceDp) {
    return true;
  }

  if (snap_position == SplitViewController::NONE) {
    // If the event started in a snap region, but has since moved out set
    // |started_in_snap_region_| to false. |event_location| is guarenteed to not
    // be in a snap region so that the drag indicators are shown correctly and
    // the snap mechanism works normally for the rest of the drag.
    started_in_snap_region_ = false;
    return true;
  }

  // If the snap region is physically on the left/top side of the device, check
  // that |distance_scalar| is less than
  // -|kMinimumDragDistanceAlreadyInSnapRegionDp|. If the snap region is
  // physically on the right/bottom side of the device, check that
  // |distance_scalar| is greater than
  // |kMinimumDragDistanceAlreadyInSnapRegionDp|.
  return on_the_left_or_top
             ? distance_scalar <= -kMinimumDragDistanceAlreadyInSnapRegionDp
             : distance_scalar >= kMinimumDragDistanceAlreadyInSnapRegionDp;
}

SplitViewController::SnapPosition OverviewWindowDragController::GetSnapPosition(
    const gfx::PointF& location_in_screen) const {
  DCHECK(item_);
  DCHECK(should_allow_split_view_);
  gfx::Rect area(
      screen_util::GetDisplayWorkAreaBoundsInParent(item_->GetWindow()));
  ::wm::ConvertRectToScreen(item_->GetWindow()->GetRootWindow(), &area);

  const bool is_landscape = IsCurrentScreenOrientationLandscape();
  const bool is_primary = IsCurrentScreenOrientationPrimary();

  // If split view mode is active at the moment, and dragging an overview window
  // to snap it to a position that already has a snapped window in place, we
  // should show the preview window as soon as the window past the split divider
  // bar.
  if (split_view_controller_->InSplitViewMode()) {
    const int position = gfx::ToRoundedInt(
        is_landscape ? location_in_screen.x() : location_in_screen.y());
    SplitViewController::SnapPosition default_snap_position =
        split_view_controller_->default_snap_position();
    // If we're trying to snap to a position that already has a snapped window:
    const bool re_snap =
        is_primary == (position < split_view_controller_->divider_position() ==
                       (default_snap_position == SplitViewController::LEFT));
    if (re_snap)
      return default_snap_position;
  }

  if (is_landscape) {
    // The window can be snapped if it reaches close enough to the screen
    // edge of the screen (on primary axis). The edge insets are a fixed ratio
    // of the screen plus some padding. This matches the drag indicators ui.
    const int screen_edge_inset_for_drag =
        area.width() * kHighlightScreenPrimaryAxisRatio +
        kHighlightScreenEdgePaddingDp;
    area.Inset(screen_edge_inset_for_drag, 0);
    if (location_in_screen.x() <= area.x()) {
      return is_primary ? SplitViewController::LEFT
                        : SplitViewController::RIGHT;
    }
    if (location_in_screen.x() >= area.right() - 1) {
      return is_primary ? SplitViewController::RIGHT
                        : SplitViewController::LEFT;
    }
    return SplitViewController::NONE;
  } else {
    const int screen_edge_inset_for_drag =
        area.height() * kHighlightScreenPrimaryAxisRatio +
        kHighlightScreenEdgePaddingDp;
    area.Inset(0, screen_edge_inset_for_drag);
    if (location_in_screen.y() <= area.y()) {
      return is_primary ? SplitViewController::LEFT
                        : SplitViewController::RIGHT;
    }
    if (location_in_screen.y() >= area.bottom() - 1) {
      return is_primary ? SplitViewController::RIGHT
                        : SplitViewController::LEFT;
    }
    return SplitViewController::NONE;
  }
}

gfx::Rect OverviewWindowDragController::GetGridBounds(
    SplitViewController::SnapPosition snap_position) {
  aura::Window* pending_snapped_window = item_->GetWindow();
  switch (snap_position) {
    case SplitViewController::NONE:
      return gfx::Rect(
          screen_util::GetDisplayWorkAreaBoundsInParentForActiveDeskContainer(
              pending_snapped_window));
    case SplitViewController::LEFT:
      return split_view_controller_->GetSnappedWindowBoundsInScreen(
          pending_snapped_window, SplitViewController::RIGHT);
    case SplitViewController::RIGHT:
      return split_view_controller_->GetSnappedWindowBoundsInScreen(
          pending_snapped_window, SplitViewController::LEFT);
  }

  NOTREACHED();
  return gfx::Rect();
}

void OverviewWindowDragController::SnapWindow(
    SplitViewController::SnapPosition snap_position) {
  DCHECK_NE(snap_position, SplitViewController::NONE);

  // |item_| will be deleted after SplitViewController::SnapWindow().
  split_view_controller_->SnapWindow(item_->GetWindow(), snap_position,
                                     /*use_divider_spawn_animation=*/true);
  item_ = nullptr;
}

}  // namespace ash
