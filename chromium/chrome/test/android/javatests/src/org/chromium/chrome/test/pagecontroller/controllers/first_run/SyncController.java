// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.test.pagecontroller.controllers.first_run;

import org.chromium.chrome.R;
import org.chromium.chrome.test.pagecontroller.controllers.PageController;
import org.chromium.chrome.test.pagecontroller.utils.IUi2Locator;
import org.chromium.chrome.test.pagecontroller.utils.Ui2Locators;

/**
 * Sync Dialog (part of First Run Experience) Page Controller.
 */
public class SyncController extends PageController {
    private final static IUi2Locator LOCATOR_SYNC_CONTROLLER =
            Ui2Locators.withResEntries(R.id.signin_sync_title);
    private final static IUi2Locator LOCATOR_NO_THANKS =
            Ui2Locators.withResEntries(R.id.negative_button);

    private static final SyncController sInstance = new SyncController();
    private SyncController() {}
    public static SyncController getInstance() {
        return sInstance;
    }

    public void clickNoThanks() {
        mUtils.click(LOCATOR_NO_THANKS);
    }

    @Override
    public boolean isCurrentPageThis() {
        return mLocatorHelper.isOnScreen(LOCATOR_SYNC_CONTROLLER);
    }
}
