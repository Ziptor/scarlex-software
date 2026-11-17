#pragma once

#include "visibility/Channel.h"

namespace visibility {

class VisibilityClient final {
public:
    explicit VisibilityClient(DWORD pid) noexcept : pid_(pid) {}
    void begin_frame() noexcept;
    [[nodiscard]] Result query(const Query& query) noexcept;
    void end_frame() noexcept;

private:
    DWORD pid_;
    Channel channel_;
    std::uint64_t next_connect_at_{};
    bool host_ready_{};
    Batch pending_{};
    Reply reply_{};
};

}  // namespace visibility
