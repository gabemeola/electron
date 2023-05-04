#include "electron/discord/discord_video_source.h"

#include "electron/discord/public/discord_video_frame.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/ipc/common/gpu_memory_buffer_support.h"
#include "media/video/gpu_video_accelerator_factories.h"

#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_base.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_copier_gfx.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

namespace {
enum DiscordFrameType {
  DISCORD_FRAME_NATIVE,
  DISCORD_FRAME_I420,
  DISCORD_FRAME_ELECTRON,
};
}  // namespace

namespace WTF {

// Template specializations of [1], needed to be able to pass WTF callbacks
// that have VideoTrackAdapterSettings or gfx::Size parameters across threads.
//
// [1] third_party/blink/renderer/platform/wtf/cross_thread_copier.h.
template <>
struct CrossThreadCopier<void*> : public CrossThreadCopierPassThrough<void*> {
  STATIC_ONLY(CrossThreadCopier);
};

}  // namespace WTF

namespace blink {

std::mutex MediaStreamDiscordVideoSource::stream_mutex_;

class MediaStreamDiscordVideoSource::VideoSourceDelegate
    : public WTF::ThreadSafeRefCounted<VideoSourceDelegate> {
 public:
  VideoSourceDelegate(
      scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
      VideoCaptureDeliverFrameCB new_frame_callback);

  void OnFrame(const std::string& streamId,
               const DiscordFrame& frame,
               DiscordFrameReleaseCB releaseCB,
               void* userData);

 protected:
  friend class WTF::ThreadSafeRefCounted<VideoSourceDelegate>;

  ~VideoSourceDelegate() {}

  void DoRenderFrameOnIOThread(scoped_refptr<media::VideoFrame> video_frame,
                               base::TimeTicks estimated_capture_time);
#ifdef OS_WIN
  void DoNativeFrameOnMediaThread(HANDLE texture,
                                  gfx::Size size,
                                  int64_t timestamp_us,
                                  void* releaseCBVoid,
                                  void* userData);
#endif

  scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;
  scoped_refptr<base::SequencedTaskRunner> media_task_runner_;

  // |frame_callback_| is accessed on the IO thread.
  VideoCaptureDeliverFrameCB frame_callback_;

  media::GpuVideoAcceleratorFactories* gpu_factories_;
  std::unique_ptr<gpu::GpuMemoryBufferSupport> gpu_memory_buffer_support_;
};

MediaStreamDiscordVideoSource::VideoSourceDelegate::VideoSourceDelegate(
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
    VideoCaptureDeliverFrameCB new_frame_callback)
    : io_task_runner_(io_task_runner),
      frame_callback_(std::move(new_frame_callback)),
      gpu_factories_(Platform::Current()->GetGpuFactories()),
      gpu_memory_buffer_support_(new gpu::GpuMemoryBufferSupport()) {
  if (gpu_factories_) {
    media_task_runner_ = gpu_factories_->GetTaskRunner();
  }
}

void MediaStreamDiscordVideoSource::VideoSourceDelegate::
    DoRenderFrameOnIOThread(scoped_refptr<media::VideoFrame> video_frame,
                            base::TimeTicks estimated_capture_time) {
  DCHECK(io_task_runner_->BelongsToCurrentThread());
  TRACE_EVENT0("webrtc", "VideoSourceDelegate::DoRenderFrameOnIOThread");
  frame_callback_.Run(std::move(video_frame), {}, estimated_capture_time);
}

#ifdef OS_WIN
void MediaStreamDiscordVideoSource::VideoSourceDelegate::
    DoNativeFrameOnMediaThread(HANDLE texture,
                               gfx::Size size,
                               int64_t timestamp_us,
                               void* releaseCBVoid,
                               void* userData) {
  DiscordFrameReleaseCB releaseCB =
      reinterpret_cast<DiscordFrameReleaseCB>(releaseCBVoid);
  gfx::GpuMemoryBufferHandle buffer_handle{};
  buffer_handle.type = gfx::DXGI_SHARED_HANDLE;
  buffer_handle.dxgi_handle = base::win::ScopedHandle(texture);

  if (!gpu_factories_) {
    releaseCB(userData);
    return;
  }
  auto* sii = gpu_factories_->SharedImageInterface();
  if (!sii) {
    releaseCB(userData);
    return;
  }
  auto buffer_impl =
      gpu_memory_buffer_support_->CreateGpuMemoryBufferImplFromHandle(
          std::move(buffer_handle), size, gfx::BufferFormat::RGBA_8888,
          gfx::BufferUsage::GPU_READ, base::DoNothing());
  auto mailbox = sii->CreateSharedImage(
      buffer_impl.get(), gpu_factories_->GpuMemoryBufferManager(),
      gfx::ColorSpace::CreateSRGB(), kTopLeft_GrSurfaceOrigin,
      kPremul_SkAlphaType,
      gpu::SHARED_IMAGE_USAGE_GLES2 | gpu::SHARED_IMAGE_USAGE_RASTER |
          gpu::SHARED_IMAGE_USAGE_DISPLAY_READ | gpu::SHARED_IMAGE_USAGE_SCANOUT);
  gpu::MailboxHolder mailbox_holder_array[media::VideoFrame::kMaxPlanes];
  mailbox_holder_array[0] = gpu::MailboxHolder(
      mailbox, sii->GenUnverifiedSyncToken(),
      gpu_factories_->ImageTextureTarget(buffer_impl->GetFormat()));

  scoped_refptr<media::VideoFrame> video_frame =
      media::VideoFrame::WrapExternalGpuMemoryBuffer(
          gfx::Rect(size), size, std::move(buffer_impl), mailbox_holder_array,
          base::DoNothing(), base::Microseconds(timestamp_us));
  PostCrossThreadTask(
      *io_task_runner_.get(), FROM_HERE,
      CrossThreadBindOnce(&VideoSourceDelegate::DoRenderFrameOnIOThread,
                          WrapRefCounted(this), video_frame,
                          base::TimeTicks()));
  video_frame->AddDestructionObserver(ConvertToBaseOnceCallback(
      CrossThreadBindOnce(releaseCB, CrossThreadUnretained(userData))));
}
#endif

MediaStreamDiscordVideoSource::MediaStreamDiscordVideoSource(
    const std::string& streamId)
    : MediaStreamVideoSource(base::ThreadTaskRunnerHandle::Get()),
      stream_id_(streamId) {}

MediaStreamDiscordVideoSource::~MediaStreamDiscordVideoSource() {
  StopSourceImpl();
}

base::WeakPtr<MediaStreamVideoSource>
MediaStreamDiscordVideoSource::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void MediaStreamDiscordVideoSource::StartSourceImpl(
    VideoCaptureDeliverFrameCB frame_callback,
    EncodedVideoFrameCB encoded_frame_callback,
    VideoCaptureCropVersionCB crop_version_callback) {
  delegate_ = base::MakeRefCounted<VideoSourceDelegate>(
      io_task_runner(), std::move(frame_callback));
  {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    if (!by_stream_id_) {
      by_stream_id_ =
          new std::unordered_map<std::string,
                                 scoped_refptr<VideoSourceDelegate>>();
    }
    by_stream_id_->erase(stream_id_);
    by_stream_id_->emplace(stream_id_, delegate_);
  }

  OnStartDone(mojom::MediaStreamRequestResult::OK);
}

void MediaStreamDiscordVideoSource::StopSourceImpl() {
  if (delegate_.get()) {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    auto it = by_stream_id_->find(stream_id_);
    if (it != by_stream_id_->end() && it->second == delegate_) {
      by_stream_id_->erase(it);
    }
    delegate_.reset();
  }
}

void MediaStreamDiscordVideoSource::VideoSourceDelegate::OnFrame(
    const std::string& streamId,
    const DiscordFrame& frame,
    DiscordFrameReleaseCB releaseCB,
    void* userData) {
  gfx::Size size(frame.width, frame.height);
  scoped_refptr<media::VideoFrame> video_frame;

  if (frame.type == DISCORD_FRAME_I420) {
    auto* yuv = &frame.frame.yuv;
    video_frame = media::VideoFrame::WrapExternalYuvData(
        media::PIXEL_FORMAT_I420, size, gfx::Rect(size), size, yuv->y_stride,
        yuv->u_stride, yuv->v_stride, const_cast<uint8_t*>(yuv->y),
        const_cast<uint8_t*>(yuv->u), const_cast<uint8_t*>(yuv->v),
        base::Microseconds(frame.timestamp_us));
#ifdef OS_WIN
  } else if (frame.type == DISCORD_FRAME_NATIVE) {
    if (!gpu_factories_) {
      releaseCB(userData);
      return;
    }
    // shared textures need to be processed on the media task runner,
    // not the IO task runner
    PostCrossThreadTask(
        *media_task_runner_.get(), FROM_HERE,
        CrossThreadBindOnce(
            &VideoSourceDelegate::DoNativeFrameOnMediaThread,
            WrapRefCounted(this),
            CrossThreadUnretained(frame.frame.texture_handle), size,
            frame.timestamp_us,
            CrossThreadUnretained(reinterpret_cast<void*>(releaseCB)),
            CrossThreadUnretained(userData)));
    return;
#endif
  } else if (frame.type == DISCORD_FRAME_ELECTRON) {
    discord::media::electron::ElectronPointer<
        discord::media::electron::IElectronVideoFrameMedia>
        electron_frame_media;

    if (ELECTRON_VIDEO_SUCCEEDED(frame.frame.electron->QueryInterface(
            discord::media::electron::IElectronVideoFrameMedia::IID,
            electron_frame_media.Receive<void>()))) {
      video_frame = electron_frame_media->GetMediaFrame();
    }
  }

  if (!video_frame && userData) {
    // currently unsupported type or WrapExternalYuvData failed
    releaseCB(userData);
    return;
  }
  media::VideoRotation rotation;
  switch (frame.rotation) {
    case discord::media::electron::kRotation0:
      rotation = media::VIDEO_ROTATION_0;
      break;
    case discord::media::electron::kRotation90:
      rotation = media::VIDEO_ROTATION_90;
      break;
    case discord::media::electron::kRotation180:
      rotation = media::VIDEO_ROTATION_180;
      break;
    case discord::media::electron::kRotation270:
      rotation = media::VIDEO_ROTATION_270;
      break;
  }
  video_frame->metadata().transformation = media::VideoTransformation(rotation);

  PostCrossThreadTask(
      *io_task_runner_.get(), FROM_HERE,
      CrossThreadBindOnce(&VideoSourceDelegate::DoRenderFrameOnIOThread,
                          WrapRefCounted(this), video_frame,
                          base::TimeTicks()));

  if (userData) {
    video_frame->AddDestructionObserver(ConvertToBaseOnceCallback(
        CrossThreadBindOnce(releaseCB, CrossThreadUnretained(userData))));
  }
}

// static
void MediaStreamDiscordVideoSource::OnFrame(const std::string& streamId,
                                            const DiscordFrame& frame,
                                            DiscordFrameReleaseCB releaseCB,
                                            void* userData) {
  std::lock_guard<std::mutex> lock(stream_mutex_);
  if (!by_stream_id_) {
    return;
  }
  auto it = by_stream_id_->find(streamId);
  if (it != by_stream_id_->end()) {
    scoped_refptr<VideoSourceDelegate> delegate = it->second;
    delegate->OnFrame(streamId, frame, releaseCB, userData);
  }
}

std::unordered_map<
    std::string,
    scoped_refptr<MediaStreamDiscordVideoSource::VideoSourceDelegate>>*
    MediaStreamDiscordVideoSource::by_stream_id_;

}  // namespace blink

extern "C" {
#ifdef OS_WIN
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
    void DeliverDiscordFrame(const char* streamId,
                             const blink::DiscordFrame& frame,
                             blink::DiscordFrameReleaseCB releaseCB,
                             void* userData) {
  blink::MediaStreamDiscordVideoSource::OnFrame(streamId, frame, releaseCB,
                                                userData);
}
}
