// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/passwords/password_bubble_view_base.h"

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/passwords/passwords_model_delegate.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/page_action/omnibox_page_action_icon_container_view.h"
#include "chrome/browser/ui/views/passwords/manage_passwords_icon_views.h"
#include "chrome/browser/ui/views/passwords/password_auto_sign_in_view.h"
#include "chrome/browser/ui/views/passwords/password_items_view.h"
#include "chrome/browser/ui/views/passwords/password_pending_view.h"
#include "chrome/browser/ui/views/passwords/password_save_confirmation_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_page_action_icon_container_view.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "components/autofill/core/common/autofill_payments_features.h"
#include "ui/views/controls/button/button.h"

#include "content/public/browser/render_widget_host_view.h"
#include "extensions/api/vivaldi_utilities/vivaldi_utilities_api.h"
#include "ui/vivaldi_browser_window.h"
#include "ui/vivaldi_native_app_window_views.h"

// static
PasswordBubbleViewBase* PasswordBubbleViewBase::g_manage_passwords_bubble_ =
    nullptr;

// static
void PasswordBubbleViewBase::ShowBubble(content::WebContents* web_contents,
                                        DisplayReason reason) {
  Browser* browser = chrome::FindBrowserWithWebContents(web_contents);
  DCHECK(browser);
  DCHECK(browser->window());
  DCHECK(!g_manage_passwords_bubble_ ||
         !g_manage_passwords_bubble_->GetWidget()->IsVisible());

  if (browser->is_vivaldi()) {
    VivaldiBrowserWindow* window =
        static_cast<VivaldiBrowserWindow*>(browser->window());
    VivaldiNativeAppWindowViews* native_view =
        static_cast<VivaldiNativeAppWindowViews*>(window->GetBaseWindow());
    bool is_fullscreen = window->IsFullscreen();

    extensions::VivaldiUtilitiesAPI* api =
        extensions::VivaldiUtilitiesAPI::GetFactoryInstance()->Get(
            browser->profile());

    std::string flow_direction;
    gfx::Rect rect(api->GetDialogPosition(browser->session_id().id(),
                                          "password", &flow_direction));

    // Normalize the rect
    if (rect.x() < 0)
      rect.set_x(0);
    if (rect.y() < 0)
      rect.set_y(0);

    gfx::Point pos =
        flow_direction == "down" ? rect.bottom_right() : rect.top_right();

    ConvertPointToScreen(native_view, &pos);

    PasswordBubbleViewBase* bubble =
      CreateBubble(web_contents, nullptr, pos, reason);
    DCHECK(bubble);
    DCHECK(bubble == g_manage_passwords_bubble_);

    // This will not actually show an arrow but force the code in
    // BubbleFrameView::GetUpdatedWindowBounds() to move the popup to be within
    // the visible rect of the screen.
    bubble->SetArrow(views::BubbleBorder::Arrow::TOP_CENTER);

    g_manage_passwords_bubble_->set_parent_window(
        // Note(bjorgvin@vivaldi.com): Fix for VB-29962
        web_contents->GetRenderWidgetHostView()->GetNativeView());

    views::BubbleDialogDelegateView::CreateBubble(g_manage_passwords_bubble_);

    if (is_fullscreen) {
      g_manage_passwords_bubble_->AdjustForFullscreen(
          native_view->GetBoundsInScreen());
    }
    g_manage_passwords_bubble_->ShowForReason(reason);
    return;
  }

  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  views::View* anchor_view = nullptr;
  if (base::FeatureList::IsEnabled(
          autofill::features::kAutofillEnableToolbarStatusChip)) {
    anchor_view = browser_view->toolbar()->toolbar_page_action_container();
  } else {
    anchor_view = browser_view->toolbar_button_provider()->GetAnchorView();
  }

  PasswordBubbleViewBase* bubble =
      CreateBubble(web_contents, anchor_view, gfx::Point(), reason);
  DCHECK(bubble);
  DCHECK(bubble == g_manage_passwords_bubble_);

  if (anchor_view) {
    views::Button* highlighted_button;
    if (base::FeatureList::IsEnabled(
            autofill::features::kAutofillEnableToolbarStatusChip)) {
      highlighted_button =
          browser_view->toolbar()->toolbar_page_action_container()->GetIconView(
              PageActionIconType::kManagePasswords);
    } else {
      highlighted_button =
          browser_view->toolbar_button_provider()
              ->GetOmniboxPageActionIconContainerView()
              ->GetPageActionIconView(PageActionIconType::kManagePasswords);
    }
    g_manage_passwords_bubble_->SetHighlightedButton(highlighted_button);
  } else {
    g_manage_passwords_bubble_->set_parent_window(
        web_contents->GetNativeView());
  }

  views::BubbleDialogDelegateView::CreateBubble(g_manage_passwords_bubble_);

  // Adjust for fullscreen after creation as it relies on the content size.
  if (!anchor_view) {
    g_manage_passwords_bubble_->AdjustForFullscreen(
        browser_view->GetBoundsInScreen());
  }

  g_manage_passwords_bubble_->ShowForReason(reason);
}

