// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/internal/common/resource_downloader.h"

#include <memory>

#include "base/bind.h"
#include "components/download/public/common/download_url_loader_factory_getter.h"
#include "components/download/public/common/stream_handle_input_stream.h"
#include "components/download/public/common/url_download_request_handle.h"
#include "services/device/public/mojom/constants.mojom.h"
#include "services/device/public/mojom/wake_lock_provider.mojom.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/service_manager/public/cpp/connector.h"

namespace network {
struct ResourceResponseHead;
}

namespace download {

// This object monitors the URLLoaderCompletionStatus change when
// ResourceDownloader is asking |delegate_| whether download can proceed.
class URLLoaderStatusMonitor : public network::mojom::URLLoaderClient {
 public:
  using URLLoaderStatusChangeCallback =
      base::OnceCallback<void(const network::URLLoaderCompletionStatus&)>;
  explicit URLLoaderStatusMonitor(URLLoaderStatusChangeCallback callback);
  ~URLLoaderStatusMonitor() override = default;

  // network::mojom::URLLoaderClient
  void OnReceiveResponse(const network::ResourceResponseHead& head) override {}
  void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                         const network::ResourceResponseHead& head) override {}
  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override {}
  void OnReceiveCachedMetadata(mojo_base::BigBuffer data) override {}
  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {}
  void OnStartLoadingResponseBody(
      mojo::ScopedDataPipeConsumerHandle body) override {}
  void OnComplete(const network::URLLoaderCompletionStatus& status) override;

 private:
  URLLoaderStatusChangeCallback callback_;
  DISALLOW_COPY_AND_ASSIGN(URLLoaderStatusMonitor);
};

URLLoaderStatusMonitor::URLLoaderStatusMonitor(
    URLLoaderStatusChangeCallback callback)
    : callback_(std::move(callback)) {}

void URLLoaderStatusMonitor::OnComplete(
    const network::URLLoaderCompletionStatus& status) {
  std::move(callback_).Run(status);
}

// static
std::unique_ptr<ResourceDownloader> ResourceDownloader::BeginDownload(
    base::WeakPtr<UrlDownloadHandler::Delegate> delegate,
    std::unique_ptr<DownloadUrlParameters> params,
    std::unique_ptr<network::ResourceRequest> request,
    scoped_refptr<download::DownloadURLLoaderFactoryGetter>
        url_loader_factory_getter,
    const URLSecurityPolicy& url_security_policy,
    const GURL& site_url,
    const GURL& tab_url,
    const GURL& tab_referrer_url,
    bool is_new_download,
    bool is_parallel_request,
    std::unique_ptr<service_manager::Connector> connector,
    bool is_background_mode,
    const scoped_refptr<base::SingleThreadTaskRunner>& task_runner) {
  auto downloader = std::make_unique<ResourceDownloader>(
      delegate, std::move(request), params->render_process_host_id(),
      params->render_frame_host_routing_id(), site_url, tab_url,
      tab_referrer_url, is_new_download, task_runner,
      std::move(url_loader_factory_getter), url_security_policy,
      std::move(connector));

  downloader->Start(std::move(params), is_parallel_request, is_background_mode);
  return downloader;
}

// static
std::unique_ptr<ResourceDownloader>
ResourceDownloader::InterceptNavigationResponse(
    base::WeakPtr<UrlDownloadHandler::Delegate> delegate,
    std::unique_ptr<network::ResourceRequest> resource_request,
    int render_process_id,
    int render_frame_id,
    const GURL& site_url,
    const GURL& tab_url,
    const GURL& tab_referrer_url,
    std::vector<GURL> url_chain,
    net::CertStatus cert_status,
    const scoped_refptr<network::ResourceResponse>& response_head,
    mojo::ScopedDataPipeConsumerHandle response_body,
    network::mojom::URLLoaderClientEndpointsPtr url_loader_client_endpoints,
    scoped_refptr<download::DownloadURLLoaderFactoryGetter>
        url_loader_factory_getter,
    const URLSecurityPolicy& url_security_policy,
    std::unique_ptr<service_manager::Connector> connector,
    const scoped_refptr<base::SingleThreadTaskRunner>& task_runner) {
  auto downloader = std::make_unique<ResourceDownloader>(
      delegate, std::move(resource_request), render_process_id, render_frame_id,
      site_url, tab_url, tab_referrer_url, true, task_runner,
      std::move(url_loader_factory_getter), url_security_policy,
      std::move(connector));
  downloader->InterceptResponse(
      std::move(url_chain), cert_status, std::move(response_head),
      std::move(response_body), std::move(url_loader_client_endpoints));
  return downloader;
}

