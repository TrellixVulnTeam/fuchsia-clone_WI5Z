// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/ring_buffer.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

#include <memory>

#include "src/media/audio/audio_core/mixer/gain.h"

using SafeReadWriteFrameFn = media::audio::BaseRingBuffer::SafeReadWriteFrameFn;

namespace media::audio {
namespace {

template <class RingBufferT>
struct BufferTraits {
  static std::optional<typename RingBufferT::Buffer> MakeBuffer(const Format& format,
                                                                int64_t start_frame,
                                                                int64_t payload_frames,
                                                                void* payload);
};

template <>
struct BufferTraits<ReadableRingBuffer> {
  static std::optional<ReadableStream::Buffer> MakeBuffer(const Format& format, int64_t start_frame,
                                                          int64_t payload_frames, void* payload) {
    // RingBuffers are synchronized only by time, which means there may not be a synchronization
    // happens-before edge connecting the last writer with the current reader, which means we must
    // invalidate our cache to ensure we read the latest data.
    //
    // This is especially important when the RingBuffer represents a buffer shared with HW, because
    // the last write may have happened very recently, increasing the likelihood that our local
    // cache is out-of-date. This is less important when the buffer is used in SW only because it
    // is more likely that the last write happened long enough ago that our cache has been flushed
    // in the interim, however to be strictly correct, a flush is needed in all cases.
    size_t payload_bytes = payload_frames * format.bytes_per_frame();
    zx_cache_flush(payload, payload_bytes, ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE);
    return std::make_optional<ReadableStream::Buffer>(Fixed(start_frame), payload_frames, payload,
                                                      true, StreamUsageMask(), Gain::kUnityGainDb);
  }
};

template <>
struct BufferTraits<WritableRingBuffer> {
  static std::optional<WritableStream::Buffer> MakeBuffer(const Format& format, int64_t start_frame,
                                                          int64_t payload_frames, void* payload) {
    size_t payload_bytes = payload_frames * format.bytes_per_frame();
    return std::make_optional<WritableStream::Buffer>(
        Fixed(start_frame), payload_frames, payload,
        // RingBuffers are synchronized only by time, which means there may not be a synchronization
        // happens-before edge connecting the current writer with the next reader. When this buffer
        // is unlocked, we must flush our cache to ensure we have published the latest data.
        [payload, payload_bytes]() {
          zx_cache_flush(payload, payload_bytes, ZX_CACHE_FLUSH_DATA);
        });
  }
};

template <class RingBufferT>
std::optional<typename RingBufferT::Buffer> LockBuffer(RingBufferT* b,
                                                       SafeReadWriteFrameFn* safe_read_frame,
                                                       SafeReadWriteFrameFn* safe_write_frame,
                                                       int64_t frame, int64_t frame_count,
                                                       bool is_read_lock) {
  auto [ref_time_to_frac_presentation_frame, _] = b->ref_time_to_frac_presentation_frame();
  if (!ref_time_to_frac_presentation_frame.invertible()) {
    return std::nullopt;
  }

  int64_t first_valid_frame;
  int64_t last_valid_frame;  // one past the end
  if (is_read_lock) {
    last_valid_frame = (*safe_read_frame)() + 1;
    first_valid_frame = last_valid_frame - b->frames();
  } else {
    first_valid_frame = (*safe_write_frame)();
    last_valid_frame = first_valid_frame + b->frames();
  }

  if (frame >= last_valid_frame || (frame + frame_count) <= first_valid_frame) {
    return std::nullopt;
  }

  int64_t last_requested_frame = frame + frame_count;

  // 'absolute' here means the frame number not adjusted for the ring size. 'local' is the frame
  // number modulo ring size.
  int64_t first_absolute_frame = std::max(frame, first_valid_frame);

  int64_t first_frame_local = first_absolute_frame % b->frames();
  if (first_frame_local < 0) {
    first_frame_local += b->frames();
  }
  int64_t last_frame_local = std::min(last_requested_frame, last_valid_frame) % b->frames();
  if (last_frame_local <= first_frame_local) {
    last_frame_local = b->frames();
  }

  void* payload = b->virt() + (first_frame_local * b->format().bytes_per_frame());
  int64_t payload_frames = last_frame_local - first_frame_local;

  return BufferTraits<RingBufferT>::MakeBuffer(b->format(), first_absolute_frame, payload_frames,
                                               payload);
}

fbl::RefPtr<RefCountedVmoMapper> MapVmo(const Format& format, zx::vmo vmo, int64_t frame_count,
                                        bool writable) {
  if (!vmo.is_valid()) {
    FX_LOGS(ERROR) << "Invalid VMO!";
    return nullptr;
  }

  if (!format.bytes_per_frame()) {
    FX_LOGS(ERROR) << "Frame size may not be zero!";
    return nullptr;
  }

  uint64_t vmo_size;
  zx_status_t status = vmo.get_size(&vmo_size);

  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to get ring buffer VMO size";
    return nullptr;
  }

