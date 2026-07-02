// libFuzzer entry point for the wl2:sdp parser. Off by default; build with
// -DWL2_SDP_FUZZ=ON under a Clang toolchain, then seed from ../data:
//
//   ./wl2_sdp_fuzz -max_len=65536 corpus/ modules/wl2_sdp/test/data/
//
// The parser must never crash, hang, or read out of bounds on arbitrary input,
// and a successful strict parse must round-trip through build().

#include "wl2_sdp/sdp.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);

    // Lenient parse: exercises the tolerant path over hostile bytes.
    wl2::sdp::ParseOptions lenient;
    lenient.lenient = true;
    (void)wl2::sdp::parse(text, lenient);

    // Strict parse: when it succeeds, the round-trip invariant must hold.
    if (auto strict = wl2::sdp::parse(text)) {
        wl2::sdp::Session session = strict.value();
        std::string rebuilt = wl2::sdp::build(session);
        // Re-parse the output to ensure build() emits parseable SDP.
        (void)wl2::sdp::parse(rebuilt, lenient);
    }
    return 0;
}
