// Holographic Reduced Representations — pure C++20, no Godot dependency.
// Phase-vector algebra: atoms are deterministic SHA-256-derived phase vectors.
// Matches the Python holographic.py in thirdparty/taskweft-planner exactly,
// so atom vectors are identical across languages and machines.
//
// References:
//   Plate (1995) — Holographic Reduced Representations
//   Gayler (2004) — Vector Symbolic Architectures
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace TwHRR {

static constexpr double TWO_PI = 6.283185307179586;

// ---- Compact SHA-256 (FIPS 180-4) -----------------------------------------
// Produces byte-identical output to Python's hashlib.sha256.

namespace _sha256 {

static constexpr uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
inline uint32_t maj(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }
inline uint32_t s0(uint32_t x) { return rotr(x,2)^rotr(x,13)^rotr(x,22); }
inline uint32_t s1(uint32_t x) { return rotr(x,6)^rotr(x,11)^rotr(x,25); }
inline uint32_t g0(uint32_t x) { return rotr(x,7)^rotr(x,18)^(x>>3); }
inline uint32_t g1(uint32_t x) { return rotr(x,17)^rotr(x,19)^(x>>10); }

inline std::array<uint8_t,32> hash(const uint8_t *data, size_t len) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };

    // Pre-processing: message schedule with padding
    uint64_t bit_len = (uint64_t)len * 8;
    size_t padded = ((len + 9 + 63) / 64) * 64;
    std::vector<uint8_t> msg(padded, 0);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i)
        msg[padded - 8 + i] = (uint8_t)(bit_len >> (56 - 8*i));

    for (size_t offset = 0; offset < padded; offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = msg.data() + offset + 4*i;
            w[i] = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        }
        for (int i = 16; i < 64; ++i)
            w[i] = g1(w[i-2]) + w[i-7] + g0(w[i-15]) + w[i-16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + s1(e) + ch(e,f,g) + K[i] + w[i];
            uint32_t t2 = s0(a) + maj(a,b,c);
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::array<uint8_t,32> out;
    for (int i = 0; i < 8; ++i) {
        out[4*i+0] = (uint8_t)(h[i]>>24); out[4*i+1] = (uint8_t)(h[i]>>16);
        out[4*i+2] = (uint8_t)(h[i]>>8);  out[4*i+3] = (uint8_t)(h[i]);
    }
    return out;
}

} // namespace _sha256

// ---- Phase vector type ----------------------------------------------------

using PhaseVec = std::vector<double>;

// ---- Core HRR algebra ------------------------------------------------------

// Deterministic atom: SHA-256 counter blocks → phases in [0, 2π).
// Matches Python: encode_atom(word, dim)
inline PhaseVec encode_atom(const std::string &word, int dim = 4096) {
    constexpr int VALUES_PER_BLOCK = 16; // 32 bytes / 2 bytes-per-uint16
    int blocks_needed = (dim + VALUES_PER_BLOCK - 1) / VALUES_PER_BLOCK;

    PhaseVec phases;
    phases.reserve(dim);

    for (int i = 0; i < blocks_needed && (int)phases.size() < dim; ++i) {
        std::string key = word + ":" + std::to_string(i);
        auto digest = _sha256::hash(
            reinterpret_cast<const uint8_t*>(key.c_str()), key.size());
        // Little-endian uint16 pairs: matches Python struct.unpack("<16H", digest)
        for (int j = 0; j < 32 && (int)phases.size() < dim; j += 2) {
            uint16_t val = (uint16_t)digest[j] | ((uint16_t)digest[j+1] << 8);
            phases.push_back(val * (TWO_PI / 65536.0));
        }
    }
    return phases;
}

// Circular convolution: element-wise phase addition mod 2π.
inline PhaseVec bind(const PhaseVec &a, const PhaseVec &b) {
    PhaseVec result(a.size());
    for (size_t i = 0; i < a.size(); ++i)
        result[i] = std::fmod(a[i] + b[i], TWO_PI);
    return result;
}

// Circular correlation: element-wise phase subtraction mod 2π.
inline PhaseVec unbind(const PhaseVec &memory, const PhaseVec &key) {
    PhaseVec result(memory.size());
    for (size_t i = 0; i < memory.size(); ++i) {
        double v = std::fmod(memory[i] - key[i], TWO_PI);
        if (v < 0.0) v += TWO_PI;
        result[i] = v;
    }
    return result;
}

