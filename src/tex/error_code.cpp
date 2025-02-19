#include "btu/tex/error_code.hpp"

#include <btu/tex/dxtex.hpp>

#include <system_error>

#ifdef _WIN32
#include <array> // only used on windows
#endif

namespace btu::tex {
auto TextureErrCategory::name() const noexcept -> const char *
{
    return "btu::tex error";
}

auto TextureErrCategory::message(int ev) const -> std::string
{
    switch (static_cast<TextureErr>(ev))
    {
        case TextureErr::Success: return "no error";
        case TextureErr::Unknown: return "default error";
        case TextureErr::BadInput: return "invalid input";
        case TextureErr::MemoryAllocation: return "memory allocation failure";
        case TextureErr::WriteFailure: return "write failure";
        case TextureErr::ReadFailure: return "read failure";
        default: return "(unrecognized error)";
    }
}

auto make_error_code(TextureErr e) -> std::error_code
{
    return {static_cast<int>(e), k_texture_err_category};
}

auto FailureSourceCategory::name() const noexcept -> const char *
{
    return "btu::tex failure-source";
}

auto FailureSourceCategory::message(int ev) const -> std::string
{
    switch (static_cast<FailureSource>(ev))
    {
        case FailureSource::BadUserInput: return "invalid user request";
        case FailureSource::SystemError: return "internal error";
        default: return "(unrecognized condition)";
    }
}

auto FailureSourceCategory::equivalent(const std::error_code &ec, int cond) const noexcept -> bool
{
    switch (static_cast<FailureSource>(cond))
    {
        case FailureSource::BadUserInput: return ec == TextureErr::BadInput;
        case FailureSource::SystemError:
            return ec == TextureErr::Unknown || ec == TextureErr::MemoryAllocation;
        default: return false;
    }
}

auto make_error_condition(FailureSource e) -> std::error_condition
{
    return {static_cast<int>(e), k_failure_source_category};
}

auto error_from_hresult(int64_t hr, std::error_code default_err, std::source_location loc) -> Error
{
    const auto make_error = [&](auto win32_code) {
        return Error(std::error_code(win32_code, std::system_category()), loc);
    };

    // Most important DirectXTex error codes
    // We only have access to them on Windows
#ifdef _WIN32
    constexpr auto known_codes = std::to_array({ERROR_ARITHMETIC_OVERFLOW,
                                                ERROR_CANNOT_MAKE,
                                                ERROR_FILE_TOO_LARGE,
                                                ERROR_HANDLE_EOF,
                                                ERROR_INVALID_DATA,
                                                ERROR_NOT_SUPPORTED});

    const auto it = std::ranges::find_if(known_codes, [&](auto err) { return HRESULT_FROM_WIN32(err) == hr; });
    if (it != known_codes.end())
        return make_error(*it);
#endif

    // Didn't work. Does this code come from HRESULT_FROM_WIN32?
    constexpr auto win32_valid = 0x80070000L; // MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, 0)
    constexpr auto mask        = 0xFFFF0000U;
    if ((static_cast<uint64_t>(hr) & mask) == win32_valid)
    {
        // Could have come from many values, but we choose this one
        return make_error(static_cast<int32_t>(static_cast<uint64_t>(hr) & 0xFFFFU));
    }
    if (hr == 0) // S_OK
        return Error(TextureErr::Success, loc);

    // It doesn't. We just return the default error
    return Error(default_err, loc);
}

} // namespace btu::tex
