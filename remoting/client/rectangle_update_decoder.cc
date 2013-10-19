// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/client/rectangle_update_decoder.h"

#include "base/bind.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/single_thread_task_runner.h"
#include "remoting/base/util.h"
#include "remoting/client/frame_consumer.h"
#include "remoting/codec/video_decoder.h"
#include "remoting/codec/video_decoder_verbatim.h"
#include "remoting/codec/video_decoder_vpx.h"
#include "remoting/protocol/session_config.h"
#include "third_party/libyuv/include/libyuv/convert_argb.h"
#include "third_party/webrtc/modules/desktop_capture/desktop_frame.h"

using base::Passed;
using remoting::protocol::ChannelConfig;
using remoting::protocol::SessionConfig;

namespace remoting {

// This class wraps a VideoDecoder and byte-swaps the pixels for compatibility
// with the android.graphics.Bitmap class.
// TODO(lambroslambrou): Refactor so that the VideoDecoder produces data
// in the right byte-order, instead of swapping it here.
class RgbToBgrVideoDecoderFilter : public VideoDecoder {
 public:
  RgbToBgrVideoDecoderFilter(scoped_ptr<VideoDecoder> parent)
      : parent_(parent.Pass()) {
  }

  virtual void Initialize(const webrtc::DesktopSize& screen_size) OVERRIDE {
    parent_->Initialize(screen_size);
  }

  virtual bool DecodePacket(const VideoPacket& packet) OVERRIDE {
    return parent_->DecodePacket(packet);
  }

  virtual void Invalidate(const webrtc::DesktopSize& view_size,
                          const webrtc::DesktopRegion& region) OVERRIDE {
    return parent_->Invalidate(view_size, region);
  }

  virtual void RenderFrame(const webrtc::DesktopSize& view_size,
                           const webrtc::DesktopRect& clip_area,
                           uint8* image_buffer,
                           int image_stride,
                           webrtc::DesktopRegion* output_region) OVERRIDE {
    parent_->RenderFrame(view_size, clip_area, image_buffer, image_stride,
                         output_region);

    for (webrtc::DesktopRegion::Iterator i(*output_region); !i.IsAtEnd();
         i.Advance()) {
      webrtc::DesktopRect rect = i.rect();
      uint8* pixels = image_buffer + (rect.top() * image_stride) +
        (rect.left() * kBytesPerPixel);
      libyuv::ABGRToARGB(pixels, image_stride, pixels, image_stride,
                         rect.width(), rect.height());
    }
  }

  virtual const webrtc::DesktopRegion* GetImageShape() OVERRIDE {
    return parent_->GetImageShape();
  }

