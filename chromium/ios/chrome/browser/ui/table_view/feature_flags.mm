// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/table_view/feature_flags.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

const base::Feature kCollectionsCardPresentationStyle{
    "CollectionsCardPresentationStyle", base::FEATURE_DISABLED_BY_DEFAULT};

bool IsCollectionsCardPresentationStyleEnabled() {
  return base::FeatureList::IsEnabled(kCollectionsCardPresentationStyle);
}
