// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/render_widget_host_input_event_router.h"

#include <algorithm>
#include <deque>
#include <vector>

#include "base/debug/crash_logging.h"
#include "base/debug/dump_without_crashing.h"
#include "base/metrics/histogram_macros.h"
#include "base/stl_util.h"
#include "base/strings/stringprintf.h"
#include "components/viz/common/features.h"
#include "components/viz/common/hit_test/hit_test_region_list.h"
#include "components/viz/common/quads/surface_draw_quad.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "components/viz/service/surfaces/surface_manager.h"
#include "content/browser/compositor/surface_utils.h"
#include "content/browser/frame_host/render_widget_host_view_guest.h"
#include "content/browser/renderer_host/cursor_manager.h"
#include "content/browser/renderer_host/input/touch_emulator.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/browser/renderer_host/render_widget_host_view_child_frame.h"
#include "content/common/frame_messages.h"
#include "content/public/browser/render_widget_host_iterator.h"
#include "third_party/blink/public/platform/web_input_event.h"
#include "ui/base/layout.h"
#include "ui/gfx/geometry/dip_util.h"

#include "app/vivaldi_apptools.h"
#include "ui/content/vivaldi_event_hooks.h"

namespace {

// Transforms WebTouchEvent touch positions from the root view coordinate
// space to the target view coordinate space.
void TransformEventTouchPositions(blink::WebTouchEvent* event,
                                  const gfx::Transform& transform) {
  for (unsigned i = 0; i < event->touches_length; ++i) {
    gfx::PointF point(event->touches[i].PositionInWidget());
    transform.TransformPoint(&point);
    event->touches[i].SetPositionInWidget(point);
  }
}

bool IsMouseButtonDown(const blink::WebMouseEvent& event) {
  constexpr int mouse_button_modifiers =
      blink::WebInputEvent::kLeftButtonDown |
      blink::WebInputEvent::kMiddleButtonDown |
      blink::WebInputEvent::kRightButtonDown |
      blink::WebInputEvent::kBackButtonDown |
      blink::WebInputEvent::kForwardButtonDown;

  return event.GetModifiers() & mouse_button_modifiers;
}

}  // anonymous namespace

namespace content {

// Helper method also used from hit_test_debug_key_event_observer.cc
viz::HitTestQuery* GetHitTestQuery(
    viz::HostFrameSinkManager* host_frame_sink_manager,
    const viz::FrameSinkId& frame_sink_id) {
  if (!frame_sink_id.is_valid())
    return nullptr;
  const auto& display_hit_test_query_map =
      host_frame_sink_manager->display_hit_test_query();
  const auto iter = display_hit_test_query_map.find(frame_sink_id);
  if (iter == display_hit_test_query_map.end())
    return nullptr;
  return iter->second.get();
}

// A class to implement a queue for tracking outbound TouchEvents, and making
// sure that their acks are returned to the appropriate root view in order.
// This is important to ensure proper operation of the GestureProvider.
// Some challenges include:
// * differentiating between native and emulated TouchEvents, as the latter ack
//   to the TouchEmulator's GestureProvider,
// * making sure all events from destroyed renderers are acked properly, and
//   without delaying acks from other renderers, and
// * making sure events are only acked if the root_view (at the time of the
//   out-bound event) is still valid.
// Some of this logic, e.g. the last item above, is shared with
// RenderWidgetHostViewBase.
class TouchEventAckQueue {
 public:
  enum class TouchEventAckStatus { TouchEventNotAcked, TouchEventAcked };
  enum class TouchEventSource { SystemTouchEvent, EmulatedTouchEvent };
  struct AckData {
    TouchEventWithLatencyInfo touch_event;
    RenderWidgetHostViewBase* target_view;
    RenderWidgetHostViewBase* root_view;
    TouchEventSource touch_event_source;
    TouchEventAckStatus touch_event_ack_status;
    InputEventAckState ack_result;
  };

  explicit TouchEventAckQueue(RenderWidgetHostInputEventRouter* client)
      : client_(client) {
    DCHECK(client_);
  }

  void Add(const TouchEventWithLatencyInfo& touch_event,
           RenderWidgetHostViewBase* target_view,
           RenderWidgetHostViewBase* root_view,
           TouchEventSource touch_event_source,
           TouchEventAckStatus touch_event_ack_status,
           InputEventAckState ack_result);

  void Add(const TouchEventWithLatencyInfo& touch_event,
           RenderWidgetHostViewBase* target_view,
           RenderWidgetHostViewBase* root_view,
           TouchEventSource touch_event_source);

  void MarkAcked(const TouchEventWithLatencyInfo& touch_event,
                 InputEventAckState ack_result,
                 RenderWidgetHostViewBase* target_view);

  void UpdateQueueAfterTargetDestroyed(RenderWidgetHostViewBase* target_view);

  std::vector<AckData> GetQueueEntriesForRootView(
      RenderWidgetHostViewBase* root_view) const;

  size_t length_for_testing() { return ack_queue_.size(); }

 private:
  void ProcessAckedTouchEvents();
  void ReportTouchEventAckQueueUmaStats();

