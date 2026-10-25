#include <lolview/decompress.hpp>

#include <zlib.h>
#include <zstd.h>

#include <stdexcept>
#include <string>

namespace lolview {
namespace {

std::string count(std::size_t value) { return std::to_string(value); }

}  // namespace

std::vector<std::uint8_t> zstd_decompress(std::span<const std::uint8_t> input,
                                          std::size_t expected_size) {
    if (expected_size == 0) {
        throw std::runtime_error("zstd: TOC declares a zero-length chunk");
    }

    std::vector<std::uint8_t> out(expected_size);

    ZSTD_DStream* stream = ZSTD_createDStream();
    if (stream == nullptr) {
        throw std::runtime_error("zstd: cannot allocate a decompression stream");
    }

    const std::size_t init = ZSTD_initDStream(stream);
    if (ZSTD_isError(init)) {
        const std::string message = ZSTD_getErrorName(init);
        ZSTD_freeDStream(stream);
        throw std::runtime_error("zstd: init failed: " + message);
    }

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    std::size_t produced = 0;
    // Beware: 0 means "frame complete", any other value means bytes still needed.
    std::size_t pending = 1;

    while (in.pos < in.size) {
        const std::size_t room = out.size() - produced;
        if (room == 0) {
            ZSTD_freeDStream(stream);
            throw std::runtime_error("zstd: payload exceeds the size declared in the TOC (" +
                                     count(expected_size) + " bytes)");
        }

        ZSTD_outBuffer ob{out.data() + produced, room, 0};
        pending = ZSTD_decompressStream(stream, &ob, &in);
        if (ZSTD_isError(pending)) {
            const std::string message = ZSTD_getErrorName(pending);
            ZSTD_freeDStream(stream);
            throw std::runtime_error("zstd: " + message);
        }
        produced += ob.pos;

        if (in.pos == in.size && pending != 0) {
            break;  // consumed everything but the frame is incomplete
        }
    }

    ZSTD_freeDStream(stream);

    if (pending != 0) {
        throw std::runtime_error("zstd: truncated payload (frame still needs " + count(pending) +
                                 " bytes, got " + count(input.size()) + ")");
    }
    if (produced != expected_size) {
        throw std::runtime_error("zstd: decoded " + count(produced) + " bytes, TOC declares " +
                                 count(expected_size));
    }
    return out;
}

std::vector<std::uint8_t> gzip_decompress(std::span<const std::uint8_t> input,
                                          std::size_t expected_size) {
    if (expected_size == 0) {
        throw std::runtime_error("gzip: TOC declares a zero-length chunk");
    }

    std::vector<std::uint8_t> out(expected_size);

    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("gzip: inflateInit2 failed");
    }

    zs.next_in = const_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());

    std::size_t produced = 0;
    int rc = Z_OK;

    while (true) {
        zs.next_out = out.data() + produced;
        zs.avail_out = static_cast<uInt>(out.size() - produced);

        rc = inflate(&zs, Z_NO_FLUSH);
        produced = out.size() - zs.avail_out;

        if (rc == Z_STREAM_END) {
            if (zs.avail_in > 0 && produced < out.size()) {
                // Multi-member gzip: reset and decode the next member.
                if (inflateReset2(&zs, 16 + MAX_WBITS) != Z_OK) {
                    inflateEnd(&zs);
                    throw std::runtime_error("gzip: inflateReset2 failed between members");
                }
                continue;
            }
            inflateEnd(&zs);
            if (produced != expected_size) {
                throw std::runtime_error("gzip: decoded " + count(produced) +
                                         " bytes, TOC declares " + count(expected_size));
            }
            return out;
        }

        if (rc == Z_BUF_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("gzip: payload exceeds the size declared in the TOC (" +
                                     count(expected_size) + " bytes)");
        }
        if (rc != Z_OK) {
            const std::string message = zs.msg != nullptr ? zs.msg : "inflate failed";
            inflateEnd(&zs);
            throw std::runtime_error("gzip: " + message);
        }
    }
}

}  // namespace lolview
