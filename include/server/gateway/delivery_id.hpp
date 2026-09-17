#ifndef GATEWAY_DELIVERY_ID_HPP
#define GATEWAY_DELIVERY_ID_HPP

#include "json.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

namespace gateway
{

// JSON 用十进制字符串承载 uint64，避免 Qt/JavaScript 数值精度损失。
inline std::optional<std::uint64_t> parseDeliveryId(
    const nlohmann::json& payload)
{
    if (!payload.is_object() || !payload.contains("delivery_id") ||
        !payload["delivery_id"].is_string())
    {
        return std::nullopt;
    }
    const auto text = payload["delivery_id"].get<std::string>();
    if (text.empty() || text.size() > 20 ||
        (text.size() > 1 && text.front() == '0'))
    {
        return std::nullopt;
    }

    std::uint64_t id = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || id == 0)
    {
        return std::nullopt;
    }
    return id;
}

} // namespace gateway

#endif
