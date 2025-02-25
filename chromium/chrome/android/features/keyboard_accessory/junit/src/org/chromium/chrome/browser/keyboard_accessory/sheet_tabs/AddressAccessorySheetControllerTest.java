// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.keyboard_accessory.sheet_tabs;

import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.Matchers.equalTo;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertThat;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.verifyNoMoreInteractions;
import static org.mockito.Mockito.when;

import static org.chromium.chrome.browser.keyboard_accessory.ManualFillingMetricsRecorder.getHistogramForType;
import static org.chromium.chrome.browser.keyboard_accessory.sheet_tabs.AccessorySheetTabMetricsRecorder.UMA_KEYBOARD_ACCESSORY_SHEET_SUGGESTIONS;
import static org.chromium.chrome.browser.keyboard_accessory.sheet_tabs.AccessorySheetTabModel.AccessorySheetDataPiece.Type.ADDRESS_INFO;
import static org.chromium.chrome.browser.keyboard_accessory.sheet_tabs.AccessorySheetTabModel.AccessorySheetDataPiece.Type.TITLE;
import static org.chromium.chrome.browser.keyboard_accessory.sheet_tabs.AccessorySheetTabModel.AccessorySheetDataPiece.getType;

import android.support.v7.widget.RecyclerView;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import org.chromium.base.metrics.RecordHistogram;
import org.chromium.base.metrics.test.ShadowRecordHistogram;
import org.chromium.base.task.test.CustomShadowAsyncTask;
import org.chromium.base.test.BaseRobolectricTestRunner;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.keyboard_accessory.AccessoryTabType;
import org.chromium.chrome.browser.keyboard_accessory.data.KeyboardAccessoryData;
import org.chromium.chrome.browser.keyboard_accessory.data.KeyboardAccessoryData.AccessorySheetData;
import org.chromium.chrome.browser.keyboard_accessory.data.KeyboardAccessoryData.UserInfo;
import org.chromium.chrome.browser.keyboard_accessory.data.PropertyProvider;
import org.chromium.chrome.browser.keyboard_accessory.data.UserInfoField;
import org.chromium.ui.modelutil.ListObservable;

import java.util.HashMap;

/**
 * Controller tests for the address accessory sheet.
 */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE,
        shadows = {CustomShadowAsyncTask.class, ShadowRecordHistogram.class})
public class AddressAccessorySheetControllerTest {
    @Mock
    private RecyclerView mMockView;
    @Mock
    private ListObservable.ListObserver<Void> mMockItemListObserver;

    private AddressAccessorySheetCoordinator mCoordinator;
    private AccessorySheetTabModel mSheetDataPieces;

    @Before
    public void setUp() {
        ShadowRecordHistogram.reset();
        MockitoAnnotations.initMocks(this);
        setAutofillFeature(true);
        mCoordinator = new AddressAccessorySheetCoordinator(RuntimeEnvironment.application, null);
        assertNotNull(mCoordinator);
        mSheetDataPieces = mCoordinator.getSheetDataPiecesForTesting();
    }

    @Test
    public void testCreatesValidTab() {
        KeyboardAccessoryData.Tab tab = mCoordinator.getTab();
        assertNotNull(tab);
        assertNotNull(tab.getIcon());
        assertNotNull(tab.getListener());
    }

    @Test
    public void testSetsViewAdapterOnTabCreation() {
        when(mMockView.getParent()).thenReturn(mMockView);
        KeyboardAccessoryData.Tab tab = mCoordinator.getTab();
        assertNotNull(tab);
        assertNotNull(tab.getListener());
        tab.getListener().onTabCreated(mMockView);
        verify(mMockView).setAdapter(any());
    }

    @Test
    public void testModelNotifiesAboutTabDataChangedByProvider() {
        final PropertyProvider<AccessorySheetData> testProvider = new PropertyProvider<>();

        mSheetDataPieces.addObserver(mMockItemListObserver);
        mCoordinator.registerDataProvider(testProvider);

        // If the coordinator receives a set of initial items, the model should report an insertion.
        testProvider.notifyObservers(
                new AccessorySheetData(AccessoryTabType.ADDRESSES, "Addresses"));
        verify(mMockItemListObserver).onItemRangeInserted(mSheetDataPieces, 0, 1);
        assertThat(mSheetDataPieces.size(), is(1));

        // If the coordinator receives a new set of items, the model should report a change.
        testProvider.notifyObservers(
                new AccessorySheetData(AccessoryTabType.ADDRESSES, "Other Addresses"));
        verify(mMockItemListObserver).onItemRangeChanged(mSheetDataPieces, 0, 1, null);
        assertThat(mSheetDataPieces.size(), is(1));

        // If the coordinator receives an empty set of items, the model should report a deletion.
        testProvider.notifyObservers(null);
        verify(mMockItemListObserver).onItemRangeRemoved(mSheetDataPieces, 0, 1);
        assertThat(mSheetDataPieces.size(), is(0));

        // There should be no notification if no item are reported repeatedly.
        testProvider.notifyObservers(null);
        verifyNoMoreInteractions(mMockItemListObserver);
    }

