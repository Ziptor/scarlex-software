#include "VisibilityClient.h"

namespace visibility {
namespace {

bool close_enough(core::Vec3 a, core::Vec3 b) noexcept {
    constexpr float tolerance = 0.10f;
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz <= tolerance * tolerance;
}

bool fresh(std::uint64_t now, std::uint64_t timestamp) noexcept {
    return timestamp != 0 && now >= timestamp && now - timestamp <= max_age_ms;
}

}  // namespace

void VisibilityClient::begin_frame() noexcept {
    pending_.count = 0;
    host_ready_ = false;
    const auto now = GetTickCount64();
    if (!channel_.connected() && now >= next_connect_at_) {
        channel_.open(pid_, false);
        next_connect_at_ = now + 1000;
    }
    channel_.try_access([&](const SharedState& state) noexcept {
        if (state.version != protocol_version || !fresh(GetTickCount64(), state.heartbeat)) return;
        host_ready_ = true;
        reply_ = state.reply;
    });
}

Result VisibilityClient::query(const Query& query) noexcept {
    if (!host_ready_) return Result::unknown;
    if (pending_.count < pending_.queries.size()) pending_.queries[pending_.count++] = query;
    if (!reply_.batch.sequence || !fresh(GetTickCount64(), reply_.batch.submitted_at) ||
        reply_.batch.count > reply_.batch.queries.size()) return Result::unknown;
    for (std::size_t i = 0; i < reply_.batch.count; ++i) {
        const auto& previous = reply_.batch.queries[i];
        if (previous.entity == query.entity && previous.zone == query.zone && previous.side == query.side &&
            close_enough(previous.from, query.from) && close_enough(previous.to, query.to)) {
            const auto result = reply_.results[i];
            return result == Result::visible || result == Result::blocked ? result : Result::unknown;
        }
    }
    return Result::unknown;
}

void VisibilityClient::end_frame() noexcept {
    if (!host_ready_ || pending_.count == 0) return;
    channel_.try_access([&](SharedState& state) noexcept {
        const auto now = GetTickCount64();
        if (state.version != protocol_version || !fresh(now, state.heartbeat)) return;
        if (state.request.sequence != state.reply.batch.sequence &&
            fresh(now, state.request.submitted_at)) return;
        pending_.sequence = state.request.sequence + 1;
        pending_.submitted_at = now;
        state.request = pending_;
    });
}

}  // namespace visibility