ResourceDownloader::ResourceDownloader(
    base::WeakPtr<UrlDownloadHandler::Delegate> delegate,
    std::unique_ptr<network::ResourceRequest> resource_request,
    int render_process_id,
    int render_frame_id,
    const GURL& site_url,
    const GURL& tab_url,
    const GURL& tab_referrer_url,
    bool is_new_download,
    const scoped_refptr<base::SingleThreadTaskRunner>& task_runner,
    scoped_refptr<download::DownloadURLLoaderFactoryGetter>
        url_loader_factory_getter,
    const URLSecurityPolicy& url_security_policy,
    std::unique_ptr<service_manager::Connector> connector)
    : delegate_(delegate),
      resource_request_(std::move(resource_request)),
      is_new_download_(is_new_download),
      render_process_id_(render_process_id),
      render_frame_id_(render_frame_id),
      site_url_(site_url),
      tab_url_(tab_url),
      tab_referrer_url_(tab_referrer_url),
      delegate_task_runner_(task_runner),
      url_loader_factory_getter_(std::move(url_loader_factory_getter)),
      url_security_policy_(url_security_policy),
      is_content_initiated_(false) {
  RequestWakeLock(connector.get());
}

ResourceDownloader::~ResourceDownloader() = default;

void ResourceDownloader::Start(
    std::unique_ptr<DownloadUrlParameters> download_url_parameters,
    bool is_parallel_request,
    bool is_background_mode) {
  callback_ = download_url_parameters->callback();
  upload_callback_ = download_url_parameters->upload_callback();
  guid_ = download_url_parameters->guid();
  is_content_initiated_ = download_url_parameters->content_initiated();

  // Set up the URLLoaderClient.
  url_loader_client_ = std::make_unique<DownloadResponseHandler>(
      resource_request_.get(), this,
      std::make_unique<DownloadSaveInfo>(
          download_url_parameters->GetSaveInfo()),
      is_parallel_request, download_url_parameters->is_transient(),
      download_url_parameters->fetch_error_body(),
      download_url_parameters->follow_cross_origin_redirects(),
      download_url_parameters->request_headers(),
      download_url_parameters->request_origin(),
      download_url_parameters->download_source(),
      download_url_parameters->ignore_content_length_mismatch(),
      std::vector<GURL>(1, resource_request_->url), is_background_mode);
  network::mojom::URLLoaderClientPtr url_loader_client_ptr;
  url_loader_client_binding_ =
      std::make_unique<mojo::Binding<network::mojom::URLLoaderClient>>(
          url_loader_client_.get(), mojo::MakeRequest(&url_loader_client_ptr));

  // Set up the URLLoader
  network::mojom::URLLoaderRequest url_loader_request =
      mojo::MakeRequest(&url_loader_);
  url_loader_factory_getter_->GetURLLoaderFactory()->CreateLoaderAndStart(
      std::move(url_loader_request),
      0,  // routing_id
      0,  // request_id
      network::mojom::kURLLoadOptionSendSSLInfoWithResponse,
      *(resource_request_.get()), std::move(url_loader_client_ptr),
      net::MutableNetworkTrafficAnnotationTag(
          download_url_parameters->GetNetworkTrafficAnnotation()));
  url_loader_->SetPriority(net::RequestPriority::IDLE,
                           0 /* intra_priority_value */);
}

