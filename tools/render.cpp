// build/host/render IN.wav OUT.wav --model X.bin [--block N]: runs the pedal's A2-Lite engine (daisy/src/dsp/a2lite.h)
// over a 48 kHz WAV file and writes mono float32. Channel 0 of a multi-channel input is used (the pedal's left input).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "dsp/a2lite.h"
#include "model.h"
#include "wav.h"

namespace {

int Usage() {
    std::fprintf(stderr, "usage: render IN.wav OUT.wav --model X.bin [--block N]\n");
    return 2;
}

int Fail(const std::string& path, const std::string& what) {
    std::fprintf(stderr, "render: %s: %s\n", path.c_str(), what.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    std::string model_path;
    size_t block = dkdsp::kBlockSize;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) {
            model_path = argv[++i];
        } else if (arg == "--block" && i + 1 < argc) {
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 65536) return Usage();
            block = size_t(v);
        } else if (arg.size() > 1 && arg[0] == '-') {
            return Usage();
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2 || model_path.empty()) return Usage();
    const std::string& in_path = positional[0];
    const std::string& out_path = positional[1];

    std::ifstream mf(model_path, std::ios::binary);
    if (!mf) return Fail(model_path, "cannot open");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    auto model = std::make_unique<dkmodel::Model>();
    const dkmodel::Status st = dkmodel::Parse(bytes.data(), bytes.size(), model.get());
    if (st != dkmodel::Status::kOk) return Fail(model_path, dkmodel::StatusName(st));

    dkwav::Audio audio;
    std::string err;
    if (!dkwav::Read(in_path, &audio, &err)) return Fail(in_path, err);
    if (audio.rate != 48000) return Fail(in_path, "sample rate " + std::to_string(audio.rate) + ", need 48000");

    auto engine = std::make_unique<dkdsp::A2Lite>();
    engine->Load(model->weights, model->head_scale);
    std::vector<float> out(audio.samples.size());
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < out.size(); i += block) {
        const size_t n = out.size() - i < block ? out.size() - i : block;
        engine->Process(audio.samples.data() + i, out.data() + i, n);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!dkwav::WriteFloat32(out_path, out, 48000, &err)) return Fail(out_path, err);
    const double secs = double(out.size()) / 48000.0;
    std::printf("%s: \"%s\" %zu samples, %.2f s in %.2f s (%.2fx real time)\n", out_path.c_str(), model->name,
                out.size(), secs, wall, wall > 0.0 ? secs / wall : 0.0);
    return 0;
}
