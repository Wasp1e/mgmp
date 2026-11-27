#pragma once
#include <cstdint>
#include <string_view>

namespace mgmp {
inline bool house_local_ui_row(std::string_view id) {
    return id == "files tutorial_tokens";
}
inline bool house_shop_request_current(uint64_t age, uint64_t day, uint64_t wanted_day,
                                       uint64_t catalog, uint64_t wanted_catalog,
                                       int32_t count, int32_t wanted_count) {
    return age <= 2000 && day == wanted_day && catalog == wanted_catalog &&
           count > 0 && count == wanted_count;
}
}
