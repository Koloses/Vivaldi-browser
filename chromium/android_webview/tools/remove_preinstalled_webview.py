#!/usr/bin/env python
#
# Copyright 2018 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Removes the preinstalled WebView on a device to avoid signature mismatches.

This should only be used by developers. This script will fail on actual user
devices (and this configuration is not recommended for user devices).

The recommended development configuration for Googlers is to satisfy all of the
below:
  1. The device has a Google-provided image.
  2. The device does not have an image based on AOSP.
  3. Set `use_signing_keys = true` in GN args.

If any of the above are not satisfied (or if you're external to Google), you can
use this script to remove the system-image WebView on your device, which will
allow you to install a local WebView build without triggering signature
mismatches (which would otherwise block installing the APK).

After running this script, you should be able to build and install
system_webview_apk.
  * If your device does *not* have an AOSP-based image, you will need to set
    `system_webview_package_name = "com.google.android.webview"` in GN args.
"""

from __future__ import print_function

import argparse
import logging
import os
import sys


sys.path.append(os.path.join(
    os.path.dirname(__file__), os.pardir, os.pardir, 'build', 'android'))
import devil_chromium  # pylint: disable=import-error
from devil.android import device_errors  # pylint: disable=import-error
from devil.android import device_utils  # pylint: disable=import-error
from devil.android.sdk import keyevent  # pylint: disable=import-error
from devil.android.tools import script_common  # pylint: disable=import-error
from devil.android.tools import system_app  # pylint: disable=import-error
from devil.utils import logging_common  # pylint: disable=import-error

WEBVIEW_PACKAGES = ['com.android.webview', 'com.google.android.webview']


def _UnlockDevice(device):
  device.SendKeyEvent(keyevent.KEYCODE_MENU)


def UninstallWebViewSystemImages(device):
  """Uninstalls system images for known WebView packages."""
  print('Removing system images from %s...' % device.serial)
  system_app.RemoveSystemApps(device, WEBVIEW_PACKAGES)
  _UnlockDevice(device)


def UninstallWebViewUpdates(device):
  """Uninstalls updates for WebView packages, if updates exist."""
  print('Uninstalling updates from %s...' % device.serial)
  for webview_package in WEBVIEW_PACKAGES:
    try:
      device.Uninstall(webview_package)
    except device_errors.AdbCommandFailedError:
      # This can happen if the app is on the system image but there are no
      # updates installed on top of that.
      logging.info('No update to uninstall for %s on %s', webview_package,
                   device.serial)


def CheckWebViewIsUninstalled(device):
  """Throws if WebView is still installed."""
  for webview_package in WEBVIEW_PACKAGES:
    paths = device.GetApplicationPaths(webview_package)
    if paths:
      raise device_errors.CommandFailedError(
          '{} is still installed on the device at {}'.format(
              webview_package, paths), device)


def RemovePreinstalledWebViews(device):
  device.EnableRoot()
  UninstallWebViewUpdates(device)
  try:
    UninstallWebViewSystemImages(device)
    CheckWebViewIsUninstalled(device)
  except device_errors.CommandFailedError as e:
    if device.adb.is_emulator:
      # Point the user to documentation, since there's a good chance they can
      # workaround this. Use lots of newlines to make sure this message doesn't
      # get lost.
      logging.error('Did you start the emulator with "-writable-system?"\n'
                    'See https://chromium.googlesource.com/chromium/src/+/'
                    'master/docs/android_emulator.md#writable-system-partition'
                    '\n')
    raise e
  device.SetWebViewFallbackLogic(False)  # Allow standalone WebView on N+


def main():
  parser = argparse.ArgumentParser(description="""
Removes the preinstalled WebView APKs to avoid signature mismatches during
development.
""")

  script_common.AddEnvironmentArguments(parser)
  script_common.AddDeviceArguments(parser)
  logging_common.AddLoggingArguments(parser)

  args = parser.parse_args()
  logging_common.InitializeLogging(args)
  devil_chromium.Initialize(adb_path=args.adb_path)

  devices = device_utils.DeviceUtils.HealthyDevices(device_arg=args.devices)
  device_utils.DeviceUtils.parallel(devices).pMap(RemovePreinstalledWebViews)


if __name__ == '__main__':
  sys.exit(main())