  uint64_t size = static_cast<uint64_t>(format.bytes_per_frame()) * frame_count;
  if (size > vmo_size) {
    FX_LOGS(ERROR) << "Driver-reported ring buffer size (" << size << ") is greater than VMO size ("
                   << vmo_size << ")";
    return nullptr;
  }

  // Map the VMO into our address space.
  // TODO(fxbug.dev/35022): How do I specify the cache policy for this mapping?
  zx_vm_option_t flags = ZX_VM_PERM_READ | (writable ? ZX_VM_PERM_WRITE : 0);
  auto vmo_mapper = fbl::MakeRefCounted<RefCountedVmoMapper>();
  status = vmo_mapper->Map(vmo, 0u, size, flags);

  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to map ring buffer VMO";
    return nullptr;
  }

  return vmo_mapper;
}

}  // namespace

BaseRingBuffer::BaseRingBuffer(
    const Format& format,
    fbl::RefPtr<VersionedTimelineFunction> ref_time_to_frac_presentation_frame,
    AudioClock& audio_clock, fbl::RefPtr<RefCountedVmoMapper> vmo_mapper, int64_t frame_count)
    : vmo_mapper_(std::move(vmo_mapper)),
      frames_(frame_count),
      ref_time_to_frac_presentation_frame_(std::move(ref_time_to_frac_presentation_frame)),
      audio_clock_(audio_clock) {
  FX_CHECK(vmo_mapper_->start() != nullptr);
  FX_CHECK(vmo_mapper_->size() >= static_cast<uint64_t>(format.bytes_per_frame() * frame_count));
}

ReadableRingBuffer::ReadableRingBuffer(
    const Format& format,
    fbl::RefPtr<VersionedTimelineFunction> ref_time_to_frac_presentation_frame,
    AudioClock& audio_clock, fbl::RefPtr<RefCountedVmoMapper> vmo_mapper, int64_t frame_count,
    SafeReadWriteFrameFn safe_read_frame)
    : ReadableStream(format),
      BaseRingBuffer(format, ref_time_to_frac_presentation_frame, audio_clock, vmo_mapper,
                     frame_count),
      safe_read_frame_(std::move(safe_read_frame)) {}

WritableRingBuffer::WritableRingBuffer(
    const Format& format,
    fbl::RefPtr<VersionedTimelineFunction> ref_time_to_frac_presentation_frame,
    AudioClock& audio_clock, fbl::RefPtr<RefCountedVmoMapper> vmo_mapper, int64_t frame_count,
    SafeReadWriteFrameFn safe_write_frame)
    : WritableStream(format),
      BaseRingBuffer(format, ref_time_to_frac_presentation_frame, audio_clock, vmo_mapper,
                     frame_count),
      safe_write_frame_(std::move(safe_write_frame)) {}

// static
BaseRingBuffer::Endpoints BaseRingBuffer::AllocateSoftwareBuffer(
    const Format& format,
    fbl::RefPtr<VersionedTimelineFunction> ref_time_to_frac_presentation_frame,
    AudioClock& audio_clock, int64_t frame_count, SafeReadWriteFrameFn safe_write_frame) {
  TRACE_DURATION("audio", "RingBuffer::AllocateSoftwareBuffer");

  size_t vmo_size = frame_count * format.bytes_per_frame();
  zx::vmo vmo;
  zx_status_t status = zx::vmo::create(vmo_size, 0, &vmo);
  FX_CHECK(status == ZX_OK);
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to allocate ring buffer VMO with size " << vmo_size;
    FX_CHECK(false);
  }

  auto vmo_mapper = MapVmo(format, std::move(vmo), frame_count, true);
  FX_DCHECK(vmo_mapper);

  // This is a normal producer/consumer ring buffer:
  //
  //  ----+-+-+----
  //  ... |R|W| ...
  //  ----+-+-+----
  //
  // If the safe_write_frame is at W, then frame W-1 must have been written, therefore the
  // safe_read_frame R = W-1. When this is used as the loopback buffer in an output pipeline,
  // the relationship between R, W and the output presentation frame (PO) is as follows:
  //
  //         |<-- delay -->|
  //  ----+--+-----------+-+-+----
  //  ... |PO|           |R|W| ...
  //  ----+--+-----------+-+-+----
  //
  // Frame PO is the frame currently being played at the output speaker. The delay between
  // W and PO is the "presentation delay" of the output pipeline. When a capture pipeline
  // hooks up to this loopback buffer, the capture pipeline can read any frame at R or
  // ealier. Note that frames are readable *before* they are presented at the speaker.
  // Conceptually, what's actually happening is:
  //
  //         |<-- delay -->|
  //  ----+--+-----------+-+--+----
  //  ... |PO|           |R|W | ...
  //      |  |           | |PC|
  //  ----+--+-----------+-+--+----
  //
  // Where PC is the current presentation frame for the capture pipeline. There's no actual
  // input device; the frame is being "presented" at this software buffer at the moment it
  // is written.
  //
  // In practice, loopback capture pipelines want to use timestamps that match the PTS of
  // the output pipeline. That is, the loopback capture wants to use PO for its timestamps,
  // not PC. This puts us in an unusual scenario where the capture pipeline can read frames
  // before they are presented.
  //
  // This explains why R = W-1 and why we pass ref_time_to_frac_presentation_frame to
  // both sides of the ring buffer.

  auto w =
      std::make_shared<WritableRingBuffer>(format, ref_time_to_frac_presentation_frame, audio_clock,
                                           vmo_mapper, frame_count, std::move(safe_write_frame));

  auto safe_read_frame = [w]() { return w->safe_write_frame_() - 1; };
  auto r =
      std::make_shared<ReadableRingBuffer>(format, ref_time_to_frac_presentation_frame, audio_clock,
                                           vmo_mapper, frame_count, std::move(safe_read_frame));

  return Endpoints{
      .reader = r,
      .writer = w,
  };
}