// static
PasswordBubbleViewBase* PasswordBubbleViewBase::CreateBubble(
    content::WebContents* web_contents,
    views::View* anchor_view,
    const gfx::Point& anchor_point,
    DisplayReason reason) {
  PasswordBubbleViewBase* view = nullptr;
  password_manager::ui::State model_state =
      PasswordsModelDelegateFromWebContents(web_contents)->GetState();
  if (model_state == password_manager::ui::MANAGE_STATE) {
    view =
        new PasswordItemsView(web_contents, anchor_view, anchor_point, reason);
  } else if (model_state == password_manager::ui::AUTO_SIGNIN_STATE) {
    view = new PasswordAutoSignInView(web_contents, anchor_view, anchor_point,
                                      reason);
  } else if (model_state == password_manager::ui::CONFIRMATION_STATE) {
    view = new PasswordSaveConfirmationView(web_contents, anchor_view,
                                            anchor_point, reason);
  } else if (model_state ==
                 password_manager::ui::PENDING_PASSWORD_UPDATE_STATE ||
             model_state == password_manager::ui::PENDING_PASSWORD_STATE) {
    view = new PasswordPendingView(web_contents, anchor_view, anchor_point,
                                   reason);
  } else {
    NOTREACHED();
  }

  g_manage_passwords_bubble_ = view;
  return view;
}

// static
void PasswordBubbleViewBase::CloseCurrentBubble() {
  if (g_manage_passwords_bubble_)
    g_manage_passwords_bubble_->GetWidget()->Close();
}

// static
void PasswordBubbleViewBase::ActivateBubble() {
  DCHECK(g_manage_passwords_bubble_);
  DCHECK(g_manage_passwords_bubble_->GetWidget()->IsVisible());
  g_manage_passwords_bubble_->GetWidget()->Activate();
}

const content::WebContents* PasswordBubbleViewBase::GetWebContents() const {
  return model_.GetWebContents();
}

base::string16 PasswordBubbleViewBase::GetWindowTitle() const {
  return model_.title();
}

bool PasswordBubbleViewBase::ShouldShowWindowTitle() const {
  return !model_.title().empty();
}

PasswordBubbleViewBase::PasswordBubbleViewBase(
    content::WebContents* web_contents,
    views::View* anchor_view,
    const gfx::Point& anchor_point,
    DisplayReason reason)
    : LocationBarBubbleDelegateView(anchor_view, anchor_point, web_contents),
      model_(PasswordsModelDelegateFromWebContents(web_contents),
             reason == AUTOMATIC ? ManagePasswordsBubbleModel::AUTOMATIC
                                 : ManagePasswordsBubbleModel::USER_ACTION),
      mouse_handler_(
          std::make_unique<WebContentMouseHandler>(this, web_contents)) {}

PasswordBubbleViewBase::~PasswordBubbleViewBase() {
  if (g_manage_passwords_bubble_ == this)
    g_manage_passwords_bubble_ = nullptr;
}

void PasswordBubbleViewBase::OnWidgetClosing(views::Widget* widget) {
  LocationBarBubbleDelegateView::OnWidgetClosing(widget);
  if (widget != GetWidget())
    return;
  mouse_handler_.reset();
  // It can be the case that a password bubble is being closed while another
  // password bubble is being opened. The metrics recorder can be shared between
  // them and it doesn't understand the sequence [open1, open2, close1, close2].
  // Therefore, we reset the model early (before the bubble destructor) to get
  // the following sequence of events [open1, close1, open2, close2].
  model_.OnBubbleClosing();
}
