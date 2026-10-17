// wav.h - 32-bit float stereo WAV writer for the offline render tool.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline bool writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, int sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t frames = (uint32_t)std::min(L.size(), R.size());
    const uint16_t channels = 2, bits = 32;
    const uint32_t dataBytes = frames * channels * (bits / 8);
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(3 /* IEEE float */); w16(channels);
    w32((uint32_t)sampleRate); w32((uint32_t)sampleRate * channels * (bits / 8)); w16(channels * (bits / 8)); w16(bits);
    std::fwrite("data", 1, 4, f); w32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i) { std::fwrite(&L[i], 4, 1, f); std::fwrite(&R[i], 4, 1, f); }
    std::fclose(f);
    return true;
}