// static
std::shared_ptr<ReadableRingBuffer> BaseRingBuffer::CreateReadableHardwareBuffer(
    const Format& format,
    fbl::RefPtr<VersionedTimelineFunction> ref_time_to_frac_presentation_frame,
    AudioClock& audio_clock, zx::vmo vmo, int64_t frame_count,
    SafeReadWriteFrameFn safe_read_frame) {
  TRACE_DURATION("audio", "RingBuffer::CreateReadableHardwareBuffer");

  auto vmo_mapper = MapVmo(format, std::move(vmo), frame_count, false);
  FX_DCHECK(vmo_mapper);

  return std::make_shared<ReadableRingBuffer>(
      format, std::move(ref_time_to_frac_presentation_frame), audio_clock, std::move(vmo_mapper),
      frame_count, std::move(safe_read_frame));
}

// static
std::shared_ptr<WritableRingBuffer> BaseRingBuffer::CreateWritableHardwareBuffer(
    const Format& format,
    fbl::RefPtr<VersionedTimelineFunction> ref_time_to_frac_presentation_frame,
    AudioClock& audio_clock, zx::vmo vmo, int64_t frame_count,
    SafeReadWriteFrameFn safe_write_frame) {
  TRACE_DURATION("audio", "RingBuffer::CreateWritableHardwareBuffer");

  auto vmo_mapper = MapVmo(format, std::move(vmo), frame_count, true);
  FX_DCHECK(vmo_mapper);

  return std::make_shared<WritableRingBuffer>(
      format, std::move(ref_time_to_frac_presentation_frame), audio_clock, std::move(vmo_mapper),
      frame_count, std::move(safe_write_frame));
}

std::optional<ReadableStream::Buffer> ReadableRingBuffer::ReadLock(ReadLockContext& ctx,
                                                                   Fixed frame,
                                                                   int64_t frame_count) {
  return LockBuffer<ReadableRingBuffer>(this, &safe_read_frame_, nullptr, frame.Floor(),
                                        frame_count, true);
}

std::optional<WritableStream::Buffer> WritableRingBuffer::WriteLock(int64_t frame,
                                                                    int64_t frame_count) {
  return LockBuffer<WritableRingBuffer>(this, nullptr, &safe_write_frame_, frame, frame_count,
                                        false);
}

BaseStream::TimelineFunctionSnapshot BaseRingBuffer::ReferenceClockToFixedImpl() const {
  if (!ref_time_to_frac_presentation_frame_) {
    return {
        .timeline_function = TimelineFunction(),
        .generation = kInvalidGenerationId,
    };
  }
  auto [timeline_function, generation] = ref_time_to_frac_presentation_frame_->get();
  return {
      .timeline_function = timeline_function,
      .generation = generation,
  };
}

BaseStream::TimelineFunctionSnapshot ReadableRingBuffer::ref_time_to_frac_presentation_frame()
    const {
  return BaseRingBuffer::ReferenceClockToFixedImpl();
}

BaseStream::TimelineFunctionSnapshot WritableRingBuffer::ref_time_to_frac_presentation_frame()
    const {
  return BaseRingBuffer::ReferenceClockToFixedImpl();
}

}  // namespace media::audio
