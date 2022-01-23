#include "btu/bsa/archive.hpp"

#include <btu/common/filesystem.hpp>
#include <btu/common/functional.hpp>

#include <execution>
#include <fstream>

namespace btu::bsa {

[[nodiscard]] auto get_archive_identifier(const UnderlyingArchive &archive) -> std::string_view
{
    const auto visiter = btu::common::overload{
        [](const libbsa::tes3::archive &) { return "tes3"; },
        [](const libbsa::tes4::archive &) { return "tes4"; },
        [](const libbsa::fo4::archive &) { return "fo4"; },
    };
    return std::visit(visiter, archive);
}

Archive::Archive(const Path &a_path)
{
    read(a_path);
}

Archive::Archive(ArchiveVersion a_version, bool a_compressed)
    : version_(a_version)
    , compressed_(a_compressed)
{
    switch (version_)
    {
        case ArchiveVersion::tes3: archive_ = libbsa::tes3::archive{}; break;
        case ArchiveVersion::tes4:
        case ArchiveVersion::fo3:
        case ArchiveVersion::sse:
        {
            libbsa::tes4::archive bsa;
            auto flags = libbsa::tes4::archive_flag::directory_strings
                         | libbsa::tes4::archive_flag::file_strings;
            if (compressed_)
            {
                flags |= libbsa::tes4::archive_flag::compressed;
            }

            bsa.archive_flags(flags);
            archive_ = std::move(bsa);
            break;
        }
        case ArchiveVersion::fo4:
        case ArchiveVersion::fo4dx: archive_ = libbsa::fo4::archive{};
    }
}

auto Archive::read(Path a_path) -> ArchiveVersion
{
    const auto format = libbsa::guess_file_format(a_path).value();

    const auto read = [this, &a_path](auto archive) {
        auto format = archive.read(std::move(a_path));
        archive_    = std::move(archive);
        return static_cast<ArchiveVersion>(format);
    };

    version_ = [&] {
        switch (format)
        {
            case libbsa::file_format::fo4: return read(libbsa::fo4::archive{});
            case libbsa::file_format::tes3:
            {
                libbsa::tes3::archive archive;
                archive.read(std::move(a_path));
                archive_ = std::move(archive);
                return ArchiveVersion::tes3;
            }
            case libbsa::file_format::tes4: return read(libbsa::tes4::archive{});
            default: libbsa::detail::declare_unreachable();
        }
    }();
    return version_;
}

auto Archive::write(Path a_path) -> void
{
    const auto writer = btu::common::overload{
        [&](libbsa::tes3::archive &bsa) { bsa.write(a_path); },
        [&](libbsa::tes4::archive &bsa) {
            const auto version = get_version<libbsa::tes4::version>();
            bsa.write(a_path, version);
        },
        [&](libbsa::fo4::archive &ba2) {
            const auto version = get_version<libbsa::fo4::format>();
            ba2.write(a_path, version);
        },
    };

    std::visit(writer, archive_);
}

void Archive::add_file(const common::Path &a_relative, UnderlyingFile file)
{
    std::scoped_lock lock(mutex_);
    const auto adder = btu::common::overload{
        [&](libbsa::tes3::archive &bsa, libbsa::tes3::file &&f) {
            bsa.insert(a_relative.generic_string(), std::move(f));
        },
        [&](libbsa::tes4::archive &bsa, libbsa::tes4::file &&f) {
            const auto d = [&]() {
                const auto key = a_relative.parent_path().lexically_normal().generic_string();
                if (bsa.find(key) == bsa.end())
                {
                    bsa.insert(key, libbsa::tes4::directory{});
                }
                return bsa[key];
            }();

            d->insert(a_relative.filename().lexically_normal().generic_string(), std::move(f));
        },
        [&](libbsa::fo4::archive &ba2, libbsa::fo4::file &&f) {
            ba2.insert(a_relative.lexically_normal().generic_string(), std::move(f));
        },
        [](auto &&, auto &&) { throw libbsa::exception("Trying to add x type file to y type bsa"); }};

    std::visit(adder, archive_, std::move(file));
}

auto Archive::add_file(const Path &a_root, const Path &a_path) -> void
{
    const auto relative = a_path.lexically_relative(a_root).lexically_normal();
    const auto compress = compressed_ ? libbsa::compression_type::compressed
                                      : libbsa::compression_type::decompressed;

    const auto adder = btu::common::overload{
        [&](libbsa::tes3::archive &) {
            libbsa::tes3::file f;
            f.read(a_path);
            add_file(relative, std::move(f));
        },
        [&, this](libbsa::tes4::archive &) {
            libbsa::tes4::file f;
            const auto version = get_version<libbsa::tes4::version>();
            f.read(a_path, version, libbsa::tes4::compression_codec::normal, compress);
            add_file(relative, std::move(f));
        },
        [&, this](libbsa::fo4::archive &) {
            libbsa::fo4::file f;
            const auto format = get_version<libbsa::fo4::format>();
            f.read(a_path,
                   format,
                   512u,
                   512u, // Default
                   libbsa::fo4::compression_level::normal,
                   compress);
            add_file(relative, std::move(f));
        },
    };
    std::visit(adder, archive_);
}

auto Archive::add_file(const Path &relative, std::vector<std::byte> a_data) -> void
{
    const auto adder = btu::common::overload{
        [&](libbsa::tes3::archive &) {
            libbsa::tes3::file f;
            f.set_data(std::move(a_data));
            add_file(relative, std::move(f));
        },
        [&, this](libbsa::tes4::archive &) {
            libbsa::tes4::file f;
            const auto version = get_version<libbsa::tes4::version>();
            f.set_data(std::move(a_data));
            if (compressed_)
                f.compress(version);
            add_file(relative, std::move(f));
        },
        [&, this](libbsa::fo4::archive &) {
            libbsa::fo4::file f;
            const auto compress = compressed_ ? libbsa::compression_type::compressed
                                              : libbsa::compression_type::decompressed;
            const auto format   = get_version<libbsa::fo4::format>();
            f.read(a_data,
                   format,
                   512u,
                   512u, // Default
                   libbsa::fo4::compression_level::normal,
                   compress);
            add_file(relative, std::move(f));
        },
    };

    std::visit(adder, archive_);
}

auto Archive::unpack(const Path &out_path) -> void
{
    auto make_dir = [](const Path &p) {
        const auto dir = p.parent_path();
        if (!std::filesystem::exists(dir))
            std::filesystem::create_directories(dir);
    };

    auto visiter = btu::common::overload{
        [&](libbsa::tes3::archive &bsa) {
            btu::common::for_each_mt(bsa, [&](auto &&pair) {
                const auto [key, file] = pair;
                const auto path        = out_path / virtual_to_local_path(key);
                make_dir(path);
                file.write(path);
            });
        },
        [&](libbsa::tes4::archive &bsa) {
            for (auto &dir : bsa)
            {
                btu::common::for_each_mt(dir.second, [&](auto &&file) {
                    const auto path = out_path / virtual_to_local_path(dir.first, file.first);
                    const auto ver  = get_version<libbsa::tes4::version>();
                    make_dir(path);
                    file.second.write(path, ver);
                });
            }
        },
        [&](libbsa::fo4::archive &ba2) {
            btu::common::for_each_mt(ba2, [&](auto &&pair) {
                auto &&[key, file] = pair;
                const auto path    = out_path / virtual_to_local_path(key);
                const auto ver     = get_version<libbsa::fo4::format>();
                make_dir(path);
                file.write(path, ver);
            });
        },
    };

    std::visit(visiter, archive_);
}

auto Archive::file_count() const noexcept -> size_t
{
    return std::visit([](const auto &arch) { return arch.size(); }, archive_);
}

template<typename VersionType>
[[nodiscard]] auto Archive::get_version() const -> VersionType
{
    const bool correct = [&] {
        switch (version_)
        {
            case ArchiveVersion::tes3:
            {
                const bool same = std::same_as<VersionType, std::uint32_t>;
                return same && std::holds_alternative<libbsa::tes3::archive>(archive_);
            }
            case ArchiveVersion::tes4:
            case ArchiveVersion::fo3:
            case ArchiveVersion::sse:
            {
                const bool same = std::same_as<VersionType, libbsa::tes4::version>;
                return same && std::holds_alternative<libbsa::tes4::archive>(archive_);
            }
            case ArchiveVersion::fo4:
            case ArchiveVersion::fo4dx:
            {
                const bool same = std::same_as<VersionType, libbsa::fo4::format>;
                return same && std::holds_alternative<libbsa::fo4::archive>(archive_);
            }
            default: return false;
        }
    }();

    if (!correct)
    {
        throw std::runtime_error("Mismatch between requested version and variant type");
    }

    return static_cast<VersionType>(libbsa::detail::to_underlying(version_));
}

template auto Archive::get_version<std::uint32_t>() const -> std::uint32_t;
template auto Archive::get_version<libbsa::tes4::version>() const -> libbsa::tes4::version;
template auto Archive::get_version<libbsa::fo4::format>() const -> libbsa::fo4::format;

auto Archive::get_version() const noexcept -> ArchiveVersion
{
    return version_;
}

auto Archive::get_archive() const noexcept -> const UnderlyingArchive &
{
    return archive_;
}
} // namespace btu::bsa