 private:
  scoped_ptr<VideoDecoder> parent_;
};

RectangleUpdateDecoder::RectangleUpdateDecoder(
    scoped_refptr<base::SingleThreadTaskRunner> main_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> decode_task_runner,
    scoped_refptr<FrameConsumerProxy> consumer)
    : main_task_runner_(main_task_runner),
      decode_task_runner_(decode_task_runner),
      consumer_(consumer),
      paint_scheduled_(false),
      latest_sequence_number_(0) {
}

RectangleUpdateDecoder::~RectangleUpdateDecoder() {
}

void RectangleUpdateDecoder::Initialize(const SessionConfig& config) {
  if (!decode_task_runner_->BelongsToCurrentThread()) {
    decode_task_runner_->PostTask(
        FROM_HERE, base::Bind(&RectangleUpdateDecoder::Initialize, this,
                              config));
    return;
  }

  // Initialize decoder based on the selected codec.
  ChannelConfig::Codec codec = config.video_config().codec;
  if (codec == ChannelConfig::CODEC_VERBATIM) {
    decoder_.reset(new VideoDecoderVerbatim());
  } else if (codec == ChannelConfig::CODEC_VP8) {
    decoder_ = VideoDecoderVpx::CreateForVP8();
  } else {
    NOTREACHED() << "Invalid Encoding found: " << codec;
  }

  if (consumer_->GetPixelFormat() == FrameConsumer::FORMAT_RGBA) {
    scoped_ptr<VideoDecoder> wrapper(
        new RgbToBgrVideoDecoderFilter(decoder_.Pass()));
    decoder_ = wrapper.Pass();
  }
}

void RectangleUpdateDecoder::DecodePacket(scoped_ptr<VideoPacket> packet,
                                          const base::Closure& done) {
  DCHECK(decode_task_runner_->BelongsToCurrentThread());

  base::ScopedClosureRunner done_runner(done);

  bool decoder_needs_reset = false;
  bool notify_size_or_dpi_change = false;

  // If the packet includes screen size or DPI information, store them.
  if (packet->format().has_screen_width() &&
      packet->format().has_screen_height()) {
    webrtc::DesktopSize source_size(packet->format().screen_width(),
                                    packet->format().screen_height());
    if (!source_size_.equals(source_size)) {
      source_size_ = source_size;
      decoder_needs_reset = true;
      notify_size_or_dpi_change = true;
    }
  }
  if (packet->format().has_x_dpi() && packet->format().has_y_dpi()) {
    webrtc::DesktopVector source_dpi(packet->format().x_dpi(),
                                     packet->format().y_dpi());
    if (!source_dpi.equals(source_dpi_)) {
      source_dpi_ = source_dpi;
      notify_size_or_dpi_change = true;
    }
  }

  // If we've never seen a screen size, ignore the packet.
  if (source_size_.is_empty())
    return;

  if (decoder_needs_reset)
    decoder_->Initialize(source_size_);
  if (notify_size_or_dpi_change)
    consumer_->SetSourceSize(source_size_, source_dpi_);

  if (decoder_->DecodePacket(*packet.get())) {
    SchedulePaint();
  } else {
    LOG(ERROR) << "DecodePacket() failed.";
  }
}

void RectangleUpdateDecoder::SchedulePaint() {
  if (paint_scheduled_)
    return;
  paint_scheduled_ = true;
  decode_task_runner_->PostTask(
      FROM_HERE, base::Bind(&RectangleUpdateDecoder::DoPaint, this));
}

void RectangleUpdateDecoder::DoPaint() {
  DCHECK(paint_scheduled_);
  paint_scheduled_ = false;

  // If the view size is empty or we have no output buffers ready, return.
  if (buffers_.empty() || view_size_.is_empty())
    return;

  // If no Decoder is initialized, or the host dimensions are empty, return.
  if (!decoder_.get() || source_size_.is_empty())
    return;

  // Draw the invalidated region to the buffer.
  webrtc::DesktopFrame* buffer = buffers_.front();
  webrtc::DesktopRegion output_region;
  decoder_->RenderFrame(view_size_, clip_area_,
                        buffer->data(),
                        buffer->stride(),
                        &output_region);

  // Notify the consumer that painting is done.
  if (!output_region.is_empty()) {
    buffers_.pop_front();
    consumer_->ApplyBuffer(view_size_, clip_area_, buffer, output_region);
  }
}

void RectangleUpdateDecoder::RequestReturnBuffers(const base::Closure& done) {
  if (!decode_task_runner_->BelongsToCurrentThread()) {
    decode_task_runner_->PostTask(
        FROM_HERE, base::Bind(&RectangleUpdateDecoder::RequestReturnBuffers,
        this, done));
    return;
  }

  while (!buffers_.empty()) {
    consumer_->ReturnBuffer(buffers_.front());
    buffers_.pop_front();
  }

  if (!done.is_null())
    done.Run();
}

void RectangleUpdateDecoder::DrawBuffer(webrtc::DesktopFrame* buffer) {
  if (!decode_task_runner_->BelongsToCurrentThread()) {
    decode_task_runner_->PostTask(
        FROM_HERE, base::Bind(&RectangleUpdateDecoder::DrawBuffer,
                              this, buffer));
    return;
  }

  DCHECK(clip_area_.width() <= buffer->size().width() &&
         clip_area_.height() <= buffer->size().height());

  buffers_.push_back(buffer);
  SchedulePaint();
}

void RectangleUpdateDecoder::InvalidateRegion(
    const webrtc::DesktopRegion& region) {
  if (!decode_task_runner_->BelongsToCurrentThread()) {
    decode_task_runner_->PostTask(
        FROM_HERE, base::Bind(&RectangleUpdateDecoder::InvalidateRegion,
                              this, region));
    return;
  }

  if (decoder_.get()) {
    decoder_->Invalidate(view_size_, region);
    SchedulePaint();
  }
}

void RectangleUpdateDecoder::SetOutputSizeAndClip(
    const webrtc::DesktopSize& view_size,
    const webrtc::DesktopRect& clip_area) {
  if (!decode_task_runner_->BelongsToCurrentThread()) {
    decode_task_runner_->PostTask(
        FROM_HERE, base::Bind(&RectangleUpdateDecoder::SetOutputSizeAndClip,
                              this, view_size, clip_area));
    return;
  }

  // The whole frame needs to be repainted if the scaling factor has changed.
  if (!view_size_.equals(view_size) && decoder_.get()) {
    webrtc::DesktopRegion region;
    region.AddRect(webrtc::DesktopRect::MakeSize(view_size));
    decoder_->Invalidate(view_size, region);
  }

  if (!view_size_.equals(view_size) ||
      !clip_area_.equals(clip_area)) {
    view_size_ = view_size;
    clip_area_ = clip_area;

    // Return buffers that are smaller than needed to the consumer for
    // reuse/reallocation.
    std::list<webrtc::DesktopFrame*>::iterator i = buffers_.begin();
    while (i != buffers_.end()) {
      if ((*i)->size().width() < clip_area_.width() ||
          (*i)->size().height() < clip_area_.height()) {
        consumer_->ReturnBuffer(*i);
        i = buffers_.erase(i);
      } else {
        ++i;
      }
    }

    SchedulePaint();
  }
}

const webrtc::DesktopRegion* RectangleUpdateDecoder::GetBufferShape() {
  return decoder_->GetImageShape();
}

void RectangleUpdateDecoder::ProcessVideoPacket(scoped_ptr<VideoPacket> packet,
                                                const base::Closure& done) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  // If the video packet is empty then drop it. Empty packets are used to
  // maintain activity on the network.
  if (!packet->has_data() || packet->data().size() == 0) {
    done.Run();
    return;
  }

