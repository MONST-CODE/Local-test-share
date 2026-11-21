#pragma once
// A bounded low-latency queue for complete (key) frames. It is a reusable
// pipeline component, not yet connected to browser capture or network transport.
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "../browser/remote_frame.hpp"

namespace plpt::media {

enum class FramePushOutcome : std::uint8_t { Queued, QueuedAfterDroppingOldest };

struct FrameQueueSnapshot {
    std::size_t queued_frames = 0;
    std::size_t queued_bytes = 0;
    std::uint64_t dropped_frames = 0;
    bool closed = false;
};

// A queue belongs to one context/stream for its lifetime. Since v1 frames are
// self-contained keyframes, replacing stale queued frames cannot break a delta
// chain. The byte and count limits are both enforced before retaining a frame.
class BoundedFrameQueue final {
public:
    BoundedFrameQueue(std::size_t maximum_frames, std::size_t maximum_bytes)
        : maximum_frames_(maximum_frames), maximum_bytes_(maximum_bytes) {
        if (maximum_frames_ == 0 || maximum_bytes_ == 0)
            throw std::invalid_argument("frame queue limits must be non-zero");
    }

    BoundedFrameQueue(const BoundedFrameQueue&) = delete;
    BoundedFrameQueue& operator=(const BoundedFrameQueue&) = delete;

    Result<FramePushOutcome> push(VideoFrame frame) {
        auto valid = validate_video_frame(frame);
        if (!valid) return Result<FramePushOutcome>::failure(valid.error());
        const std::size_t bytes = frame.pixels.size();
        if (bytes > maximum_bytes_)
            return Result<FramePushOutcome>::failure({ErrorCode::TooLarge,
                "frame exceeds bounded queue byte limit", "media-queue"});

        std::lock_guard lock(mutex_);
        if (closed_)
            return Result<FramePushOutcome>::failure({ErrorCode::InvalidState,
                "frame queue is closed", "media-queue"});
        if (context_ && (*context_ != frame.header.context || stream_id_ != frame.header.stream_id))
            return Result<FramePushOutcome>::failure({ErrorCode::InvalidArgument,
                "frame belongs to another context or stream", "media-queue"});
        if (last_sequence_ && frame.header.sequence <= *last_sequence_)
            return Result<FramePushOutcome>::failure({ErrorCode::Replayed,
                "frame sequence is not strictly increasing", "media-queue"});

        context_ = frame.header.context;
        stream_id_ = frame.header.stream_id;
        last_sequence_ = frame.header.sequence;
        bool dropped = false;
        while (!frames_.empty() &&
            (frames_.size() >= maximum_frames_ || queued_bytes_ > maximum_bytes_ - bytes)) {
            queued_bytes_ -= frames_.front().pixels.size();
            frames_.pop_front();
            ++dropped_frames_;
            dropped = true;
        }
        queued_bytes_ += bytes;
        frames_.push_back(std::move(frame));
        return Result<FramePushOutcome>::success(dropped
            ? FramePushOutcome::QueuedAfterDroppingOldest : FramePushOutcome::Queued);
    }

    std::optional<VideoFrame> try_pop() {
        std::lock_guard lock(mutex_);
        if (frames_.empty()) return std::nullopt;
        VideoFrame frame = std::move(frames_.front());
        queued_bytes_ -= frame.pixels.size();
        frames_.pop_front();
        return frame;
    }

    // Close is idempotent. Existing queued frames may still be drained with
    // try_pop(); all later pushes fail explicitly.
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }

    FrameQueueSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return {frames_.size(), queued_bytes_, dropped_frames_, closed_};
    }

private:
    const std::size_t maximum_frames_;
    const std::size_t maximum_bytes_;
    mutable std::mutex mutex_;
    std::deque<VideoFrame> frames_;
    std::optional<ContextId> context_;
    std::optional<std::uint64_t> last_sequence_;
    std::uint32_t stream_id_ = 0;
    std::size_t queued_bytes_ = 0;
    std::uint64_t dropped_frames_ = 0;
    bool closed_ = false;
};

} // namespace plpt::media
