# Copyright 2016 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import re

from core import perf_benchmark
from telemetry import benchmark
from telemetry import story
from telemetry.timeline import chrome_trace_config
from telemetry.web_perf import timeline_based_measurement
import page_sets


# Track only the high-level GC stats to reduce the data load on dashboard.
_IGNORED_V8_STATS_RE = re.compile(
    r'_(idle_deadline_overrun|percentage_idle|outside_idle)')
_V8_GC_HIGH_LEVEL_STATS_RE = re.compile(r'^v8-gc-('
    r'full-mark-compactor_|'
    r'incremental-finalize_|'
    r'incremental-step_|'
    r'latency-mark-compactor_|'
    r'mark-compactor-|'
    r'memory-mark-compactor_|'
    r'scavenger_|'
    r'total_)')


def V8BrowsingShouldAddValue(name):
  # TODO(crbug.com/775942): The "unknown_browser" is needed because of a race
  # condition in the memory dump manager. Remove this once the bug is fixed.
  if 'memory:chrome' in name or 'memory:unknown_browser' in name:
    return 'renderer_processes' in name
  if 'v8-gc' in name:
    return (_V8_GC_HIGH_LEVEL_STATS_RE.search(name) and
            not _IGNORED_V8_STATS_RE.search(name))
  # Allow all other metrics.
  return True


def AugmentOptionsForV8BrowsingMetrics(options, enable_runtime_call_stats=True):
  categories = [
    # Disable all categories by default.
    '-*',
    # Memory categories.
    'disabled-by-default-memory-infra',
    'toplevel',
    # V8 categories.
    'disabled-by-default-v8.gc',
    'v8',
    'v8.console',
    'webkit.console',
    # Blink categories.
    'blink_gc',
  ]
  options.ExtendTraceCategoryFilter(categories)
  if enable_runtime_call_stats:
    options.AddTraceCategoryFilter('disabled-by-default-v8.runtime_stats')

  options.config.enable_android_graphics_memtrack = True
  # Trigger periodic light memory dumps every 1000 ms.
  memory_dump_config = chrome_trace_config.MemoryDumpConfig()
  memory_dump_config.AddTrigger('light', 1000)
  options.config.chrome_trace_config.SetMemoryDumpConfig(memory_dump_config)

  options.config.chrome_trace_config.SetTraceBufferSizeInKb(400 * 1024)

  metrics = [
    'blinkGcMetric',
    'consoleErrorMetric',
    'expectedQueueingTimeMetric',
    'gcMetric',
    'memoryMetric',
  ]
  options.ExtendTimelineBasedMetric(metrics)
  if enable_runtime_call_stats:
    options.AddTimelineBasedMetric('runtimeStatsTotalMetric')
  return options

class _V8BrowsingBenchmark(perf_benchmark.PerfBenchmark):
  """Base class for V8 browsing benchmarks that measure RuntimeStats,
  eqt, gc and memory metrics.
  See browsing_stories._BrowsingStory for workload description.
  """

  def CreateStorySet(self, options):
    return page_sets.SystemHealthStorySet(platform=self.PLATFORM, case='browse')

  def CreateCoreTimelineBasedMeasurementOptions(self):
    options = timeline_based_measurement.Options()
    AugmentOptionsForV8BrowsingMetrics(options)
    return options

  @classmethod
  def ShouldAddValue(cls, name, from_first_story_run):
    del from_first_story_run  # unused
    return V8BrowsingShouldAddValue(name)


@benchmark.Info(
   emails=['mythria@chromium.org','ulan@chromium.org'],
   component='Blink>JavaScript')
class V8DesktopBrowsingBenchmark(
    _V8BrowsingBenchmark):
  PLATFORM = 'desktop'
  SUPPORTED_PLATFORMS = [story.expectations.ALL_DESKTOP]

  def SetExtraBrowserOptions(self, options):
    options.AppendExtraBrowserArgs(
      '--enable-blink-features=BlinkRuntimeCallStats')

  @classmethod
  def Name(cls):
    return 'v8.browsing_desktop'


@benchmark.Info(
   emails=['mythria@chromium.org','ulan@chromium.org'],
   component='Blink>JavaScript')
class V8MobileBrowsingBenchmark(
    _V8BrowsingBenchmark):
  PLATFORM = 'mobile'
  SUPPORTED_PLATFORMS = [story.expectations.ALL_MOBILE]

  def SetExtraBrowserOptions(self, options):
    options.AppendExtraBrowserArgs(
      '--enable-blink-features=BlinkRuntimeCallStats')

  @classmethod
  def Name(cls):
    return 'v8.browsing_mobile'


@benchmark.Info(
   emails=['mythria@chromium.org','ulan@chromium.org'],
   component='Blink>JavaScript')
class V8FutureDesktopBrowsingBenchmark(
    _V8BrowsingBenchmark):
  PLATFORM = 'desktop'
  SUPPORTED_PLATFORMS = [story.expectations.ALL_DESKTOP]

  def SetExtraBrowserOptions(self, options):
    options.AppendExtraBrowserArgs(
      '--enable-blink-features=BlinkRuntimeCallStats')
    options.AppendExtraBrowserArgs('--enable-features=V8VmFuture')

  @classmethod
  def Name(cls):
    return 'v8.browsing_desktop-future'


@benchmark.Info(
   emails=['mythria@chromium.org','ulan@chromium.org'],
   component='Blink>JavaScript')
class V8FutureMobileBrowsingBenchmark(
    _V8BrowsingBenchmark):
  PLATFORM = 'mobile'
  SUPPORTED_PLATFORMS = [story.expectations.ALL_MOBILE]

  def SetExtraBrowserOptions(self, options):
    options.AppendExtraBrowserArgs(
      '--enable-blink-features=BlinkRuntimeCallStats')
    options.AppendExtraBrowserArgs(
      '--enable-features=V8VmFuture')

  @classmethod
  def Name(cls):
    return 'v8.browsing_mobile-future'
