// build/host/render IN.wav OUT.wav --model X.bin [--ir Y.bin] [--block N]: runs the pedal's A2-Lite engine
// (daisy/src/dsp/a2lite.h), then the IR convolver (daisy/src/dsp/convolver.h) when --ir is given, over a 48 kHz WAV file
// and writes mono float32. Channel 0 of a multi-channel input is used (the pedal's left input).
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
#include "dsp/convolver.h"
#include "ir.h"
#include "model.h"
#include "wav.h"

namespace {

int Usage() {
    std::fprintf(stderr, "usage: render IN.wav OUT.wav --model X.bin [--ir Y.bin] [--block N]\n");
    return 2;
}

int Fail(const std::string& path, const std::string& what) {
    std::fprintf(stderr, "render: %s: %s\n", path.c_str(), what.c_str());
    return 1;
}

// Empty on failure.
std::vector<uint8_t> ReadBin(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    std::string model_path, ir_path;
    size_t block = dkdsp::kBlockSize;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) {
            model_path = argv[++i];
        } else if (arg == "--ir" && i + 1 < argc) {
            ir_path = argv[++i];
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

    const std::vector<uint8_t> bytes = ReadBin(model_path);
    if (bytes.empty()) return Fail(model_path, "cannot open");
    auto model = std::make_unique<dkmodel::Model>();
    const dkmodel::Status st = dkmodel::Parse(bytes.data(), bytes.size(), model.get());
    if (st != dkmodel::Status::kOk) return Fail(model_path, dkmodel::StatusName(st));

    auto ir = std::make_unique<dkir::Ir>();
    if (!ir_path.empty()) {
        const std::vector<uint8_t> ir_bytes = ReadBin(ir_path);
        if (ir_bytes.empty()) return Fail(ir_path, "cannot open");
        const dkir::Status ist = dkir::Parse(ir_bytes.data(), ir_bytes.size(), ir.get());
        if (ist != dkir::Status::kOk) return Fail(ir_path, dkir::StatusName(ist));
    }

    dkwav::Audio audio;
    std::string err;
    if (!dkwav::Read(in_path, &audio, &err)) return Fail(in_path, err);
    if (audio.rate != 48000) return Fail(in_path, "sample rate " + std::to_string(audio.rate) + ", need 48000");

    std::vector<float> hist(dkdsp::a2::kHistFloats);
    auto engine = std::make_unique<dkdsp::A2Lite>();
    engine->Bind(hist.data(), nullptr, 0);
    engine->Load(model->weights, model->head_scale);
    std::vector<float> out(audio.samples.size());
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < out.size(); i += block) {
        const size_t n = out.size() - i < block ? out.size() - i : block;
        engine->Process(audio.samples.data() + i, out.data() + i, n);
    }
    if (!ir_path.empty()) {
        // Exactly kBlockSize per call: the last partial chunk is zero-padded and only its real samples kept.
        std::vector<float> pool(dkdsp::kIrTailPoolFloats);
        auto conv = std::make_unique<dkdsp::IrConvolver>();
        if (!conv->Init(pool.data())) return Fail(ir_path, "fft");
        conv->Load(ir->taps);
        float chunk[dkdsp::kBlockSize];
        for (size_t i = 0; i < out.size(); i += dkdsp::kBlockSize) {
            const size_t n = out.size() - i < dkdsp::kBlockSize ? out.size() - i : dkdsp::kBlockSize;
            std::memset(chunk, 0, sizeof chunk);
            std::memcpy(chunk, out.data() + i, n * sizeof(float));
            conv->Process(chunk, chunk);
            std::memcpy(out.data() + i, chunk, n * sizeof(float));
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!dkwav::WriteFloat32(out_path, out, 48000, &err)) return Fail(out_path, err);
    const double secs = double(out.size()) / 48000.0;
    std::printf("%s: \"%s\" %zu samples, %.2f s in %.2f s (%.2fx real time)%s\n", out_path.c_str(), model->name,
                out.size(), secs, wall, wall > 0.0 ? secs / wall : 0.0,
                ir_path.empty() ? "" : (std::string(" ir \"") + ir->name + "\"").c_str());
    return 0;
}
