// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/network/network_state_list_detailed_view.h"

#include <algorithm>

#include "ash/metrics/user_metrics_recorder.h"
#include "ash/public/cpp/system_tray_client.h"
#include "ash/session/session_controller_impl.h"
#include "ash/shell.h"
#include "ash/strings/grit/ash_strings.h"
#include "ash/system/model/system_tray_model.h"
#include "ash/system/network/tray_network_state_model.h"
#include "ash/system/tray/system_menu_button.h"
#include "ash/system/tray/tri_view.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "chromeos/network/network_connect.h"
#include "net/base/ip_address.h"
#include "third_party/cros_system_api/dbus/service_constants.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/layout_manager.h"
#include "ui/views/widget/widget.h"

using chromeos::network_config::mojom::ConnectionStateType;
using chromeos::network_config::mojom::DeviceStateProperties;
using chromeos::network_config::mojom::DeviceStateType;
using chromeos::network_config::mojom::NetworkStateProperties;
using chromeos::network_config::mojom::NetworkStatePropertiesPtr;
using chromeos::network_config::mojom::NetworkType;

namespace ash {
namespace tray {
namespace {

// Delay between scan requests.
constexpr int kRequestScanDelaySeconds = 10;

// This margin value is used throughout the bubble:
// - margins inside the border
// - horizontal spacing between bubble border and parent bubble border
// - distance between top of this bubble's border and the bottom of the anchor
//   view (horizontal rule).
constexpr int kBubbleMargin = 8;

// Elevation used for the bubble shadow effect (tiny).
constexpr int kBubbleShadowElevation = 2;

bool IsSecondaryUser() {
  SessionControllerImpl* session_controller =
      Shell::Get()->session_controller();
  return session_controller->IsActiveUserSessionStarted() &&
         !session_controller->IsUserPrimary();
}

bool NetworkTypeIsConfigurable(NetworkType type) {
  switch (type) {
    case NetworkType::kVPN:
    case NetworkType::kWiFi:
    case NetworkType::kWiMAX:
      return true;
    case NetworkType::kAll:
    case NetworkType::kCellular:
    case NetworkType::kEthernet:
    case NetworkType::kMobile:
    case NetworkType::kTether:
    case NetworkType::kWireless:
      return false;
  }
  NOTREACHED();
  return false;
}

}  // namespace

// A bubble which displays network info.
class NetworkStateListDetailedView::InfoBubble
    : public views::BubbleDialogDelegateView {
 public:
  InfoBubble(views::View* anchor,
             views::View* content,
             NetworkStateListDetailedView* detailed_view)
      : views::BubbleDialogDelegateView(anchor, views::BubbleBorder::TOP_RIGHT),
        detailed_view_(detailed_view) {
    set_margins(gfx::Insets(kBubbleMargin));
    SetArrow(views::BubbleBorder::NONE);
    set_shadow(views::BubbleBorder::NO_ASSETS);
    set_anchor_view_insets(gfx::Insets(0, 0, kBubbleMargin, 0));
    set_notify_enter_exit_on_child(true);
    SetLayoutManager(std::make_unique<views::FillLayout>());
    AddChildView(content);
  }

  ~InfoBubble() override {
    // The detailed view can be destructed before info bubble is destructed.
    // Call OnInfoBubbleDestroyed only if the detailed view is live.
    if (detailed_view_)
      detailed_view_->OnInfoBubbleDestroyed();
  }

  void OnNetworkStateListDetailedViewIsDeleting() { detailed_view_ = nullptr; }

 private:
  // View:
  gfx::Size CalculatePreferredSize() const override {
    // This bubble should be inset by kBubbleMargin on both left and right
    // relative to the parent bubble.
    const gfx::Size anchor_size = GetAnchorView()->size();
    int contents_width =
        anchor_size.width() - 2 * kBubbleMargin - margins().width();
    return gfx::Size(contents_width, GetHeightForWidth(contents_width));
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    // Like the user switching bubble/menu, hide the bubble when the mouse
    // exits.
    if (detailed_view_)
      detailed_view_->ResetInfoBubble();
  }

  // BubbleDialogDelegateView:
  int GetDialogButtons() const override { return ui::DIALOG_BUTTON_NONE; }

  void OnBeforeBubbleWidgetInit(views::Widget::InitParams* params,
                                views::Widget* widget) const override {
    params->shadow_type = views::Widget::InitParams::SHADOW_TYPE_DROP;
    params->shadow_elevation = kBubbleShadowElevation;
    params->name = "NetworkStateListDetailedView::InfoBubble";
  }

  // Not owned.
  NetworkStateListDetailedView* detailed_view_;

  DISALLOW_COPY_AND_ASSIGN(InfoBubble);
};

//------------------------------------------------------------------------------

// Special layout to overlap the scanning throbber and the info button.
class InfoThrobberLayout : public views::LayoutManager {
 public:
  InfoThrobberLayout() = default;
  ~InfoThrobberLayout() override = default;