// Superposition via circular mean (unit complex vector mean).
// Each component e^{iθ} is treated as a unit phasor. The mean phasor direction
// is atan2(Σsin(θ), Σcos(θ)), which is the correct superposition for retrieval:
// similarity(v_k, bundle({v1,...,vN})) ≈ 1/N when v_k is one of the N components.
// This replaces the broken additive-phase bundle (which is really another bind).
inline PhaseVec bundle(const std::vector<PhaseVec> &vecs) {
    if (vecs.empty()) return {};
    size_t dim = vecs[0].size();
    std::vector<double> sum_sin(dim, 0.0), sum_cos(dim, 0.0);
    for (auto &v : vecs) {
        for (size_t i = 0; i < dim; ++i) {
            sum_sin[i] += std::sin(v[i]);
            sum_cos[i] += std::cos(v[i]);
        }
    }
    PhaseVec result(dim);
    for (size_t i = 0; i < dim; ++i) {
        result[i] = std::atan2(sum_sin[i], sum_cos[i]);
        if (result[i] < 0.0) result[i] += TWO_PI;
    }
    return result;
}

// Phase cosine similarity: mean(cos(a - b)) ∈ [-1, 1].
inline double similarity(const PhaseVec &a, const PhaseVec &b) {
    if (a.empty()) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += std::cos(a[i] - b[i]);
    return sum / (double)a.size();
}

// SNR estimate for storage capacity: sqrt(dim / n_items).
inline double snr_estimate(int dim, int n_items) {
    if (n_items <= 0) return 1e18;
    return std::sqrt((double)dim / n_items);
}

// ---- Text encoding ---------------------------------------------------------

// Tokenise: lowercase + split on whitespace + strip punctuation.
inline std::vector<std::string> tokenize(const std::string &text) {
    static const std::string PUNCT = ".,!?;:\"'()[]{}-";
    std::vector<std::string> tokens;
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return;
        // strip leading/trailing punct
        size_t s = word.find_first_not_of(PUNCT);
        size_t e = word.find_last_not_of(PUNCT);
        if (s != std::string::npos)
            tokens.push_back(word.substr(s, e - s + 1));
        word.clear();
    };
    for (unsigned char c : text) {
        if (std::isspace(c)) { flush(); }
        else { word += (char)std::tolower(c); }
    }
    flush();
    return tokens;
}

// Bag-of-words: bundle of token atom vectors. Matches Python: encode_text.
inline PhaseVec encode_text(const std::string &text, int dim = 4096) {
    auto tokens = tokenize(text);
    if (tokens.empty()) return encode_atom("__hrr_empty__", dim);
    std::vector<PhaseVec> vecs;
    vecs.reserve(tokens.size());
    for (auto &tok : tokens) vecs.push_back(encode_atom(tok, dim));
    return bundle(vecs);
}

// Direct content-entity binding. Matches Python: encode_binding.
// unbind(encode_binding(c, e), encode_atom(e)) == encode_text(c)
inline PhaseVec encode_binding(const std::string &content, const std::string &entity, int dim = 4096) {
    std::string ent_lower = entity;
    for (auto &c : ent_lower) c = (char)std::tolower((unsigned char)c);
    // Qualify to suppress ADL finding std::bind (args are std::vector<double>).
    return TwHRR::bind(encode_text(content, dim), encode_atom(ent_lower, dim));
}

// Bundled role encoding. Matches Python: encode_fact.
inline PhaseVec encode_fact(const std::string &content,
        const std::vector<std::string> &entities, int dim = 4096) {
    PhaseVec role_content = encode_atom("__hrr_role_content__", dim);
    PhaseVec role_entity  = encode_atom("__hrr_role_entity__",  dim);

    std::vector<PhaseVec> components;
    components.push_back(TwHRR::bind(encode_text(content, dim), role_content));
    for (auto &entity : entities) {
        std::string e = entity;
        for (auto &c : e) c = (char)std::tolower((unsigned char)c);
        components.push_back(TwHRR::bind(encode_atom(e, dim), role_entity));
    }
    return bundle(components);
}

// ---- Serialisation ---------------------------------------------------------

// Phase vector → raw bytes (float64 little-endian, 8 bytes/element).
inline std::vector<uint8_t> phases_to_bytes(const PhaseVec &phases) {
    std::vector<uint8_t> out(phases.size() * 8);
    for (size_t i = 0; i < phases.size(); ++i)
        std::memcpy(out.data() + i * 8, &phases[i], 8);
    return out;
}

// Raw bytes → phase vector. Inverse of phases_to_bytes.
inline PhaseVec bytes_to_phases(const uint8_t *data, size_t byte_len) {
    size_t dim = byte_len / 8;
    PhaseVec phases(dim);
    for (size_t i = 0; i < dim; ++i)
        std::memcpy(&phases[i], data + i * 8, 8);
    return phases;
}
inline PhaseVec bytes_to_phases(const std::vector<uint8_t> &data) {
    return bytes_to_phases(data.data(), data.size());
}

} // namespace TwHRR