    @Test
    public void testSplitsTabDataToList() {
        final PropertyProvider<AccessorySheetData> testProvider = new PropertyProvider<>();
        final AccessorySheetData testData =
                new AccessorySheetData(AccessoryTabType.ADDRESSES, "Addresses for this site");
        testData.getUserInfoList().add(new UserInfo("", null));
        testData.getUserInfoList().get(0).addField(
                new UserInfoField("Name", "Name", "", false, null));
        testData.getUserInfoList().get(0).addField(
                new UserInfoField("Street", "Street", "", true, field -> {}));

        mCoordinator.registerDataProvider(testProvider);
        testProvider.notifyObservers(testData);

        assertThat(mSheetDataPieces.size(), is(1));
        assertThat(getType(mSheetDataPieces.get(0)), is(ADDRESS_INFO));
        assertThat(mSheetDataPieces.get(0).getDataPiece(), is(testData.getUserInfoList().get(0)));
    }

    @Test
    public void testUsesTitleElementForEmptyState() {
        final PropertyProvider<AccessorySheetData> testProvider = new PropertyProvider<>();
        final AccessorySheetData testData =
                new AccessorySheetData(AccessoryTabType.ADDRESSES, "No addresses");
        mCoordinator.registerDataProvider(testProvider);

        testProvider.notifyObservers(testData);

        assertThat(mSheetDataPieces.size(), is(1));
        assertThat(getType(mSheetDataPieces.get(0)), is(TITLE));
        assertThat(mSheetDataPieces.get(0).getDataPiece(), is(equalTo("No addresses")));

        // As soon UserInfo is available, discard the title.
        testData.getUserInfoList().add(new UserInfo("", null));
        testData.getUserInfoList().get(0).addField(
                new UserInfoField("Name", "Name", "", false, null));
        testData.getUserInfoList().get(0).addField(
                new UserInfoField("Address", "Address for Name", "", true, field -> {}));
        testProvider.notifyObservers(testData);

        assertThat(mSheetDataPieces.size(), is(1));
        assertThat(getType(mSheetDataPieces.get(0)), is(ADDRESS_INFO));
    }

    @Test
    public void testRecordsSuggestionsImpressionsWhenShown() {
        final PropertyProvider<AccessorySheetData> testProvider = new PropertyProvider<>();
        mCoordinator.registerDataProvider(testProvider);
        assertThat(RecordHistogram.getHistogramTotalCountForTesting(
                           UMA_KEYBOARD_ACCESSORY_SHEET_SUGGESTIONS),
                is(0));
        assertThat(getSuggestionsImpressions(AccessoryTabType.ADDRESSES, 0), is(0));
        assertThat(getSuggestionsImpressions(AccessoryTabType.ALL, 0), is(0));

        // If the tab is shown without interactive item, log "0" samples.
        AccessorySheetData accessorySheetData =
                new AccessorySheetData(AccessoryTabType.ADDRESSES, "No addresses!");
        testProvider.notifyObservers(accessorySheetData);
        mCoordinator.onTabShown();

        assertThat(getSuggestionsImpressions(AccessoryTabType.ADDRESSES, 0), is(1));
        assertThat(getSuggestionsImpressions(AccessoryTabType.ALL, 0), is(1));
    }

    private int getSuggestionsImpressions(@AccessoryTabType int type, int sample) {
        return RecordHistogram.getHistogramValueCountForTesting(
                getHistogramForType(UMA_KEYBOARD_ACCESSORY_SHEET_SUGGESTIONS, type), sample);
    }

    private void setAutofillFeature(boolean enabled) {
        HashMap<String, Boolean> features = new HashMap<>();
        features.put(ChromeFeatureList.AUTOFILL_KEYBOARD_ACCESSORY, enabled);
        ChromeFeatureList.setTestFeatures(features);
    }
}