void ResourceDownloader::InterceptResponse(
    std::vector<GURL> url_chain,
    net::CertStatus cert_status,
    const scoped_refptr<network::ResourceResponse>& response_head,
    mojo::ScopedDataPipeConsumerHandle response_body,
    network::mojom::URLLoaderClientEndpointsPtr endpoints) {
  // Set the URLLoader.
  url_loader_.Bind(std::move(endpoints->url_loader));

  // Create the new URLLoaderClient that will intercept the navigation.
  url_loader_client_ = std::make_unique<DownloadResponseHandler>(
      resource_request_.get(), this, std::make_unique<DownloadSaveInfo>(),
      false, /* is_parallel_request */
      false, /* is_transient */
      false, /* fetch_error_body */
      true,  /* follow_cross_origin_redirects */
      download::DownloadUrlParameters::RequestHeadersType(),
      std::string(), /* request_origin */
      download::DownloadSource::NAVIGATION,
      false /* ignore_content_length_mismatch */, std::move(url_chain),
      false /* is_background_mode */);

  // Simulate on the new URLLoaderClient calls that happened on the old client.
  response_head->head.cert_status = cert_status;
  url_loader_client_->OnReceiveResponse(response_head->head);

  // Available when NavigationImmediateResponse is enabled.
  if (response_body)
    url_loader_client_->OnStartLoadingResponseBody(std::move(response_body));

  // Bind the new client.
  url_loader_client_binding_ =
      std::make_unique<mojo::Binding<network::mojom::URLLoaderClient>>(
          url_loader_client_.get(), std::move(endpoints->url_loader_client));
}

void ResourceDownloader::OnResponseStarted(
    std::unique_ptr<DownloadCreateInfo> download_create_info,
    mojom::DownloadStreamHandlePtr stream_handle) {
  download_create_info->request_handle.reset(new UrlDownloadRequestHandle(
      weak_ptr_factory_.GetWeakPtr(), base::SequencedTaskRunnerHandle::Get()));
  download_create_info->is_new_download = is_new_download_;
  download_create_info->guid = guid_;
  download_create_info->site_url = site_url_;
  download_create_info->tab_url = tab_url_;
  download_create_info->tab_referrer_url = tab_referrer_url_;
  download_create_info->render_process_id = render_process_id_;
  download_create_info->render_frame_id = render_frame_id_;
  download_create_info->has_user_gesture = resource_request_->has_user_gesture;
  download_create_info->is_content_initiated = is_content_initiated_;

  delegate_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &UrlDownloadHandler::Delegate::OnUrlDownloadStarted, delegate_,
          std::move(download_create_info),
          std::make_unique<StreamHandleInputStream>(std::move(stream_handle)),
          std::move(url_loader_factory_getter_), callback_));
}

void ResourceDownloader::OnReceiveRedirect() {
  url_loader_->FollowRedirect(std::vector<std::string>() /* removed_headers */,
                              net::HttpRequestHeaders() /* modified_headers */,
                              base::nullopt);
}

void ResourceDownloader::OnResponseCompleted() {
  Destroy();
}

bool ResourceDownloader::CanRequestURL(const GURL& url) {
  return url_security_policy_
             ? url_security_policy_.Run(render_process_id_, url)
             : true;
}

void ResourceDownloader::OnUploadProgress(uint64_t bytes_uploaded) {
  if (!upload_callback_)
    return;

  delegate_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(upload_callback_, bytes_uploaded));
}

void ResourceDownloader::CancelRequest() {
  Destroy();
}

void ResourceDownloader::Destroy() {
  if (wake_lock_)
    wake_lock_->CancelWakeLock();
  delegate_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&UrlDownloadHandler::Delegate::OnUrlDownloadStopped,
                     delegate_, this));
}

void ResourceDownloader::RequestWakeLock(
    service_manager::Connector* connector) {
  if (!connector)
    return;
  device::mojom::WakeLockProviderPtr wake_lock_provider;
  connector->BindInterface(device::mojom::kServiceName,
                           mojo::MakeRequest(&wake_lock_provider));
  wake_lock_provider->GetWakeLockWithoutContext(
      device::mojom::WakeLockType::kPreventAppSuspension,
      device::mojom::WakeLockReason::kOther, "Download in progress",
      mojo::MakeRequest(&wake_lock_));

  wake_lock_->RequestWakeLock();
}

}  // namespace download