  // views::LayoutManager
  void Layout(views::View* host) override {
    gfx::Size max_size(GetMaxChildSize(host));
    // Center each child view within |max_size|.
    for (auto* child : host->children()) {
      if (!child->GetVisible())
        continue;
      gfx::Size child_size = child->GetPreferredSize();
      gfx::Point origin;
      origin.set_x((max_size.width() - child_size.width()) / 2);
      origin.set_y((max_size.height() - child_size.height()) / 2);
      gfx::Rect bounds(origin, child_size);
      bounds.Inset(-host->GetInsets());
      child->SetBoundsRect(bounds);
    }
  }

  gfx::Size GetPreferredSize(const views::View* host) const override {
    gfx::Point origin;
    gfx::Rect rect(origin, GetMaxChildSize(host));
    rect.Inset(-host->GetInsets());
    return rect.size();
  }

 private:
  gfx::Size GetMaxChildSize(const views::View* host) const {
    gfx::Size max_size;
    for (const auto* child : host->children()) {
      if (child->GetVisible())
        max_size.SetToMax(child->GetPreferredSize());
    }
    return max_size;
  }

  DISALLOW_COPY_AND_ASSIGN(InfoThrobberLayout);
};

//------------------------------------------------------------------------------
// NetworkStateListDetailedView

NetworkStateListDetailedView::NetworkStateListDetailedView(
    DetailedViewDelegate* delegate,
    ListType list_type,
    LoginStatus login)
    : TrayDetailedView(delegate),
      list_type_(list_type),
      login_(login),
      model_(Shell::Get()->system_tray_model()->network_state_model()),
      info_button_(nullptr),
      settings_button_(nullptr),
      info_bubble_(nullptr) {}

NetworkStateListDetailedView::~NetworkStateListDetailedView() {
  if (info_bubble_)
    info_bubble_->OnNetworkStateListDetailedViewIsDeleting();
  ResetInfoBubble();
}

void NetworkStateListDetailedView::Update() {
  UpdateNetworkList();
  UpdateHeaderButtons();
  UpdateScanningBar();
  Layout();
}

void NetworkStateListDetailedView::ToggleInfoBubbleForTesting() {
  ToggleInfoBubble();
}

const char* NetworkStateListDetailedView::GetClassName() const {
  return "NetworkStateListDetailedView";
}

void NetworkStateListDetailedView::Init() {
  CreateScrollableList();
  CreateTitleRow(list_type_ == ListType::LIST_TYPE_NETWORK
                     ? IDS_ASH_STATUS_TRAY_NETWORK
                     : IDS_ASH_STATUS_TRAY_VPN);

  Update();

  if (list_type_ == LIST_TYPE_NETWORK && IsWifiEnabled())
    ScanAndStartTimer();
}

void NetworkStateListDetailedView::HandleButtonPressed(views::Button* sender,
                                                       const ui::Event& event) {
  if (sender == info_button_) {
    ToggleInfoBubble();
    return;
  }

  if (sender == settings_button_)
    ShowSettings();
}

void NetworkStateListDetailedView::HandleViewClicked(views::View* view) {
  if (login_ == LoginStatus::LOCKED)
    return;

  std::string guid;
  if (!IsNetworkEntry(view, &guid))
    return;

  model_->cros_network_config()->GetNetworkState(
      guid, base::BindOnce(&NetworkStateListDetailedView::HandleViewClickedImpl,
                           weak_ptr_factory_.GetWeakPtr()));
}

void NetworkStateListDetailedView::HandleViewClickedImpl(
    NetworkStatePropertiesPtr network) {
  if (network) {
    // Attempt a network connection if the network is not connected and:
    // * The network is connectable or
    // * The active user is primary and the network is configurable
    bool can_connect =
        network->connection_state == ConnectionStateType::kNotConnected &&
        (network->connectable ||
         (!IsSecondaryUser() && NetworkTypeIsConfigurable(network->type)));
    if (can_connect) {
      Shell::Get()->metrics()->RecordUserMetricsAction(
          list_type_ == LIST_TYPE_VPN
              ? UMA_STATUS_AREA_CONNECT_TO_VPN
              : UMA_STATUS_AREA_CONNECT_TO_CONFIGURED_NETWORK);
      chromeos::NetworkConnect::Get()->ConnectToNetworkId(network->guid);
      return;
    }
  }
  // If the network is no longer available or not connectable or configurable,
  // show the Settings UI.
  Shell::Get()->metrics()->RecordUserMetricsAction(
      list_type_ == LIST_TYPE_VPN
          ? UMA_STATUS_AREA_SHOW_VPN_CONNECTION_DETAILS
          : UMA_STATUS_AREA_SHOW_NETWORK_CONNECTION_DETAILS);
  Shell::Get()->system_tray_model()->client()->ShowNetworkSettings(
      network ? network->guid : std::string());
}

void NetworkStateListDetailedView::CreateExtraTitleRowButtons() {
  if (login_ == LoginStatus::LOCKED)
    return;

  DCHECK(!info_button_);
  tri_view()->SetContainerVisible(TriView::Container::END, true);

  info_button_ = CreateInfoButton(IDS_ASH_STATUS_TRAY_NETWORK_INFO);
  tri_view()->AddView(TriView::Container::END, info_button_);

  DCHECK(!settings_button_);
  settings_button_ = CreateSettingsButton(IDS_ASH_STATUS_TRAY_NETWORK_SETTINGS);
  tri_view()->AddView(TriView::Container::END, settings_button_);
}

void NetworkStateListDetailedView::ShowSettings() {
  Shell::Get()->metrics()->RecordUserMetricsAction(
      list_type_ == LIST_TYPE_VPN ? UMA_STATUS_AREA_VPN_SETTINGS_OPENED
                                  : UMA_STATUS_AREA_NETWORK_SETTINGS_OPENED);
  CloseBubble();  // Deletes |this|.
  Shell::Get()->system_tray_model()->client()->ShowNetworkSettings(
      std::string());
}

void NetworkStateListDetailedView::UpdateHeaderButtons() {
  if (settings_button_) {
    if (login_ == LoginStatus::NOT_LOGGED_IN) {
      // When not logged in, only enable the settings button if there is a
      // default (i.e. connected or connecting) network to show settings for.
      settings_button_->SetEnabled(model_->default_network());
    } else {
      // Otherwise, enable if showing settings is allowed. There are situations
      // (supervised user creation flow) when the session is started but UI flow
      // continues within login UI, i.e., no browser window is yet available.
      settings_button_->SetEnabled(
          Shell::Get()->session_controller()->ShouldEnableSettings());
    }
  }
}

void NetworkStateListDetailedView::UpdateScanningBar() {
  if (list_type_ != LIST_TYPE_NETWORK)
    return;

  bool is_wifi_enabled = IsWifiEnabled();
  if (is_wifi_enabled && !network_scan_repeating_timer_.IsRunning())
    ScanAndStartTimer();

  if (!is_wifi_enabled && network_scan_repeating_timer_.IsRunning())
    network_scan_repeating_timer_.Stop();

  bool scanning_bar_visible = false;
  if (is_wifi_enabled) {
    const DeviceStateProperties* wifi = model_->GetDevice(NetworkType::kWiFi);
    const DeviceStateProperties* tether =
        model_->GetDevice(NetworkType::kTether);
    scanning_bar_visible =
        (wifi && wifi->scanning) || (tether && tether->scanning);
  }
  ShowProgress(-1, scanning_bar_visible);
}

void NetworkStateListDetailedView::ToggleInfoBubble() {
  if (ResetInfoBubble())
    return;

  info_bubble_ = new InfoBubble(tri_view(), CreateNetworkInfoView(), this);
  views::BubbleDialogDelegateView::CreateBubble(info_bubble_)->Show();
  info_bubble_->NotifyAccessibilityEvent(ax::mojom::Event::kAlert, false);
}

bool NetworkStateListDetailedView::ResetInfoBubble() {
  if (!info_bubble_)
    return false;

  info_bubble_->GetWidget()->Close();
  return true;
}

void NetworkStateListDetailedView::OnInfoBubbleDestroyed() {
  info_bubble_ = nullptr;

  // Widget of info bubble is activated while info bubble is shown. To move
  // focus back to the widget of this view, activate it again here.
  GetWidget()->Activate();
}

views::View* NetworkStateListDetailedView::CreateNetworkInfoView() {
  std::string ipv4_address, ipv6_address;
  const NetworkStateProperties* network = model_->default_network();
  const DeviceStateProperties* device =
      network ? model_->GetDevice(network->type) : nullptr;
  if (device) {
    if (device->ipv4_address)
      ipv4_address = device->ipv4_address->ToString();
    if (device->ipv6_address)
      ipv6_address = device->ipv6_address->ToString();
  }

  std::string ethernet_address, wifi_address;
  if (list_type_ == LIST_TYPE_NETWORK) {
    const DeviceStateProperties* ethernet =
        model_->GetDevice(NetworkType::kEthernet);
    if (ethernet && ethernet->mac_address)
      ethernet_address = *ethernet->mac_address;
    const DeviceStateProperties* wifi = model_->GetDevice(NetworkType::kWiFi);
    if (wifi && wifi->mac_address)
      wifi_address = *wifi->mac_address;
  }

  base::string16 bubble_text;
  auto add_line = [&bubble_text](const std::string& address, int ids) {
    if (!address.empty()) {
      if (!bubble_text.empty())
        bubble_text += base::ASCIIToUTF16("\n");

      bubble_text +=
          l10n_util::GetStringFUTF16(ids, base::UTF8ToUTF16(address));
    }
  };

  add_line(ipv4_address, IDS_ASH_STATUS_TRAY_IP_ADDRESS);
  add_line(ipv6_address, IDS_ASH_STATUS_TRAY_IPV6_ADDRESS);
  add_line(ethernet_address, IDS_ASH_STATUS_TRAY_ETHERNET_ADDRESS);
  add_line(wifi_address, IDS_ASH_STATUS_TRAY_WIFI_ADDRESS);

  // Avoid an empty bubble in the unlikely event that there is no network
  // information at all.
  if (bubble_text.empty())
    bubble_text = l10n_util::GetStringUTF16(IDS_ASH_STATUS_TRAY_NO_NETWORKS);

  auto* label = new views::Label(bubble_text);
  label->SetMultiLine(true);
  label->SetHorizontalAlignment(gfx::ALIGN_TO_HEAD);
  label->SetSelectable(true);
  return label;
}

void NetworkStateListDetailedView::ScanAndStartTimer() {
  CallRequestScan();
  network_scan_repeating_timer_.Start(
      FROM_HERE, base::TimeDelta::FromSeconds(kRequestScanDelaySeconds), this,
      &NetworkStateListDetailedView::CallRequestScan);
}

void NetworkStateListDetailedView::CallRequestScan() {
  if (!IsWifiEnabled())
    return;

  VLOG(1) << "Requesting Network Scan.";
  model_->cros_network_config()->RequestNetworkScan(NetworkType::kWiFi);
  model_->cros_network_config()->RequestNetworkScan(NetworkType::kTether);
}

bool NetworkStateListDetailedView::IsWifiEnabled() {
  return model_->GetDeviceState(NetworkType::kWiFi) ==
         DeviceStateType::kEnabled;
}

}  // namespace tray
}  // namespace ash