  std::deque<AckData> ack_queue_;
  RenderWidgetHostInputEventRouter* client_;
};

void TouchEventAckQueue::Add(const TouchEventWithLatencyInfo& touch_event,
                             RenderWidgetHostViewBase* target_view,
                             RenderWidgetHostViewBase* root_view,
                             TouchEventSource touch_event_source,
                             TouchEventAckStatus touch_event_ack_status,
                             InputEventAckState ack_result) {
  AckData data = {touch_event,
                  target_view,
                  root_view,
                  touch_event_source,
                  touch_event_ack_status,
                  ack_result};
  ack_queue_.push_back(data);
  if (touch_event_ack_status == TouchEventAckStatus::TouchEventAcked)
    ProcessAckedTouchEvents();
  ReportTouchEventAckQueueUmaStats();
}

void TouchEventAckQueue::Add(const TouchEventWithLatencyInfo& touch_event,
                             RenderWidgetHostViewBase* target_view,
                             RenderWidgetHostViewBase* root_view,
                             TouchEventSource touch_event_source) {
  Add(touch_event, target_view, root_view, touch_event_source,
      TouchEventAckStatus::TouchEventNotAcked, INPUT_EVENT_ACK_STATE_UNKNOWN);
}

void TouchEventAckQueue::MarkAcked(const TouchEventWithLatencyInfo& touch_event,
                                   InputEventAckState ack_result,
                                   RenderWidgetHostViewBase* target_view) {
  auto it = find_if(ack_queue_.begin(), ack_queue_.end(),
                    [touch_event](AckData data) {
                      return data.touch_event.event.unique_touch_event_id ==
                             touch_event.event.unique_touch_event_id;
                    });
  if (it == ack_queue_.end()) {
    // If the touch-event was sent directly to the view without going through
    // RenderWidgetHostInputEventRouter, as is the case with AndroidWebView,
    // then we must ack it directly.
    DCHECK(target_view);
    target_view->ProcessAckedTouchEvent(touch_event, ack_result);
    return;
  }

  DCHECK(it->touch_event_ack_status != TouchEventAckStatus::TouchEventAcked);
  DCHECK(target_view && target_view == it->target_view);
  it->touch_event = touch_event;
  it->touch_event_ack_status = TouchEventAckStatus::TouchEventAcked;
  it->ack_result = ack_result;
  ProcessAckedTouchEvents();
}

void TouchEventAckQueue::ProcessAckedTouchEvents() {
  if (ack_queue_.empty())
    return;

  TouchEmulator* touch_emulator =
      client_->has_touch_emulator() ? client_->GetTouchEmulator() : nullptr;
  while (!ack_queue_.empty() && ack_queue_.front().touch_event_ack_status ==
                                    TouchEventAckStatus::TouchEventAcked) {
    TouchEventAckQueue::AckData ack_data = ack_queue_.front();
    ack_queue_.pop_front();

    if ((!touch_emulator ||
         !touch_emulator->HandleTouchEventAck(ack_data.touch_event.event,
                                              ack_data.ack_result)) &&
        (client_->IsViewInMap(ack_data.root_view) ||
         client_->ViewMapIsEmpty())) {
      // Forward acked event and result to the root view associated with the
      // event. The view map is only empty for AndroidWebView.
      ack_data.root_view->ProcessAckedTouchEvent(ack_data.touch_event,
                                                 ack_data.ack_result);
    }
  }
}

void TouchEventAckQueue::ReportTouchEventAckQueueUmaStats() {
  size_t count = ack_queue_.size();
  UMA_HISTOGRAM_COUNTS_10000("Event.FrameEventRouting.TouchEventAckQueueSize",
                             count);
  // TODO(wjmaclean): is it worth also recording how many different renderers
  // are waiting on touch event acks at the time of reporting?
}

void TouchEventAckQueue::UpdateQueueAfterTargetDestroyed(
    RenderWidgetHostViewBase* target_view) {
  // If a queue entry's root view is being destroyed, just delete it.
  base::EraseIf(ack_queue_, [target_view](AckData data) {
    return data.root_view == target_view;
  });

  // Otherwise, mark its status accordingly.
  for_each(ack_queue_.begin(), ack_queue_.end(), [target_view](AckData& data) {
    if (data.target_view == target_view) {
      data.touch_event_ack_status = TouchEventAckStatus::TouchEventAcked;
      data.ack_result = INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS;
    }
  });

  ProcessAckedTouchEvents();
}

std::vector<TouchEventAckQueue::AckData>
TouchEventAckQueue::GetQueueEntriesForRootView(
    RenderWidgetHostViewBase* root_view) const {
  std::vector<TouchEventAckQueue::AckData> acks;
  for (const auto& data : ack_queue_) {
    if (data.root_view == root_view)
      acks.push_back(data);
  }
  return acks;
}

RenderWidgetHostInputEventRouter::TouchscreenPinchState::TouchscreenPinchState()
    : state_(PinchState::NONE) {}

bool RenderWidgetHostInputEventRouter::TouchscreenPinchState::IsInPinch()
    const {
  switch (state_) {
    case PinchState::NONE:
    case PinchState::EXISTING_BUBBLING_TO_ROOT:
      return false;
    case PinchState::PINCH_WITH_ROOT_GESTURE_TARGET:
    case PinchState::PINCH_WHILE_BUBBLING_TO_ROOT:
    case PinchState::PINCH_DURING_CHILD_GESTURE:
      return true;
  }
}

bool RenderWidgetHostInputEventRouter::TouchscreenPinchState::
    NeedsWrappingScrollSequence() const {
  switch (state_) {
    case PinchState::NONE:
    case PinchState::PINCH_DURING_CHILD_GESTURE:
      return true;
    case PinchState::EXISTING_BUBBLING_TO_ROOT:
    case PinchState::PINCH_WITH_ROOT_GESTURE_TARGET:
    case PinchState::PINCH_WHILE_BUBBLING_TO_ROOT:
      return false;
  }
}

void RenderWidgetHostInputEventRouter::TouchscreenPinchState::
    DidStartBubblingToRoot() {
  switch (state_) {
    case PinchState::NONE:
      state_ = PinchState::EXISTING_BUBBLING_TO_ROOT;
      break;
    case PinchState::PINCH_DURING_CHILD_GESTURE:
      state_ = PinchState::PINCH_WHILE_BUBBLING_TO_ROOT;
      break;
    case PinchState::EXISTING_BUBBLING_TO_ROOT:
    case PinchState::PINCH_WITH_ROOT_GESTURE_TARGET:
    case PinchState::PINCH_WHILE_BUBBLING_TO_ROOT:
      NOTREACHED();
  }
}

void RenderWidgetHostInputEventRouter::TouchscreenPinchState::
    DidStopBubblingToRoot() {
  DCHECK_EQ(PinchState::EXISTING_BUBBLING_TO_ROOT, state_);
  state_ = PinchState::NONE;
}

void RenderWidgetHostInputEventRouter::TouchscreenPinchState::
    DidStartPinchInRoot() {
  DCHECK_EQ(PinchState::NONE, state_);
  state_ = PinchState::PINCH_WITH_ROOT_GESTURE_TARGET;
}

void RenderWidgetHostInputEventRouter::TouchscreenPinchState::
    DidStartPinchInChild() {
  switch (state_) {
    case PinchState::NONE:
      state_ = PinchState::PINCH_DURING_CHILD_GESTURE;
      break;
    case PinchState::EXISTING_BUBBLING_TO_ROOT:
      state_ = PinchState::PINCH_WHILE_BUBBLING_TO_ROOT;
      break;
    case PinchState::PINCH_WITH_ROOT_GESTURE_TARGET:
    case PinchState::PINCH_WHILE_BUBBLING_TO_ROOT:
    case PinchState::PINCH_DURING_CHILD_GESTURE:
      NOTREACHED();
  }
}

void RenderWidgetHostInputEventRouter::TouchscreenPinchState::DidStopPinch() {
  switch (state_) {
    case PinchState::PINCH_WITH_ROOT_GESTURE_TARGET:
      state_ = PinchState::NONE;
      break;
    case PinchState::PINCH_WHILE_BUBBLING_TO_ROOT:
      state_ = PinchState::EXISTING_BUBBLING_TO_ROOT;
      break;
    case PinchState::PINCH_DURING_CHILD_GESTURE:
      state_ = PinchState::NONE;
      break;
    case PinchState::NONE:
    case PinchState::EXISTING_BUBBLING_TO_ROOT:
      NOTREACHED();
  }
}

bool RenderWidgetHostInputEventRouter::HasEventsPendingDispatch() const {
  return event_targeter_->HasEventsPendingDispatch();
}

size_t RenderWidgetHostInputEventRouter::TouchEventAckQueueLengthForTesting()
    const {
  return touch_event_ack_queue_->length_for_testing();
}

size_t RenderWidgetHostInputEventRouter::RegisteredViewCountForTesting() const {
  return owner_map_.size();
}

void RenderWidgetHostInputEventRouter::IgnoreUnackedTouchEvents(
    RenderWidgetHostViewBase* root_view) {
  DCHECK(!root_view->IsRenderWidgetHostViewChildFrame());
  std::vector<TouchEventAckQueue::AckData> acks =
      touch_event_ack_queue_->GetQueueEntriesForRootView(root_view);
  std::map<RenderWidgetHostViewBase*, std::unordered_set<uint32_t>>
      events_for_target;
  for (auto& ack : acks) {
    events_for_target[ack.target_view].insert(
        ack.touch_event.event.unique_touch_event_id);
  }
  for (auto& pair : events_for_target) {
    pair.first->host()->IgnoreTouchEventAcks(pair.second);
  }
}

void RenderWidgetHostInputEventRouter::OnRenderWidgetHostViewBaseDestroyed(
    RenderWidgetHostViewBase* view) {
  // RenderWidgetHostViewBase::RemoveObserver() should only ever be called
  // in this function, except during the shutdown of this class. This prevents
  // removal of an observed view that is being tracked as an event target
  // without cleaning up dangling pointers to it.
  view->RemoveObserver(this);

  // Remove this view from the owner_map.
  for (auto entry : owner_map_) {
    if (entry.second == view) {
      owner_map_.erase(entry.first);
      // There will only be one instance of a particular view in the map.
      break;
    }
  }

  if (touch_emulator_)
    touch_emulator_->OnViewDestroyed(view);

  if (view == touch_target_) {
    touch_target_ = nullptr;
    active_touches_ = 0;
  }
  touch_event_ack_queue_->UpdateQueueAfterTargetDestroyed(view);

  if (view == wheel_target_)
    wheel_target_ = nullptr;

  // If the target that's being destroyed is in the gesture target map, we
  // replace it with nullptr so that we maintain the 1:1 correspondence between
  // map entries and the touch sequences that underly them.
  for (auto it : touchscreen_gesture_target_map_) {
    if (it.second == view)
      it.second = nullptr;
  }

  if (view == mouse_capture_target_)
    mouse_capture_target_ = nullptr;

  if (view == touchscreen_gesture_target_)
    SetTouchscreenGestureTarget(nullptr);

  if (view == touchpad_gesture_target_)
    touchpad_gesture_target_ = nullptr;

  if (view == bubbling_gesture_scroll_target_) {
    bubbling_gesture_scroll_target_ = nullptr;
    bubbling_gesture_scroll_origin_ = nullptr;
  } else if (view == bubbling_gesture_scroll_origin_) {
    bubbling_gesture_scroll_origin_ = nullptr;
  }

  if (view == last_mouse_move_target_) {
    // When a child iframe is destroyed, consider its parent to be to be the
    // most recent target, if possible. In some cases the parent might already
    // have been destroyed, in which case the last target is cleared.
    if (view != last_mouse_move_root_view_) {
      DCHECK(last_mouse_move_target_->IsRenderWidgetHostViewChildFrame());
      last_mouse_move_target_ =
          static_cast<RenderWidgetHostViewChildFrame*>(last_mouse_move_target_)
              ->GetParentView();
    } else {
      last_mouse_move_target_ = nullptr;
    }

    if (!last_mouse_move_target_ || view == last_mouse_move_root_view_)
      last_mouse_move_root_view_ = nullptr;
  }

  if (view == last_fling_start_target_)
    last_fling_start_target_ = nullptr;

  if (view == last_mouse_down_target_)
    last_mouse_down_target_ = nullptr;

  event_targeter_->ViewWillBeDestroyed(view);
}

void RenderWidgetHostInputEventRouter::ClearAllObserverRegistrations() {
  // Since we're shutting down, it's safe to call RenderWidgetHostViewBase::
  // RemoveObserver() directly here.
  for (auto entry : owner_map_)
    entry.second->RemoveObserver(this);
  owner_map_.clear();
  viz::HostFrameSinkManager* manager = GetHostFrameSinkManager();
  if (manager)
    manager->RemoveHitTestRegionObserver(this);
}

RenderWidgetHostInputEventRouter::RenderWidgetHostInputEventRouter()
    : last_mouse_move_target_(nullptr),
      last_mouse_move_root_view_(nullptr),
      last_emulated_event_root_view_(nullptr),
      last_device_scale_factor_(1.f),
      active_touches_(0),
      event_targeter_(std::make_unique<RenderWidgetTargeter>(this)),
      touch_event_ack_queue_(new TouchEventAckQueue(this)) {
  viz::HostFrameSinkManager* manager = GetHostFrameSinkManager();
  DCHECK(manager);
  manager->AddHitTestRegionObserver(this);
}

RenderWidgetHostInputEventRouter::~RenderWidgetHostInputEventRouter() {
  // We may be destroyed before some of the owners in the map, so we must
  // remove ourself from their observer lists.
  ClearAllObserverRegistrations();
}

RenderWidgetTargetResult RenderWidgetHostInputEventRouter::FindMouseEventTarget(
    RenderWidgetHostViewBase* root_view,
    const blink::WebMouseEvent& event) const {
  RenderWidgetHostViewBase* target = nullptr;
  bool needs_transform_point = true;
  bool latched_target = true;
  bool should_verify_result = false;
  // Allow devtools to route events into the root view based on the
  // browser-side inspector overlay state.
  if (route_to_root_for_devtools_)
    target = root_view;

  if (!target && root_view->IsMouseLocked()) {
    target = root_view->host()->delegate()->GetMouseLockWidget()->GetView();
  }

  gfx::PointF transformed_point;

  // Ignore mouse_capture_target_ if there are no mouse buttons currently down
  // because this is only for the purpose of dragging.
  if (!target && mouse_capture_target_ &&
      (event.GetType() == blink::WebInputEvent::kMouseUp ||
       IsMouseButtonDown(event))) {
    target = mouse_capture_target_;
    // Hit testing is skipped for MouseUp with mouse capture which is enabled by
    // the OOPIF renderer. Instead of using the coordinate transformation in the
    // browser process process, use the cached coordinates that were determined
    // by the renderer process on the previous MouseDown.
    // TODO(yigu): Currently there is a mismatch between the coordinate
    // transforms from browser process and renderer process. We need to fix it
    // so that we don't need to cache the transform from MouseDown.
    // https://crbug.com/934434.
    if (event.GetType() == blink::WebInputEvent::kMouseUp &&
        target == last_mouse_down_target_ &&
        mouse_down_pre_transformed_coordinate_ ==
            gfx::PointF(event.PositionInWidget().x,
                        event.PositionInWidget().y)) {
      transformed_point = mouse_down_post_transformed_coordinate_;
      needs_transform_point = false;
    }
  }

  if (!target) {
    latched_target = false;
    auto result =
        FindViewAtLocation(root_view, event.PositionInWidget(),
                           viz::EventSource::MOUSE, &transformed_point);
    // Due to performance concerns we do not verify mouse move events.
    should_verify_result = (event.GetType() == blink::WebInputEvent::kMouseMove)
                               ? false
                               : result.should_verify_result;
    if (event.GetType() == blink::WebInputEvent::kMouseDown) {
      mouse_down_pre_transformed_coordinate_.SetPoint(
          event.PositionInWidget().x, event.PositionInWidget().y);
    }
    if (result.should_query_view) {
      DCHECK(!should_verify_result);
      return {result.view, true, transformed_point, latched_target,
              should_verify_result};
    }
    target = result.view;
    // |transformed_point| is already transformed.
    needs_transform_point = false;
  }

  if (needs_transform_point) {
    if (!root_view->TransformPointToCoordSpaceForView(
            event.PositionInWidget(), target, &transformed_point)) {
      return {nullptr, false, base::nullopt, latched_target, false};
    }
  }
  return {target, false, transformed_point, latched_target,
          should_verify_result};
}

RenderWidgetTargetResult
RenderWidgetHostInputEventRouter::FindMouseWheelEventTarget(
    RenderWidgetHostViewBase* root_view,
    const blink::WebMouseWheelEvent& event) const {
  RenderWidgetHostViewBase* target = nullptr;
  gfx::PointF transformed_point;
  if (root_view->IsMouseLocked()) {
    target = root_view->host()->delegate()->GetMouseLockWidget()->GetView();
    if (!root_view->TransformPointToCoordSpaceForView(
            event.PositionInWidget(), target, &transformed_point)) {
      return {nullptr, false, base::nullopt, true, false};
    }
    return {target, false, transformed_point, true, false};
  }

  if (event.phase == blink::WebMouseWheelEvent::kPhaseBegan) {
    auto result =
        FindViewAtLocation(root_view, event.PositionInWidget(),
                           viz::EventSource::MOUSE, &transformed_point);
    return {result.view, result.should_query_view, transformed_point, false,
            result.should_verify_result};
  }
  // For non-begin events, the target found for the previous phaseBegan is
  // used.
  return {nullptr, false, base::nullopt, true, false};
}

RenderWidgetTargetResult RenderWidgetHostInputEventRouter::FindViewAtLocation(
    RenderWidgetHostViewBase* root_view,
    const gfx::PointF& point,
    viz::EventSource source,
    gfx::PointF* transformed_point) const {
  // Short circuit if owner_map has only one RenderWidgetHostView, no need for
  // hit testing.
  if (owner_map_.size() <= 1) {
    *transformed_point = point;
    return {root_view, false, *transformed_point, false, false};
  }

  viz::FrameSinkId frame_sink_id;
  bool query_renderer = false;
  bool should_verify_result = false;
  viz::HitTestQuery* query = GetHitTestQuery(GetHostFrameSinkManager(),
                                             root_view->GetRootFrameSinkId());
  if (!query) {
    *transformed_point = point;
    return {root_view, false, *transformed_point, false, false};
  }
  float device_scale_factor = root_view->GetDeviceScaleFactor();
  DCHECK_GT(device_scale_factor, 0.0f);
  gfx::PointF point_in_pixels =
      gfx::ConvertPointToPixel(device_scale_factor, point);
  viz::Target target = query->FindTargetForLocationStartingFrom(
      source, point_in_pixels, root_view->GetFrameSinkId());
  frame_sink_id = target.frame_sink_id;
  if (frame_sink_id.is_valid()) {
    *transformed_point =
        gfx::ConvertPointToDIP(device_scale_factor, target.location_in_target);
  } else {
    *transformed_point = point;
  }
  // To ensure the correctness of viz hit testing with cc generated data, we
  // verify hit test results when:
  // a) We use cc generated data to do synchronous hit testing and
  // b) We use HitTestQuery to find the target (instead of reusing previous
  // targets when hit testing latched events) and
  // c) We are not hit testing MouseMove events which is too frequent to
  // verify it without impacting performance.
  // The code that implements c) locates in |FindMouseEventTarget|.
  if (target.flags & viz::HitTestRegionFlags::kHitTestAsk)
    query_renderer = true;
  else if (features::IsVizHitTestingSurfaceLayerEnabled())
    should_verify_result = true;

  auto* view = FindViewFromFrameSinkId(frame_sink_id);
  // Send the event to |root_view| if |view| is not in |root_view|'s sub-tree
  // anymore.
  if (!view || (RenderWidgetHostViewGuest::GetRootView(view) != root_view)) {
    view = root_view;
    *transformed_point = point;
  }

  return {view, query_renderer, *transformed_point, false,
          should_verify_result};
}

void RenderWidgetHostInputEventRouter::RouteMouseEvent(
    RenderWidgetHostViewBase* root_view,
    blink::WebMouseEvent* event,
    const ui::LatencyInfo& latency) {
  if (::vivaldi::IsVivaldiRunning()) {
    VivaldiEventHooks* h = VivaldiEventHooks::FromRootView(root_view);
    if (h && h->HandleMouseEvent(root_view, *event))
      return;
  }
  event_targeter_->FindTargetAndDispatch(root_view, *event, latency);
}

void RenderWidgetHostInputEventRouter::DispatchMouseEvent(
    RenderWidgetHostViewBase* root_view,
    RenderWidgetHostViewBase* target,
    const blink::WebMouseEvent& mouse_event,
    const ui::LatencyInfo& latency,
    const base::Optional<gfx::PointF>& target_location) {
  // TODO(wjmaclean): Should we be sending a no-consumer ack to the root_view
  // if there is no target?
  if (!target)
    return;

  // Implicitly release any capture when a MouseUp arrives, so that if any
  // events arrive before the renderer can explicitly release capture, we can
  // target those correctly. This also releases if there are no mouse buttons
  // down, which is to protect against problems that can occur on some
  // platforms where MouseUps are not received when the mouse cursor is off the
  // browser window.
  // Also, this is strictly necessary for touch emulation.
  if (mouse_event.GetType() == blink::WebInputEvent::kMouseUp ||
      !IsMouseButtonDown(mouse_event))
    mouse_capture_target_ = nullptr;

  // When touch emulation is active, mouse events have to act like touch
  // events, which requires that there be implicit capture between MouseDown
  // and MouseUp.
  if (mouse_event.GetType() == blink::WebInputEvent::kMouseDown &&
      touch_emulator_ && touch_emulator_->enabled()) {
    mouse_capture_target_ = target;
  }

  DCHECK(target_location.has_value());
  blink::WebMouseEvent event = mouse_event;
  event.SetPositionInWidget(target_location->x(), target_location->y());

  // SendMouseEnterOrLeaveEvents is called with the original event
  // coordinates, which are transformed independently for each view that will
  // receive an event. Also, since the view under the mouse has changed,
  // notify the CursorManager that it might need to change the cursor.
  if ((event.GetType() == blink::WebInputEvent::kMouseLeave ||
       event.GetType() == blink::WebInputEvent::kMouseMove) &&
      target != last_mouse_move_target_ && !root_view->IsMouseLocked()) {
    SendMouseEnterOrLeaveEvents(mouse_event, target, root_view);
    if (root_view->GetCursorManager())
      root_view->GetCursorManager()->UpdateViewUnderCursor(target);
  }

  target->ProcessMouseEvent(event, latency);
}

void RenderWidgetHostInputEventRouter::RouteMouseWheelEvent(
    RenderWidgetHostViewBase* root_view,
    blink::WebMouseWheelEvent* event,
    const ui::LatencyInfo& latency) {
  if (::vivaldi::IsVivaldiRunning()) {
    VivaldiEventHooks* h = VivaldiEventHooks::FromRootView(root_view);
    if (h && h->HandleWheelEvent(root_view, *event, latency))
      return;
  }
  event_targeter_->FindTargetAndDispatch(root_view, *event, latency);
}

void RenderWidgetHostInputEventRouter::DispatchMouseWheelEvent(
    RenderWidgetHostViewBase* root_view,
    RenderWidgetHostViewBase* target,
    const blink::WebMouseWheelEvent& mouse_wheel_event,
    const ui::LatencyInfo& latency,
    const base::Optional<gfx::PointF>& target_location) {
  if (!root_view->IsMouseLocked()) {
    if (mouse_wheel_event.phase == blink::WebMouseWheelEvent::kPhaseBegan) {
      wheel_target_ = target;
    } else {
      if (wheel_target_) {
        DCHECK(!target);
        target = wheel_target_;
      } else if ((mouse_wheel_event.phase ==
                      blink::WebMouseWheelEvent::kPhaseEnded ||
                  mouse_wheel_event.momentum_phase ==
                      blink::WebMouseWheelEvent::kPhaseEnded) &&
                 bubbling_gesture_scroll_target_) {
        // Send a GSE to the bubbling target and cancel scroll bubbling since
        // the wheel target view is destroyed and the wheel end event won't get
        // processed.
        CancelScrollBubbling();
      }
    }
  }

  if (!target) {
    root_view->WheelEventAck(mouse_wheel_event,
                             INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS);
    return;
  }

  blink::WebMouseWheelEvent event = mouse_wheel_event;
  gfx::PointF point_in_target;
  if (target_location) {
    point_in_target = target_location.value();
  } else {
    point_in_target = target->TransformRootPointToViewCoordSpace(
        mouse_wheel_event.PositionInWidget());
  }
  event.SetPositionInWidget(point_in_target.x(), point_in_target.y());
  bool skip_process_call = false;
  if (::vivaldi::IsVivaldiRunning() && target == root_view) {
    VivaldiEventHooks* h = VivaldiEventHooks::FromRootView(root_view);
    if (h &&
        h->HandleWheelEventAfterChild(root_view, nullptr, mouse_wheel_event)) {
      root_view->WheelEventAck(mouse_wheel_event,
                               INPUT_EVENT_ACK_STATE_CONSUMED);
      skip_process_call = true;
    }
  }
  if (!skip_process_call) {
    // clang-format off
  target->ProcessMouseWheelEvent(event, latency);
    // clang-format on
  }

  if (mouse_wheel_event.phase == blink::WebMouseWheelEvent::kPhaseEnded ||
      mouse_wheel_event.momentum_phase ==
          blink::WebMouseWheelEvent::kPhaseEnded) {
    wheel_target_ = nullptr;
  }
}

void RenderWidgetHostInputEventRouter::RouteGestureEvent(
    RenderWidgetHostViewBase* root_view,
    const blink::WebGestureEvent* event,
    const ui::LatencyInfo& latency) {
  if (event->IsTargetViewport()) {
    root_view->ProcessGestureEvent(*event, latency);
    return;
  }

  switch (event->SourceDevice()) {
    case blink::WebGestureDevice::kUninitialized:
      NOTREACHED() << "Uninitialized device type is not allowed";
      break;
    case blink::WebGestureDevice::kSyntheticAutoscroll:
      NOTREACHED() << "Only target_viewport synthetic autoscrolls are "
                      "currently supported";
      break;
    case blink::WebGestureDevice::kTouchpad:
      RouteTouchpadGestureEvent(root_view, event, latency);
      break;
    case blink::WebGestureDevice::kTouchscreen:
      RouteTouchscreenGestureEvent(root_view, event, latency);
      break;
    case blink::WebGestureDevice::kScrollbar:
      NOTREACHED()
          << "This gesture source is only ever generated inside the renderer "
             "and is designated for compositor threaded scrollbar scrolling. "
             "We should never see it in the browser.";
      break;
  };
}

namespace {

unsigned CountChangedTouchPoints(const blink::WebTouchEvent& event) {
  unsigned changed_count = 0;

  blink::WebTouchPoint::State required_state =
      blink::WebTouchPoint::kStateUndefined;
  switch (event.GetType()) {
    case blink::WebInputEvent::kTouchStart:
      required_state = blink::WebTouchPoint::kStatePressed;
      break;
    case blink::WebInputEvent::kTouchEnd:
      required_state = blink::WebTouchPoint::kStateReleased;
      break;
    case blink::WebInputEvent::kTouchCancel:
      required_state = blink::WebTouchPoint::kStateCancelled;
      break;
    default:
      // We'll only ever call this method for TouchStart, TouchEnd
      // and TounchCancel events, so mark the rest as not-reached.
      NOTREACHED();
  }
  for (unsigned i = 0; i < event.touches_length; ++i) {
    if (event.touches[i].state == required_state)
      ++changed_count;
  }

  DCHECK(event.GetType() == blink::WebInputEvent::kTouchCancel ||
         changed_count == 1);
  return changed_count;
}

}  // namespace

// Any time a touch start event is handled/consumed/default prevented it is
// removed from the gesture map, because it will never create a gesture.
void RenderWidgetHostInputEventRouter::OnHandledTouchStartOrFirstTouchMove(
    uint32_t unique_touch_event_id) {
  // unique_touch_event_id of 0 implies a gesture not created by a touch.
  DCHECK_NE(unique_touch_event_id, 0U);
  touchscreen_gesture_target_map_.erase(unique_touch_event_id);
}

RenderWidgetTargetResult RenderWidgetHostInputEventRouter::FindTouchEventTarget(
    RenderWidgetHostViewBase* root_view,
    const blink::WebTouchEvent& event) {
  // Tests may call this without an initial TouchStart, so check event type
  // explicitly here.
  if (active_touches_ || event.GetType() != blink::WebInputEvent::kTouchStart)
    return {nullptr, false, base::nullopt, true, false};

  active_touches_ += CountChangedTouchPoints(event);
  gfx::PointF original_point = gfx::PointF(event.touches[0].PositionInWidget());
  gfx::PointF transformed_point;

  return FindViewAtLocation(root_view, original_point, viz::EventSource::TOUCH,
                            &transformed_point);
}

void RenderWidgetHostInputEventRouter::DispatchTouchEvent(
    RenderWidgetHostViewBase* root_view,
    RenderWidgetHostViewBase* target,
    const blink::WebTouchEvent& touch_event,
    const ui::LatencyInfo& latency,
    const base::Optional<gfx::PointF>& target_location,
    bool is_emulated_touchevent) {
  DCHECK(blink::WebInputEvent::IsTouchEventType(touch_event.GetType()) &&
         touch_event.GetType() != blink::WebInputEvent::kTouchScrollStarted);

  bool is_sequence_start = !touch_target_ && target;
  if (is_sequence_start) {
    touch_target_ = target;
    DCHECK(touchscreen_gesture_target_map_.find(
               touch_event.unique_touch_event_id) ==
           touchscreen_gesture_target_map_.end());
    touchscreen_gesture_target_map_[touch_event.unique_touch_event_id] =
        touch_target_;
  } else if (touch_event.GetType() == blink::WebInputEvent::kTouchStart) {
    active_touches_ += CountChangedTouchPoints(touch_event);
  }

  // Test active_touches_ before decrementing, since its value can be
  // reset to 0 in OnRenderWidgetHostViewBaseDestroyed, and this can
  // happen between the TouchStart and a subsequent TouchMove/End/Cancel.
  if ((touch_event.GetType() == blink::WebInputEvent::kTouchEnd ||
       touch_event.GetType() == blink::WebInputEvent::kTouchCancel) &&
      active_touches_) {
    active_touches_ -= CountChangedTouchPoints(touch_event);
  }
  DCHECK_GE(active_touches_, 0);

  // Debugging for crbug.com/814674.
  if (touch_target_ && !IsViewInMap(touch_target_)) {
    NOTREACHED() << "Touch events should not be routed to a destroyed target "
                    "View.";
    touch_target_ = nullptr;
    base::debug::DumpWithoutCrashing();
  }

  TouchEventAckQueue::TouchEventSource event_source =
      is_emulated_touchevent
          ? TouchEventAckQueue::TouchEventSource::EmulatedTouchEvent
          : TouchEventAckQueue::TouchEventSource::SystemTouchEvent;
  if (!touch_target_) {
    touch_event_ack_queue_->Add(
        TouchEventWithLatencyInfo(touch_event), nullptr, root_view,
        event_source, TouchEventAckQueue::TouchEventAckStatus::TouchEventAcked,
        INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS);
    return;
  }

  gfx::Transform transform;
  if (!root_view->GetTransformToViewCoordSpace(touch_target_, &transform)) {
    // Fall-back to just using the delta if we are unable to get the full
    // transform.
    transform.MakeIdentity();
    if (target_location.has_value()) {
      transform.Translate(target_location.value() -
                          touch_event.touches[0].PositionInWidget());
    } else {
      // GetTransformToViewCoordSpace() fails when viz_hit_test is off but
      // TransformRootPointToViewCoordSpace() still works at this case.
      // TODO(crbug.com/917015) remove the extra code when viz_hit_test is
      // always on.
      gfx::PointF point_in_target =
          touch_target_->TransformRootPointToViewCoordSpace(
              touch_event.touches[0].PositionInWidget());
      transform.Translate(point_in_target -
                          touch_event.touches[0].PositionInWidget());
    }
  }

  if (is_sequence_start) {
    CancelScrollBubblingIfConflicting(touch_target_);
  }

  touch_event_ack_queue_->Add(TouchEventWithLatencyInfo(touch_event),
                              touch_target_, root_view, event_source);

  blink::WebTouchEvent event(touch_event);
  TransformEventTouchPositions(&event, transform);
  touch_target_->ProcessTouchEvent(event, latency);

  if (!active_touches_)
    touch_target_ = nullptr;
}

void RenderWidgetHostInputEventRouter::ProcessAckedTouchEvent(
    const TouchEventWithLatencyInfo& event,
    InputEventAckState ack_result,
    RenderWidgetHostViewBase* view) {
  touch_event_ack_queue_->MarkAcked(event, ack_result, view);
}

void RenderWidgetHostInputEventRouter::RouteTouchEvent(
    RenderWidgetHostViewBase* root_view,
    blink::WebTouchEvent* event,
    const ui::LatencyInfo& latency) {
  event_targeter_->FindTargetAndDispatch(root_view, *event, latency);
}

void RenderWidgetHostInputEventRouter::SendMouseEnterOrLeaveEvents(
    const blink::WebMouseEvent& event,
    RenderWidgetHostViewBase* target,
    RenderWidgetHostViewBase* root_view) {
  // This method treats RenderWidgetHostViews as a tree, where the mouse
  // cursor is potentially leaving one node and entering another somewhere
  // else in the tree. Since iframes are graphically self-contained (i.e. an
  // iframe can't have a descendant that renders outside of its rect
  // boundaries), all affected RenderWidgetHostViews are ancestors of either
  // the node being exited or the node being entered.
  // Approach:
  // 1. Find lowest common ancestor (LCA) of the last view and current target
  //    view.
  // 2. The last view, and its ancestors up to but not including the LCA,
  //    receive a MouseLeave.
  // 3. The LCA itself, unless it is the new target, receives a MouseOut
  //    because the cursor has passed between elements within its bounds.
  // 4. The new target view's ancestors, up to but not including the LCA,
  //    receive a MouseEnter.
  // Ordering does not matter since these are handled asynchronously relative
  // to each other.

  // If the mouse has moved onto a different root view (typically meaning it
  // has crossed over a popup or context menu boundary), then we invalidate
  // last_mouse_move_target_ because we have no reference for its coordinate
  // space.
  if (root_view != last_mouse_move_root_view_)
    last_mouse_move_target_ = nullptr;

  // Finding the LCA uses a standard approach. We build vectors of the
  // ancestors of each node up to the root, and then remove common ancestors.
  std::vector<RenderWidgetHostViewBase*> entered_views;
  std::vector<RenderWidgetHostViewBase*> exited_views;
  RenderWidgetHostViewBase* cur_view = target;
  entered_views.push_back(cur_view);
  // Non-root RWHVs are guaranteed to be RenderWidgetHostViewChildFrames,
  // as long as they are the only embeddable RWHVs.
  while (cur_view->IsRenderWidgetHostViewChildFrame()) {
    cur_view =
        static_cast<RenderWidgetHostViewChildFrame*>(cur_view)->GetParentView();
    // cur_view can possibly be nullptr for guestviews that are not currently
    // connected to the webcontents tree.
    if (!cur_view) {
      last_mouse_move_target_ = target;
      last_mouse_move_root_view_ = root_view;
      return;
    }
    entered_views.push_back(cur_view);
  }

  // On Windows, it appears to be possible that render widget targeting could
  // produce a target that is outside of the specified root. For now, we'll
  // just give up in such a case. See https://crbug.com/851958.
  if (cur_view != root_view)
    return;

  cur_view = last_mouse_move_target_;
  if (cur_view) {
    exited_views.push_back(cur_view);
    while (cur_view->IsRenderWidgetHostViewChildFrame()) {
      cur_view = static_cast<RenderWidgetHostViewChildFrame*>(cur_view)
                     ->GetParentView();
      if (!cur_view) {
        last_mouse_move_target_ = target;
        last_mouse_move_root_view_ = root_view;
        return;
      }
      exited_views.push_back(cur_view);
    }
    // NOTE(andre@vivaldi.com) : In Vivaldi we can end up with a non root here
    // as we have patches in the event emit functions. See
    // |RenderWidgetTargeter::FoundTarget| Might be problematic, but hide this
    // as it happens often when moving views between windows.
    if (!vivaldi::IsVivaldiRunning()) {
    DCHECK_EQ(cur_view, root_view);
    }
  }

  // This removes common ancestors from the root downward.
  RenderWidgetHostViewBase* common_ancestor = nullptr;
  while (entered_views.size() > 0 && exited_views.size() > 0 &&
         entered_views.back() == exited_views.back()) {
    common_ancestor = entered_views.back();
    entered_views.pop_back();
    exited_views.pop_back();
  }

  gfx::PointF transformed_point;
  // Send MouseLeaves.
  for (auto* view : exited_views) {
    blink::WebMouseEvent mouse_leave(event);
    mouse_leave.SetType(blink::WebInputEvent::kMouseLeave);
    // There is a chance of a race if the last target has recently created a
    // new compositor surface. The SurfaceID for that might not have
    // propagated to its embedding surface, which makes it impossible to
    // compute the transformation for it
    if (!root_view->TransformPointToCoordSpaceForView(
            event.PositionInWidget(), view, &transformed_point)) {
      transformed_point = gfx::PointF();
    }
    mouse_leave.SetPositionInWidget(transformed_point.x(),
                                    transformed_point.y());
    view->ProcessMouseEvent(mouse_leave, ui::LatencyInfo());
  }

  // The ancestor might need to trigger MouseOut handlers.
  if (common_ancestor && common_ancestor != target) {
    blink::WebMouseEvent mouse_move(event);
    mouse_move.SetType(blink::WebInputEvent::kMouseMove);
    if (!root_view->TransformPointToCoordSpaceForView(
            event.PositionInWidget(), common_ancestor, &transformed_point)) {
      transformed_point = gfx::PointF();
    }
    mouse_move.SetPositionInWidget(transformed_point.x(),
                                   transformed_point.y());
    common_ancestor->ProcessMouseEvent(mouse_move, ui::LatencyInfo());
  }

  // Send MouseMoves to trigger MouseEnter handlers.
  for (auto* view : entered_views) {
    if (view == target)
      continue;
    blink::WebMouseEvent mouse_enter(event);
    mouse_enter.SetType(blink::WebInputEvent::kMouseMove);
    if (!root_view->TransformPointToCoordSpaceForView(
            event.PositionInWidget(), view, &transformed_point)) {
      transformed_point = gfx::PointF();
    }
    mouse_enter.SetPositionInWidget(transformed_point.x(),
                                    transformed_point.y());
    view->ProcessMouseEvent(mouse_enter, ui::LatencyInfo());
  }

  last_mouse_move_target_ = target;
  last_mouse_move_root_view_ = root_view;
}

void RenderWidgetHostInputEventRouter::ReportBubblingScrollToSameView(
    const blink::WebGestureEvent& event,
    const RenderWidgetHostViewBase* view) {
#if 0
  // For now, we've disabled the DumpWithoutCrashing as it's no longer
  // providing useful information.
  // TODO(828422): Determine useful crash keys and reenable the report.
  base::debug::DumpWithoutCrashing();
#endif
}

namespace {

// Returns true if |target_view| is one of |starting_view|'s ancestors.
// If |stay_within| is provided, we only consider ancestors within that
// sub-tree.
bool IsAncestorView(RenderWidgetHostViewChildFrame* starting_view,
                    const RenderWidgetHostViewBase* target_view,
                    const RenderWidgetHostViewBase* stay_within = nullptr) {
  RenderWidgetHostViewBase* cur_view = starting_view->GetParentView();
  while (cur_view) {
    if (cur_view == target_view)
      return true;

    if (stay_within && cur_view == stay_within)
      return false;

    cur_view = cur_view->IsRenderWidgetHostViewChildFrame()
                   ? static_cast<RenderWidgetHostViewChildFrame*>(cur_view)
                         ->GetParentView()
                   : nullptr;
  }
  return false;
}

// Given |event| in root coordinates, return an event in |target_view|'s
// coordinates.
blink::WebGestureEvent GestureEventInTarget(
    const blink::WebGestureEvent& event,
    RenderWidgetHostViewBase* target_view) {
  const gfx::PointF point_in_target =
      target_view->TransformRootPointToViewCoordSpace(event.PositionInWidget());
  blink::WebGestureEvent event_for_target(event);
  event_for_target.SetPositionInWidget(point_in_target);
  return event_for_target;
}

}  // namespace

bool RenderWidgetHostInputEventRouter::BubbleScrollEvent(
    RenderWidgetHostViewBase* target_view,
    RenderWidgetHostViewChildFrame* resending_view,
    const blink::WebGestureEvent& event) {
  DCHECK(target_view);
  DCHECK(event.GetType() == blink::WebInputEvent::kGestureScrollBegin ||
         event.GetType() == blink::WebInputEvent::kGestureScrollUpdate ||
         event.GetType() == blink::WebInputEvent::kGestureScrollEnd);

  ui::LatencyInfo latency_info =
      ui::WebInputEventTraits::CreateLatencyInfoForWebGestureEvent(event);

  if (event.GetType() == blink::WebInputEvent::kGestureScrollBegin) {
    forced_last_fling_start_target_to_stop_flinging_for_test_ = false;
    // If target_view has unrelated gesture events in progress, do
    // not proceed. This could cause confusion between independent
    // scrolls.
    if (target_view == touchscreen_gesture_target_ ||
        target_view == touchpad_gesture_target_ ||
        target_view == touch_target_) {
      return false;
    }

    // A view is trying to bubble a separate scroll sequence while we have
    // ongoing bubbling.
    if (bubbling_gesture_scroll_target_ &&
        bubbling_gesture_scroll_target_ != resending_view) {
      return false;
    }

    // This accounts for bubbling through nested OOPIFs. A gesture scroll
    // begin has been bubbled but the target has sent back a gesture scroll
    // event ack which didn't consume any scroll delta, and so another level
    // of bubbling is needed. This requires a GestureScrollEnd be sent to the
    // last view, which will no longer be the scroll target.
    if (bubbling_gesture_scroll_target_) {
      SendGestureScrollEnd(
          bubbling_gesture_scroll_target_,
          GestureEventInTarget(event, bubbling_gesture_scroll_target_));
    } else {
      bubbling_gesture_scroll_origin_ = resending_view;
    }

    bubbling_gesture_scroll_target_ = target_view;
    bubbling_gesture_scroll_source_device_ = event.SourceDevice();
    DCHECK(IsAncestorView(bubbling_gesture_scroll_origin_,
                          bubbling_gesture_scroll_target_));
  } else {  // !(event.GetType() == blink::WebInputEvent::kGestureScrollBegin)
    if (!bubbling_gesture_scroll_target_) {
      // Drop any acked events that come in after bubbling has ended.
      // TODO(mcnee): If we inform |bubbling_gesture_scroll_origin_| and the
      // intermediate views of the end of bubbling, we could presumably DCHECK
      // that we have a target.
      return false;
    }

    // Don't bubble the GSE events that are generated and sent to intermediate
    // bubbling targets.
    if (event.GetType() == blink::WebInputEvent::kGestureScrollEnd &&
        resending_view != bubbling_gesture_scroll_origin_) {
      return true;
    }
  }

  // If the router tries to resend a gesture scroll event back to the same
  // view, we could hang.
  DCHECK_NE(resending_view, bubbling_gesture_scroll_target_);
  // We've seen reports of this, but don't know the cause yet. For now,
  // instead of CHECKing or hanging, we'll report the issue and abort scroll
  // bubbling.
  // TODO(828422): Remove once this issue no longer occurs.
  if (resending_view == bubbling_gesture_scroll_target_) {
    ReportBubblingScrollToSameView(event, resending_view);
    CancelScrollBubbling();
    return false;
  }

  const bool touchscreen_bubble_to_root =
      event.SourceDevice() == blink::WebGestureDevice::kTouchscreen &&
      !bubbling_gesture_scroll_target_->IsRenderWidgetHostViewChildFrame();
  if (touchscreen_bubble_to_root) {
    if (event.GetType() == blink::WebInputEvent::kGestureScrollBegin) {
      touchscreen_pinch_state_.DidStartBubblingToRoot();

      // If a pinch's scroll sequence is sent to an OOPIF and the pinch
      // begin event is dispatched to the root before the scroll begin is
      // bubbled to the root, a wrapping scroll begin will have already been
      // sent to the root.
      if (touchscreen_pinch_state_.IsInPinch()) {
        return true;
      }
    } else if (event.GetType() == blink::WebInputEvent::kGestureScrollEnd) {
      touchscreen_pinch_state_.DidStopBubblingToRoot();
    }
  }

  bubbling_gesture_scroll_target_->ProcessGestureEvent(
      GestureEventInTarget(event, bubbling_gesture_scroll_target_),
      latency_info);

  if (event.GetType() == blink::WebInputEvent::kGestureScrollEnd) {
    bubbling_gesture_scroll_origin_ = nullptr;
    bubbling_gesture_scroll_target_ = nullptr;
    bubbling_gesture_scroll_source_device_ =
        blink::WebGestureDevice::kUninitialized;
  }
  return true;
}

void RenderWidgetHostInputEventRouter::SendGestureScrollBegin(
    RenderWidgetHostViewBase* view,
    const blink::WebGestureEvent& event) {
  DCHECK_EQ(blink::WebInputEvent::kGesturePinchBegin, event.GetType());
  DCHECK_EQ(blink::WebGestureDevice::kTouchscreen, event.SourceDevice());
  blink::WebGestureEvent scroll_begin(event);
  scroll_begin.SetType(blink::WebInputEvent::kGestureScrollBegin);
  scroll_begin.data.scroll_begin.delta_x_hint = 0;
  scroll_begin.data.scroll_begin.delta_y_hint = 0;
  scroll_begin.data.scroll_begin.delta_hint_units =
      ui::input_types::ScrollGranularity::kScrollByPrecisePixel;
  scroll_begin.data.scroll_begin.scrollable_area_element_id = 0;
  view->ProcessGestureEvent(
      scroll_begin,
      ui::WebInputEventTraits::CreateLatencyInfoForWebGestureEvent(
          scroll_begin));
}

void RenderWidgetHostInputEventRouter::SendGestureScrollEnd(
    RenderWidgetHostViewBase* view,
    const blink::WebGestureEvent& event) {
  blink::WebGestureEvent scroll_end(event);
  scroll_end.SetType(blink::WebInputEvent::kGestureScrollEnd);
  scroll_end.SetTimeStamp(base::TimeTicks::Now());
  switch (event.GetType()) {
    case blink::WebInputEvent::kGestureScrollBegin:
      scroll_end.data.scroll_end.inertial_phase =
          event.data.scroll_begin.inertial_phase;
      scroll_end.data.scroll_end.delta_units =
          event.data.scroll_begin.delta_hint_units;
      break;
    case blink::WebInputEvent::kGesturePinchEnd:
      DCHECK_EQ(blink::WebGestureDevice::kTouchscreen, event.SourceDevice());
      scroll_end.data.scroll_end.inertial_phase =
          blink::WebGestureEvent::InertialPhaseState::kUnknownMomentum;
      scroll_end.data.scroll_end.delta_units =
          ui::input_types::ScrollGranularity::kScrollByPrecisePixel;
      break;
    default:
      NOTREACHED();
  }
  view->ProcessGestureEvent(
      scroll_end,
      ui::WebInputEventTraits::CreateLatencyInfoForWebGestureEvent(scroll_end));
}

void RenderWidgetHostInputEventRouter::SendGestureScrollEnd(
    RenderWidgetHostViewBase* view,
    blink::WebGestureDevice source_device) {
  blink::WebGestureEvent scroll_end(blink::WebInputEvent::kGestureScrollEnd,
                                    blink::WebInputEvent::kNoModifiers,
                                    base::TimeTicks::Now(), source_device);
  scroll_end.data.scroll_end.inertial_phase =
      blink::WebGestureEvent::InertialPhaseState::kUnknownMomentum;
  scroll_end.data.scroll_end.delta_units =
      ui::input_types::ScrollGranularity::kScrollByPrecisePixel;
  view->ProcessGestureEvent(
      scroll_end,
      ui::WebInputEventTraits::CreateLatencyInfoForWebGestureEvent(scroll_end));
}

void RenderWidgetHostInputEventRouter::WillDetachChildView(
    const RenderWidgetHostViewChildFrame* detaching_view) {
  // If necessary, cancel ongoing scroll bubbling in response to a frame
  // connector change.
  if (!bubbling_gesture_scroll_target_ || !bubbling_gesture_scroll_origin_)
    return;

  // We cancel bubbling only when the child view affects the current scroll
  // bubbling sequence.
  if (detaching_view == bubbling_gesture_scroll_origin_ ||
      IsAncestorView(bubbling_gesture_scroll_origin_, detaching_view)) {
    CancelScrollBubbling();
  }
}

void RenderWidgetHostInputEventRouter::CancelScrollBubbling() {
  DCHECK(bubbling_gesture_scroll_target_);
  SendGestureScrollEnd(bubbling_gesture_scroll_target_,
                       bubbling_gesture_scroll_source_device_);

  const bool touchscreen_bubble_to_root =
      bubbling_gesture_scroll_source_device_ ==
          blink::WebGestureDevice::kTouchscreen &&
      !bubbling_gesture_scroll_target_->IsRenderWidgetHostViewChildFrame();
  if (touchscreen_bubble_to_root)
    touchscreen_pinch_state_.DidStopBubblingToRoot();

  // TODO(mcnee): We should also inform |bubbling_gesture_scroll_origin_| that
  // we are no longer bubbling its events, otherwise it could continue to send
  // them and interfere with a new scroll gesture being bubbled.
  // See https://crbug.com/828422
  bubbling_gesture_scroll_origin_ = nullptr;
  bubbling_gesture_scroll_target_ = nullptr;
  bubbling_gesture_scroll_source_device_ =
      blink::WebGestureDevice::kUninitialized;
}

void RenderWidgetHostInputEventRouter::CancelScrollBubblingIfConflicting(
    const RenderWidgetHostViewBase* target) {
  if (!target)
    return;
  if (!bubbling_gesture_scroll_target_ || !bubbling_gesture_scroll_origin_)
    return;

  if (IsAncestorView(bubbling_gesture_scroll_origin_, target,
                     bubbling_gesture_scroll_target_)) {
    CancelScrollBubbling();
  }
}

void RenderWidgetHostInputEventRouter::StopFling() {
  if (!bubbling_gesture_scroll_target_)
    return;

  if (!last_fling_start_target_ || !last_fling_start_target_->host())
    return;

  // The last_fling_start_target_'s fling controller must stop flinging when its
  // generated GSUs are not consumed by the bubbling target view.
  last_fling_start_target_->host()->StopFling();
  forced_last_fling_start_target_to_stop_flinging_for_test_ = true;
}
void RenderWidgetHostInputEventRouter::AddFrameSinkIdOwner(
    const viz::FrameSinkId& id,
    RenderWidgetHostViewBase* owner) {
  DCHECK(owner_map_.find(id) == owner_map_.end());
  // We want to be notified if the owner is destroyed so we can remove it from
  // our map.
  owner->AddObserver(this);
  owner_map_.insert(std::make_pair(id, owner));
}

void RenderWidgetHostInputEventRouter::RemoveFrameSinkIdOwner(
    const viz::FrameSinkId& id) {
  auto it_to_remove = owner_map_.find(id);
  if (it_to_remove != owner_map_.end()) {
    // If we remove a view from the observer list, we need to be sure to do a
    // cleanup of the various targets and target maps, else we will end up with
    // stale values if the view destructs and isn't an observer anymore.
    // Note: the view the iterator points at will be deleted in the following
    // call, and shouldn't be used after this point.
    OnRenderWidgetHostViewBaseDestroyed(it_to_remove->second);
  }
}

RenderWidgetHostImpl*
RenderWidgetHostInputEventRouter::GetRenderWidgetHostAtPoint(
    RenderWidgetHostViewBase* root_view,
    const gfx::PointF& point,
    gfx::PointF* transformed_point) {
  if (!root_view)
    return nullptr;
  return RenderWidgetHostImpl::From(FindViewAtLocation(root_view, point,
                                                       viz::EventSource::MOUSE,
                                                       transformed_point)
                                        .view->GetRenderWidgetHost());
}

void RenderWidgetHostInputEventRouter::GetRenderWidgetHostAtPointAsynchronously(
    RenderWidgetHostViewBase* root_view,
    const gfx::PointF& point,
    RenderWidgetTargeter::RenderWidgetHostAtPointCallback callback) {
  event_targeter_->FindTargetAndCallback(root_view, point, std::move(callback));
}

RenderWidgetTargetResult
RenderWidgetHostInputEventRouter::FindTouchscreenGestureEventTarget(
    RenderWidgetHostViewBase* root_view,
    const blink::WebGestureEvent& gesture_event) {
  if (vivaldi::IsVivaldiRunning() &&
      blink::WebInputEvent::IsPinchGestureEventType(gesture_event.GetType())) {
    // We have to find the view under the event position as we can not use the
    // root view since it includes the UI.
    gfx::PointF transformed_point;
    return FindViewAtLocation(root_view, gesture_event.PositionInWidget(),
                              viz::EventSource::TOUCH, &transformed_point);
  }
  // Since DispatchTouchscreenGestureEvent() doesn't pay any attention to the
  // target we could just return nullptr for pinch events, but since we know
  // where they are going we return the correct target.
  if (blink::WebInputEvent::IsPinchGestureEventType(gesture_event.GetType()))
    return {root_view, false, gesture_event.PositionInWidget(), true, false};

  // Android sends gesture events that have no corresponding touch sequence, so
  // these we hit-test explicitly.
  if (gesture_event.unique_touch_event_id == 0) {
    gfx::PointF transformed_point;
    gfx::PointF original_point(gesture_event.PositionInWidget());
    return FindViewAtLocation(root_view, original_point,
                              viz::EventSource::TOUCH, &transformed_point);
  }

  // Remaining gesture events will defer to the gesture event target queue
  // during dispatch.
  return {nullptr, false, base::nullopt, true, false};
}

bool RenderWidgetHostInputEventRouter::IsViewInMap(
    const RenderWidgetHostViewBase* view) const {
  DCHECK(!is_registered(view->GetFrameSinkId()) ||
         owner_map_.find(view->GetFrameSinkId())->second == view);
  return is_registered(view->GetFrameSinkId());
}

bool RenderWidgetHostInputEventRouter::ViewMapIsEmpty() const {
  return owner_map_.empty();
}

namespace {

bool IsPinchCurrentlyAllowedInTarget(RenderWidgetHostViewBase* target) {
  base::Optional<cc::TouchAction> target_active_touch_action(
      cc::kTouchActionNone);
  if (target) {
    target_active_touch_action =
        (static_cast<RenderWidgetHostImpl*>(target->GetRenderWidgetHost()))
            ->input_router()
            ->ActiveTouchAction();
  }
  // This function is called on GesturePinchBegin, by which time there should
  // be an active touch action assessed for the target.
  // DCHECK(target_active_touch_action.has_value());
  // TODO(wjmaclean): Find out why we can be in the middle of a gesture
  // sequence and not have a valid touch action assigned.
  if (!target_active_touch_action)
    target_active_touch_action = cc::kTouchActionNone;
  return (target_active_touch_action.value() &
          cc::TouchAction::kTouchActionPinchZoom);
}

}  // namespace

void RenderWidgetHostInputEventRouter::DispatchTouchscreenGestureEvent(
    RenderWidgetHostViewBase* root_view,
    RenderWidgetHostViewBase* target,
    const blink::WebGestureEvent& gesture_event,
    const ui::LatencyInfo& latency,
    const base::Optional<gfx::PointF>& target_location) {
  if (gesture_event.GetType() == blink::WebInputEvent::kGesturePinchBegin) {
    if (root_view == touchscreen_gesture_target_) {
      // If the root view is the current gesture target, there is no need to
      // wrap the pinch events ourselves.
      touchscreen_pinch_state_.DidStartPinchInRoot();
    } else if (IsPinchCurrentlyAllowedInTarget(touchscreen_gesture_target_)) {
      // If pinch is not allowed in the child, don't do any diverting of the
      // pinch events to the root. Have the events go to the child whose
      // TouchActionFilter will discard them.

      auto* root_rwhi =
          static_cast<RenderWidgetHostImpl*>(root_view->GetRenderWidgetHost());

      // The pinch gesture will be sent to the root view and it may not have a
      // valid touch action yet. In this case, set the touch action to auto.
      root_rwhi->input_router()->ForceSetTouchActionAuto();

      if (touchscreen_pinch_state_.NeedsWrappingScrollSequence()) {
        // If the root view is not the gesture target, and a scroll gesture has
        // not already started in the root from scroll bubbling, then we need
        // to warp the diverted pinch events in a GestureScrollBegin/End.
        DCHECK(!root_rwhi->is_in_touchscreen_gesture_scroll());
        if (vivaldi::IsVivaldiRunning() && target) // Prevent pinching UI
          SendGestureScrollBegin(target, gesture_event);
        else
        SendGestureScrollBegin(root_view, gesture_event);
      }

      touchscreen_pinch_state_.DidStartPinchInChild();
    }
  }

  if (touchscreen_pinch_state_.IsInPinch()) {
    if (vivaldi::IsVivaldiRunning() && target) // Prevent pinching UI
      target->ProcessGestureEvent(gesture_event, latency);
    else
    root_view->ProcessGestureEvent(gesture_event, latency);

    if (gesture_event.GetType() == blink::WebInputEvent::kGesturePinchEnd) {
      const bool send_scroll_end =
          touchscreen_pinch_state_.NeedsWrappingScrollSequence();
      touchscreen_pinch_state_.DidStopPinch();

      if (send_scroll_end) {
        auto* root_rwhi = static_cast<RenderWidgetHostImpl*>(
            root_view->GetRenderWidgetHost());
        DCHECK(root_rwhi->is_in_touchscreen_gesture_scroll());
        if (vivaldi::IsVivaldiRunning() && target) // Prevent pinching UI
          SendGestureScrollEnd(target, gesture_event);
        else
        SendGestureScrollEnd(root_view, gesture_event);
      }
    }

    return;
  }

  if (gesture_event.GetType() == blink::WebInputEvent::kGestureFlingCancel &&
      last_fling_start_target_) {
    last_fling_start_target_->ProcessGestureEvent(gesture_event, latency);
    return;
  }

  auto gesture_target_it =
      touchscreen_gesture_target_map_.find(gesture_event.unique_touch_event_id);
  bool no_matching_id =
      gesture_target_it == touchscreen_gesture_target_map_.end();

  // We use GestureTapDown to detect the start of a gesture sequence since
  // there is no WebGestureEvent equivalent for ET_GESTURE_BEGIN.
  const bool is_gesture_start =
      gesture_event.GetType() == blink::WebInputEvent::kGestureTapDown;

  base::Optional<gfx::PointF> fallback_target_location;

  if (gesture_event.unique_touch_event_id == 0) {
    // On Android it is possible for touchscreen gesture events to arrive that
    // are not associated with touch events, because non-synthetic events can be
    // created by ContentView. These will use the target found by the
    // RenderWidgetTargeter. These gesture events should always have a
    // unique_touch_event_id of 0. They must have a non-null target in order
    // to get the coordinate transform.
    DCHECK(target);
    fallback_target_location = target_location;
  } else if (no_matching_id && is_gesture_start) {
    // A long-standing Windows issues where occasionally a GestureStart is
    // encountered with no targets in the event queue. We never had a repro for
    // this, but perhaps we should drop these events and wait to see if a bug
    // (with a repro) gets filed, then just fix it.
    //
    // For now, we do a synchronous-only hit test here, which even though
    // incorrect is not likely to have a large effect in the short term.
    UMA_HISTOGRAM_BOOLEAN("Event.FrameEventRouting.NoGestureTarget", true);
    LOG(ERROR) << "Gesture sequence start detected with no target available.";
    // It is still safe to continue; we will recalculate the target.
    gfx::PointF transformed_point;
    gfx::PointF original_point(gesture_event.PositionInWidget());
    auto result = FindViewAtLocation(
        root_view, original_point, viz::EventSource::TOUCH, &transformed_point);
    // Re https://crbug.com/796656): Since we are already in an error case,
    // don't worry about the fact we're ignoring |result.should_query_view|, as
    // this is the best we can do until we fix https://crbug.com/595422.
    target = result.view;
    fallback_target_location = transformed_point;
  } else if (is_gesture_start) {
    target = gesture_target_it->second;
    touchscreen_gesture_target_map_.erase(gesture_target_it);

    // Abort any scroll bubbling in progress to avoid double entry.
    CancelScrollBubblingIfConflicting(target);
  }

  if (gesture_event.unique_touch_event_id == 0 || is_gesture_start) {
    bool moved_recently = touchscreen_gesture_target_moved_recently_;
    if (is_gesture_start)
      moved_recently = target->ScreenRectIsUnstableFor(gesture_event);
    SetTouchscreenGestureTarget(target, moved_recently);
  }

  // If we set a target and it's not in the map, we won't get notified if the
  // target goes away, so drop the target and the resulting events.
  if (touchscreen_gesture_target_ && !IsViewInMap(touchscreen_gesture_target_))
    SetTouchscreenGestureTarget(nullptr);

  if (!touchscreen_gesture_target_) {
    root_view->GestureEventAck(gesture_event,
                               INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS);
    return;
  }

  blink::WebGestureEvent event(gesture_event);
  if (touchscreen_gesture_target_moved_recently_)
    event.SetTargetFrameMovedRecently();

  gfx::PointF point_in_target;
  // This |fallback_target_location| is fast path when
  // |gesture_event.unique_touch_event_id == 0| or
  // |no_matching_id && is_gesture_start|, we did actually do hit testing in
  // these two cases. |target_location| pass in maybe wrong in other cases.
  if (fallback_target_location) {
    point_in_target = fallback_target_location.value();
  } else {
    point_in_target =
        touchscreen_gesture_target_->TransformRootPointToViewCoordSpace(
            gesture_event.PositionInWidget());
  }

  event.SetPositionInWidget(point_in_target);

  if (events_being_flushed_) {
    touchscreen_gesture_target_->host()
        ->input_router()
        ->ForceSetTouchActionAuto();
  }
  touchscreen_gesture_target_->ProcessGestureEvent(event, latency);

  if (gesture_event.GetType() == blink::WebInputEvent::kGestureFlingStart)
    last_fling_start_target_ = touchscreen_gesture_target_;

  // If we have one of the following events, then the user has lifted their
  // last finger.
  const bool is_gesture_end =
      gesture_event.GetType() == blink::WebInputEvent::kGestureTap ||
      gesture_event.GetType() == blink::WebInputEvent::kGestureLongTap ||
      gesture_event.GetType() == blink::WebInputEvent::kGestureDoubleTap ||
      gesture_event.GetType() == blink::WebInputEvent::kGestureTwoFingerTap ||
      gesture_event.GetType() == blink::WebInputEvent::kGestureScrollEnd ||
      gesture_event.GetType() == blink::WebInputEvent::kGestureFlingStart;

  if (is_gesture_end)
    SetTouchscreenGestureTarget(nullptr);
}

void RenderWidgetHostInputEventRouter::RouteTouchscreenGestureEvent(
    RenderWidgetHostViewBase* root_view,
    const blink::WebGestureEvent* event,
    const ui::LatencyInfo& latency) {
  DCHECK_EQ(blink::WebGestureDevice::kTouchscreen, event->SourceDevice());
  event_targeter_->FindTargetAndDispatch(root_view, *event, latency);
}

RenderWidgetTargetResult
RenderWidgetHostInputEventRouter::FindTouchpadGestureEventTarget(
    RenderWidgetHostViewBase* root_view,
    const blink::WebGestureEvent& event) const {

  if (vivaldi::IsVivaldiRunning() &&
    (event.GetType() == blink::WebInputEvent::kGestureDoubleTap)) {
    // NOTE(espen@vivaldi.com): We have to find the view under the event
    // position as we can not use the root view since it includes the UI.
    gfx::PointF transformed_point;
    return FindViewAtLocation(root_view, event.PositionInWidget(),
                              viz::EventSource::TOUCH, &transformed_point);
  }

  if (event.GetType() != blink::WebInputEvent::kGesturePinchBegin &&
      event.GetType() != blink::WebInputEvent::kGestureFlingCancel &&
      event.GetType() != blink::WebInputEvent::kGestureDoubleTap) {
    return {nullptr, false, base::nullopt, true, false};
  }

  gfx::PointF transformed_point;
  return FindViewAtLocation(root_view, event.PositionInWidget(),
                            viz::EventSource::MOUSE, &transformed_point);
}

void RenderWidgetHostInputEventRouter::RouteTouchpadGestureEvent(
    RenderWidgetHostViewBase* root_view,
    const blink::WebGestureEvent* event,
    const ui::LatencyInfo& latency) {
  DCHECK_EQ(blink::WebGestureDevice::kTouchpad, event->SourceDevice());
  event_targeter_->FindTargetAndDispatch(root_view, *event, latency);
}

void RenderWidgetHostInputEventRouter::DispatchTouchpadGestureEvent(
    RenderWidgetHostViewBase* root_view,
    RenderWidgetHostViewBase* target,
    const blink::WebGestureEvent& touchpad_gesture_event,
    const ui::LatencyInfo& latency,
    const base::Optional<gfx::PointF>& target_location) {
  // Touchpad gesture flings should be treated as mouse wheels for the purpose
  // of routing.
  if (touchpad_gesture_event.GetType() ==
      blink::WebInputEvent::kGestureFlingStart) {
    if (wheel_target_) {
      blink::WebGestureEvent gesture_fling = touchpad_gesture_event;
      gfx::PointF point_in_target =
          wheel_target_->TransformRootPointToViewCoordSpace(
              gesture_fling.PositionInWidget());
      gesture_fling.SetPositionInWidget(point_in_target);
      wheel_target_->ProcessGestureEvent(gesture_fling, latency);
      last_fling_start_target_ = wheel_target_;
    } else {
      root_view->GestureEventAck(touchpad_gesture_event,
                                 INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS);
    }
    return;
  }

  if (touchpad_gesture_event.GetType() ==
      blink::WebInputEvent::kGestureFlingCancel) {
    if (last_fling_start_target_) {
      last_fling_start_target_->ProcessGestureEvent(touchpad_gesture_event,
                                                    latency);
    } else if (target) {
      target->ProcessGestureEvent(touchpad_gesture_event, latency);
    } else {
      root_view->GestureEventAck(touchpad_gesture_event,
                                 INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS);
    }
    return;
  }

  if (target) {
    touchpad_gesture_target_ = target;

    // Abort any scroll bubbling in progress to avoid double entry.
    CancelScrollBubblingIfConflicting(touchpad_gesture_target_);
  }

  if (!touchpad_gesture_target_) {
    root_view->GestureEventAck(touchpad_gesture_event,
                               INPUT_EVENT_ACK_STATE_NO_CONSUMER_EXISTS);
    return;
  }

  blink::WebGestureEvent gesture_event = touchpad_gesture_event;
  gfx::PointF point_in_target;
  if (target_location) {
    point_in_target = target_location.value();
  } else {
    point_in_target =
        touchpad_gesture_target_->TransformRootPointToViewCoordSpace(
            gesture_event.PositionInWidget());
  }
  gesture_event.SetPositionInWidget(point_in_target);
  touchpad_gesture_target_->ProcessGestureEvent(gesture_event, latency);

  if (touchpad_gesture_event.GetType() ==
          blink::WebInputEvent::kGesturePinchEnd ||
      touchpad_gesture_event.GetType() ==
          blink::WebInputEvent::kGestureDoubleTap) {
    touchpad_gesture_target_ = nullptr;
  }
}

RenderWidgetHostViewBase*
RenderWidgetHostInputEventRouter::FindViewFromFrameSinkId(
    const viz::FrameSinkId& frame_sink_id) const {
  // TODO(kenrb): There should be a better way to handle hit tests to surfaces
  // that are no longer valid for hit testing. See https://crbug.com/790044.
  auto iter = owner_map_.find(frame_sink_id);
  // If the point hit a Surface whose namspace is no longer in the map, then
  // it likely means the RenderWidgetHostView has been destroyed but its
  // parent frame has not sent a new compositor frame since that happened.
  return iter == owner_map_.end() ? nullptr : iter->second;
}

bool RenderWidgetHostInputEventRouter::ShouldContinueHitTesting(
    RenderWidgetHostViewBase* target_view) const {
  // Determine if |view| has any embedded children that could potentially
  // receive the event.
  auto* widget_host =
      static_cast<RenderWidgetHostImpl*>(target_view->GetRenderWidgetHost());
  std::unique_ptr<RenderWidgetHostIterator> child_widgets(
      widget_host->GetEmbeddedRenderWidgetHosts());
  if (child_widgets->GetNextHost())
    return true;

  return false;
}

std::vector<RenderWidgetHostView*>
RenderWidgetHostInputEventRouter::GetRenderWidgetHostViewsForTests() const {
  std::vector<RenderWidgetHostView*> hosts;
  for (auto entry : owner_map_)
    hosts.push_back(entry.second);

  return hosts;
}

RenderWidgetTargeter*
RenderWidgetHostInputEventRouter::GetRenderWidgetTargeterForTests() {
  return event_targeter_.get();
}

RenderWidgetTargetResult
RenderWidgetHostInputEventRouter::FindTargetSynchronouslyAtPoint(
    RenderWidgetHostViewBase* root_view,
    const gfx::PointF& location) {
  gfx::PointF transformed_pt;
  // The transformed point is already in the return value of FindViewAtLocation.
  return FindViewAtLocation(root_view, location, viz::EventSource::MOUSE,
                            &transformed_pt);
}

RenderWidgetTargetResult
RenderWidgetHostInputEventRouter::FindTargetSynchronously(
    RenderWidgetHostViewBase* root_view,
    const blink::WebInputEvent& event) {
  if (blink::WebInputEvent::IsMouseEventType(event.GetType())) {
    return FindMouseEventTarget(
        root_view, static_cast<const blink::WebMouseEvent&>(event));
  }
  if (event.GetType() == blink::WebInputEvent::kMouseWheel) {
    return FindMouseWheelEventTarget(
        root_view, static_cast<const blink::WebMouseWheelEvent&>(event));
  }
  if (blink::WebInputEvent::IsTouchEventType(event.GetType())) {
    return FindTouchEventTarget(
        root_view, static_cast<const blink::WebTouchEvent&>(event));
  }
  if (blink::WebInputEvent::IsGestureEventType(event.GetType())) {
    auto gesture_event = static_cast<const blink::WebGestureEvent&>(event);
    if (gesture_event.SourceDevice() == blink::WebGestureDevice::kTouchscreen) {
      return FindTouchscreenGestureEventTarget(root_view, gesture_event);
    }
    if (gesture_event.SourceDevice() == blink::WebGestureDevice::kTouchpad) {
      return FindTouchpadGestureEventTarget(root_view, gesture_event);
    }
  }
  NOTREACHED();
  return RenderWidgetTargetResult();
}

void RenderWidgetHostInputEventRouter::SetEventsBeingFlushed(
    bool events_being_flushed) {
  events_being_flushed_ = events_being_flushed;
}

void RenderWidgetHostInputEventRouter::SetTouchscreenGestureTarget(
    RenderWidgetHostViewBase* target,
    bool moved_recently) {
  touchscreen_gesture_target_ = target;
  touchscreen_gesture_target_moved_recently_ = moved_recently;
}

void RenderWidgetHostInputEventRouter::DispatchEventToTarget(
    RenderWidgetHostViewBase* root_view,
    RenderWidgetHostViewBase* target,
    const blink::WebInputEvent& event,
    const ui::LatencyInfo& latency,
    const base::Optional<gfx::PointF>& target_location) {
  if (target && target->ScreenRectIsUnstableFor(event))
    event.SetTargetFrameMovedRecently();
  if (blink::WebInputEvent::IsMouseEventType(event.GetType())) {
    if (target && event.GetType() == blink::WebInputEvent::kMouseDown) {
      mouse_down_post_transformed_coordinate_.SetPoint(target_location->x(),
                                                       target_location->y());
      last_mouse_down_target_ = target;
    }
    DispatchMouseEvent(root_view, target,
                       static_cast<const blink::WebMouseEvent&>(event), latency,
                       target_location);
    return;
  }
  if (event.GetType() == blink::WebInputEvent::kMouseWheel) {
    DispatchMouseWheelEvent(
        root_view, target, static_cast<const blink::WebMouseWheelEvent&>(event),
        latency, target_location);
    return;
  }
  if (blink::WebInputEvent::IsTouchEventType(event.GetType())) {
    auto& touch_event = static_cast<const blink::WebTouchEvent&>(event);
    TouchEventWithLatencyInfo touch_with_latency(touch_event, latency);
    if (touch_emulator_ &&
        touch_emulator_->HandleTouchEvent(touch_with_latency.event)) {
      // We cheat a litle bit here, and assume that we know that even if the
      // target is a RenderWidgetHostViewChildFrame, that it would only try to
      // forward the ack to the root view anyways, so we send it there directly.
      root_view->ProcessAckedTouchEvent(touch_with_latency,
                                        INPUT_EVENT_ACK_STATE_CONSUMED);
      return;
    }
    DispatchTouchEvent(root_view, target, touch_event, latency, target_location,
                       false /* not emulated */);
    return;
  }
  if (blink::WebInputEvent::IsGestureEventType(event.GetType())) {
    auto gesture_event = static_cast<const blink::WebGestureEvent&>(event);
    if (gesture_event.SourceDevice() == blink::WebGestureDevice::kTouchscreen) {
      DispatchTouchscreenGestureEvent(root_view, target, gesture_event, latency,
                                      target_location);
      return;
    }
    if (gesture_event.SourceDevice() == blink::WebGestureDevice::kTouchpad) {
      DispatchTouchpadGestureEvent(root_view, target, gesture_event, latency,
                                   target_location);
      return;
    }
  }
  NOTREACHED();
}

TouchEmulator* RenderWidgetHostInputEventRouter::GetTouchEmulator() {
  if (!touch_emulator_)
    touch_emulator_.reset(new TouchEmulator(this, last_device_scale_factor_));

  return touch_emulator_.get();
}

void RenderWidgetHostInputEventRouter::ForwardEmulatedGestureEvent(
    const blink::WebGestureEvent& event) {
  TRACE_EVENT0("input",
               "RenderWidgetHostInputEventRouter::ForwardEmulatedGestureEvent");
  // It's possible that since |last_emulated_event_root_view_| was set by the
  // outbound touch event that the view may have gone away. Before with dispatch
  // the GestureEvent, confirm the view is still available.
  if (!IsViewInMap(last_emulated_event_root_view_))
    return;
  DispatchTouchscreenGestureEvent(last_emulated_event_root_view_, nullptr,
                                  event, ui::LatencyInfo(),
                                  event.PositionInWidget());
}

void RenderWidgetHostInputEventRouter::ForwardEmulatedTouchEvent(
    const blink::WebTouchEvent& event,
    RenderWidgetHostViewBase* target) {
  TRACE_EVENT0("input",
               "RenderWidgetHostInputEventRouter::ForwardEmulatedTouchEvent");
  // Here we re-use the last root view we saw for a mouse move event, or fall
  // back to using |target| as the root_view if we haven't seen a mouse event;
  // this latter case only happens for injected touch events.
  // TODO(wjmaclean): Why doesn't this class just track its root view?
  DCHECK(IsViewInMap(static_cast<RenderWidgetHostViewBase*>(target)));
  last_emulated_event_root_view_ =
      last_mouse_move_root_view_ ? last_mouse_move_root_view_ : target;

  if (event.GetType() == blink::WebInputEvent::kTouchStart)
    active_touches_ += CountChangedTouchPoints(event);
  blink::WebFloatPoint position_in_widget = event.touches[0].PositionInWidget();
  gfx::PointF transformed_point = target->TransformRootPointToViewCoordSpace(
      gfx::PointF(position_in_widget.x, position_in_widget.y));
  DispatchTouchEvent(last_emulated_event_root_view_, target, event,
                     ui::LatencyInfo(), transformed_point, true /* emulated */);
}

void RenderWidgetHostInputEventRouter::SetCursor(const WebCursor& cursor) {
  if (!last_mouse_move_root_view_)
    return;

  last_device_scale_factor_ =
      last_mouse_move_root_view_->current_device_scale_factor();
  if (auto* cursor_manager = last_mouse_move_root_view_->GetCursorManager()) {
    for (auto it : owner_map_)
      // NOTE(david@vivaldi.com): In Vivaldi we need to check if the device
      // emulation is active for that specific view and then change the cursor
      // accordingly.
      if (!vivaldi::IsVivaldiRunning() ||
          it.second->host()->IsDeviceEmulationActive())
      cursor_manager->UpdateCursor(it.second, cursor);
  }
}

void RenderWidgetHostInputEventRouter::ShowContextMenuAtPoint(
    const gfx::Point& point,
    const ui::MenuSourceType source_type) {
  // It's possible that since |last_mouse_move_target_| was set by the
  // outbound mouse event that the view may have gone away. Before dispatching
  // the context menu, confirm the view is still available.
  if (!IsViewInMap(last_mouse_move_target_))
    return;

  auto* rwhi = static_cast<RenderWidgetHostImpl*>(
      last_mouse_move_target_->GetRenderWidgetHost());
  DCHECK(rwhi);
  rwhi->ShowContextMenuAtPoint(point, source_type);
}

void RenderWidgetHostInputEventRouter::OnAggregatedHitTestRegionListUpdated(
    const viz::FrameSinkId& frame_sink_id,
    const std::vector<viz::AggregatedHitTestRegion>& hit_test_data) {
  for (auto& region : hit_test_data) {
    auto iter = owner_map_.find(region.frame_sink_id);
    if (iter != owner_map_.end())
      iter->second->NotifyHitTestRegionUpdated(region);
  }
}

void RenderWidgetHostInputEventRouter::SetMouseCaptureTarget(
    RenderWidgetHostViewBase* target,
    bool capture) {
  if (touch_emulator_ && touch_emulator_->enabled())
    return;

  if (capture) {
    mouse_capture_target_ = target;
    return;
  }

  if (mouse_capture_target_ == target)
    mouse_capture_target_ = nullptr;
}

RenderWidgetHostImpl*
RenderWidgetHostInputEventRouter::GetMouseCaptureWidgetForTests() const {
  if (mouse_capture_target_)
    return mouse_capture_target_->host();
  return nullptr;
}

}  // namespace content