  // Add one frame to the counter.
  stats_.video_frame_rate()->Record(1);

  // Record other statistics received from host.
  stats_.video_bandwidth()->Record(packet->data().size());
  if (packet->has_capture_time_ms())
    stats_.video_capture_ms()->Record(packet->capture_time_ms());
  if (packet->has_encode_time_ms())
    stats_.video_encode_ms()->Record(packet->encode_time_ms());
  if (packet->has_client_sequence_number() &&
      packet->client_sequence_number() > latest_sequence_number_) {
    latest_sequence_number_ = packet->client_sequence_number();
    base::TimeDelta round_trip_latency =
        base::Time::Now() -
        base::Time::FromInternalValue(packet->client_sequence_number());
    stats_.round_trip_ms()->Record(round_trip_latency.InMilliseconds());
  }

  // Measure the latency between the last packet being received and presented.
  base::Time decode_start = base::Time::Now();

  base::Closure decode_done = base::Bind(
      &RectangleUpdateDecoder::OnPacketDone, this, decode_start, done);

  decode_task_runner_->PostTask(FROM_HERE, base::Bind(
      &RectangleUpdateDecoder::DecodePacket, this,
      base::Passed(&packet), decode_done));
}

void RectangleUpdateDecoder::OnPacketDone(base::Time decode_start,
                                          const base::Closure& done) {
  if (!main_task_runner_->BelongsToCurrentThread()) {
    main_task_runner_->PostTask(FROM_HERE, base::Bind(
        &RectangleUpdateDecoder::OnPacketDone, this,
        decode_start, done));
    return;
  }

  // Record the latency between the packet being received and presented.
  stats_.video_decode_ms()->Record(
      (base::Time::Now() - decode_start).InMilliseconds());

  done.Run();
}

ChromotingStats* RectangleUpdateDecoder::GetStats() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  return &stats_;
}

}  // namespace remoting
