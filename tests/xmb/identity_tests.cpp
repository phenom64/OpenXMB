#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size < 0) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!data.empty() &&
        !input.read(reinterpret_cast<char*>(data.data()),
                    static_cast<std::streamsize>(data.size()))) {
        return {};
    }
    return data;
}

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::string sha256(std::vector<std::uint8_t> data) {
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while (data.size() % 64U != 56U) data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    std::array<std::uint32_t, 8> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    for (std::size_t offset = 0; offset < data.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = offset + i * 4U;
            words[i] = (static_cast<std::uint32_t>(data[at]) << 24U) |
                       (static_cast<std::uint32_t>(data[at + 1]) << 16U) |
                       (static_cast<std::uint32_t>(data[at + 2]) << 8U) |
                       static_cast<std::uint32_t>(data[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = std::rotr(words[i - 15], 7) ^
                                     std::rotr(words[i - 15], 18) ^
                                     (words[i - 15] >> 3U);
            const std::uint32_t s1 = std::rotr(words[i - 2], 17) ^
                                     std::rotr(words[i - 2], 19) ^
                                     (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choice + kSha256Constants[i] + words[i];
            const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint32_t value : state) output << std::setw(8) << value;
    return output.str();
}

struct IdentityAsset {
    std::string_view relative_path;
    std::size_t bytes;
    std::string_view digest;
};

constexpr std::array<IdentityAsset, 16> kIdentityAssets{{
    {"interfaceFX/TypefaceServer/Play-Regular.ttf", 183928,
     "77f57b8fa6f004d8a3405662ba883d29d491d86cf9449351797f205380ac57cc"},
    {"interfaceFX/TypefaceServer/Play-Bold.ttf", 194848,
     "6ea1a9b2ad8979646e3a48672e3f26d074f9a3caf644e0fc2139d383b637346a"},
    {"interfaceFX/TypefaceServer/OFL.txt", 4537,
     "9795f1c1a0b97d946994e3eae682c04133098d414d9c6a295fb4560231d025cc"},
    {"interfaceFX/AudioServer/NSE.clicker.Opt.wav", 4966,
     "8119d85f173fb28a80b26b9e6275e06b229efa36b98491abc56775897513fe15"},
    {"interfaceFX/AudioServer/NSE.questionMark.wav", 31152,
     "435e130c843ced1f1310f2404e9a45322fa13924fd958c4d75153838fc203fdb"},
    {"interfaceFX/AudioServer/NSE.ui.Confirm.wav", 70320,
     "0221c1fe8327b3ec9040be5250f38a218722446b4ec316797ef86b9108f67153"},
    {"interfaceFX/AudioServer/NSE.ui.Cancel.wav", 57712,
     "0642be278d7f0275243d569c2fff49ae62430acdfd0a99d79c8c4f89ee8297a5"},
    {"interfaceFX/AudioServer/NSE.clicker.Cancel.wav", 8466,
     "c3f735f43954985168d68dea143885d61ef9aa13b6b247113683903cc835f5d3"},
    {"interfaceFX/AudioServer/NSE.startup.ogg", 371661,
     "7a85812c7495394657eed8ab6b93f2a9c8196b7d90560e6b3de23b1044306c99"},
    {"interfaceFX/AudioServer/NSE.startup.GameBoot.wav", 394414,
     "26fb728f4f998d36e17069d30345ef6126e3581b6b19b1c2493267b310f312b7"},
    {"interfaceFX/AudioServer/NSE.ui.Error.wav", 77918,
     "9557e6b981abb661bb968974c36c26bc5b15bf6668c16ba99cb39e0af58a7746"},
    {"interfaceFX/AudioServer/NSE.charge.wav", 107282,
     "01c64f66fc36603db9cd2bef16607de234ba0df2901305aa9cebb287cc717b1e"},
    {"shaders/wave.vert", 2076,
     "bf2fe9040ffbc3766f1986375fcfcdbbe824ec94b0f28ef3019eeebcefa90b49"},
    {"shaders/wave.frag", 1369,
     "6ba9149e10fc402c3bdc15eac3c06faaf925adc014f77d42472e00e2ee0895df"},
    {"LICENSE.RetroArch", 543,
     "6a155c9af8d6114f059bae61deff079277f1ceb05898c9207b992d41650d487f"},
    {"tools/licensing/headers/header_retroarch_wave_shader.txt", 356,
     "5f7a85d5473d4c5037f213796484876c09a87419ece982f3e593deed5b9354e2"},
}};

std::filesystem::path find_repository(int argc, char** argv) {
    if (argc > 1) return std::filesystem::absolute(argv[1]);
    std::vector<std::filesystem::path> seeds{std::filesystem::current_path(),
                                             std::filesystem::absolute(__FILE__)};
    for (auto seed : seeds) {
        if (!std::filesystem::is_directory(seed)) seed = seed.parent_path();
        while (!seed.empty()) {
            if (std::filesystem::exists(
                    seed / "assets/xmb/manifests/identity-assets.json")) {
                return seed;
            }
            const auto parent = seed.parent_path();
            if (parent == seed) break;
            seed = parent;
        }
    }
    return {};
}

void test_startup_identity() {
    constexpr std::string_view startup = "Syndromatic Limited Bharat Britannia";
    constexpr std::string_view expected_hex =
        "53796e64726f6d61746963204c696d69746564204268617261742042726974616e6e6961";
    expect(startup.size() == 36, "startup identity is exactly 36 UTF-8 bytes");
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    std::vector<std::uint8_t> bytes;
    for (const unsigned char value : startup) {
        encoded << std::setw(2) << static_cast<unsigned int>(value);
        bytes.push_back(value);
    }
    expect(encoded.str() == expected_hex, "startup identity UTF-8 bytes are canonical");
    expect(sha256(bytes) ==
               "02d55303109e06f5021f03ff3f313237f25c310cbb9bab37c8c96f344b64435b",
           "startup identity SHA-256 is canonical");
    expect(startup.find("PlayStation") == std::string_view::npos &&
               startup.find("PS3") == std::string_view::npos,
           "startup identity contains no PlayStation branding");
}

void test_manifest_and_assets(const std::filesystem::path& repository) {
    const auto manifest_bytes =
        read_file(repository / "assets/xmb/manifests/identity-assets.json");
    expect(!manifest_bytes.empty(), "identity manifest is readable");
    const std::string manifest(manifest_bytes.begin(), manifest_bytes.end());
    expect(manifest.find("Syndromatic Limited Bharat Britannia") != std::string::npos,
           "identity manifest records canonical startup text");
    expect(manifest.find("\"original_captured_assets_absent\": \"classic\"") !=
               std::string::npos,
           "Original compatibility-pack fallback is explicitly Classic");
    expect(manifest.find("\"provenance_classification\": \"unresolved/local\"") !=
               std::string::npos,
           "NSE provenance remains explicitly unresolved/local");
    expect(manifest.find("\"license\": \"OFL-1.1\"") != std::string::npos,
           "Play assets record OFL-1.1");
    expect(manifest.find("\"license\": \"GPL-3.0-only\"") != std::string::npos,
           "RetroArch Classic files record GPL-3.0-only");

    for (const auto& expected : kIdentityAssets) {
        const auto data = read_file(repository / expected.relative_path);
        expect(data.size() == expected.bytes,
               std::string(expected.relative_path) + " byte size matches Packet 02");
        if (data.size() == expected.bytes) {
            expect(sha256(data) == expected.digest,
                   std::string(expected.relative_path) + " SHA-256 matches Packet 02");
        }
        expect(manifest.find(expected.relative_path) != std::string::npos,
               std::string(expected.relative_path) + " is present in the identity manifest");
        expect(manifest.find(expected.digest) != std::string::npos,
               std::string(expected.relative_path) + " hash is present in the identity manifest");
    }
}

}  // namespace

int main(int argc, char** argv) {
    test_startup_identity();
    const auto repository = find_repository(argc, argv);
    expect(!repository.empty(), "OpenXMB repository root is discoverable");
    if (!repository.empty()) {
        std::cout << "repository=" << repository.string() << '\n';
        test_manifest_and_assets(repository);
    }
    if (failures != 0) {
        std::cerr << failures << " identity test(s) failed\n";
        return 1;
    }
    std::cout << "identity_tests: PASS\n";
    return 0;
}
