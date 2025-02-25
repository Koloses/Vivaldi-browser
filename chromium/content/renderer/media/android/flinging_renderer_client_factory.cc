// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/android/flinging_renderer_client_factory.h"

#include <memory>
#include <string>
#include <utility>

#include "base/logging.h"
#include "content/renderer/media/android/flinging_renderer_client.h"
#include "media/mojo/clients/mojo_renderer.h"
#include "media/mojo/clients/mojo_renderer_factory.h"
#include "media/mojo/interfaces/renderer_extensions.mojom.h"

namespace content {

FlingingRendererClientFactory::FlingingRendererClientFactory(
    std::unique_ptr<media::MojoRendererFactory> mojo_flinging_factory,
    std::unique_ptr<media::RemotePlaybackClientWrapper> remote_playback_client)
    : mojo_flinging_factory_(std::move(mojo_flinging_factory)),
      remote_playback_client_(std::move(remote_playback_client)) {}

FlingingRendererClientFactory::~FlingingRendererClientFactory() = default;

std::unique_ptr<media::Renderer> FlingingRendererClientFactory::CreateRenderer(
    const scoped_refptr<base::SingleThreadTaskRunner>& media_task_runner,
    const scoped_refptr<base::TaskRunner>& /* worker_task_runner */,
    media::AudioRendererSink* /* audio_renderer_sink */,
    media::VideoRendererSink* video_renderer_sink,
    const media::RequestOverlayInfoCB& /* request_overlay_info_cb */,
    const gfx::ColorSpace& /* target_color_space */,
    bool use_platform_media_pipeline) {
  DCHECK(IsFlingingActive());
  DCHECK(remote_play_state_change_cb_);

  // Used to send messages from the FlingingRenderer (Browser process),
  // to the FlingingRendererClient (Renderer process). The
  // |client_extension_request| will be bound in FlingingRendererClient.
  media::mojom::FlingingRendererClientExtensionPtr client_extension_ptr;
  auto client_extension_request = mojo::MakeRequest(&client_extension_ptr);

  auto mojo_renderer = mojo_flinging_factory_->CreateFlingingRenderer(
      GetActivePresentationId(), std::move(client_extension_ptr),
      media_task_runner, video_renderer_sink);

  return std::make_unique<FlingingRendererClient>(
      std::move(client_extension_request), media_task_runner,
      std::move(mojo_renderer), remote_play_state_change_cb_);
}

void FlingingRendererClientFactory::SetRemotePlayStateChangeCB(
    media::RemotePlayStateChangeCB callback) {
  remote_play_state_change_cb_ = std::move(callback);
}

std::string FlingingRendererClientFactory::GetActivePresentationId() {
  return remote_playback_client_->GetActivePresentationId();
}

bool FlingingRendererClientFactory::IsFlingingActive() {
  return !GetActivePresentationId().empty();
}

}  // namespace content
