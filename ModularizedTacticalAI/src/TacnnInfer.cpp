/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-16; see CHANGES-ja2mod.md and
 * TacnnInfer.h. The network below is tools/tacrl/model/policy.py line for
 * line; when that file changes, this one changes with it, and the parity
 * test in tools/test_tacrl_embedded.py is what says the two still agree.
 */

#include "../include/TacnnInfer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <map>

#if defined(__SSE__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define TACNN_SSE 1
#endif

namespace tacnn
{
    namespace
    {
        // ------------------------------------------------------------------ the network's fixed sizes

        const int PATCH = TACNN_PATCH;                       // 20
        const int PATCH_CHANNELS = TACNN_PATCH_CHANNELS;     // 16
        const int GLOBAL = TACNN_GLOBAL;                     // 16
        const int GLOBAL_CHANNELS = TACNN_GLOBAL_CHANNELS;   // 6
        const int ENTITIES = TACNN_ENTITIES;                 // 20
        const int ENTITY_DIMS = TACNN_ENTITY_DIMS;           // 40
        const int ENTITY_D = 96;
        const int HEADS = 4;
        const int HEAD_D = ENTITY_D / HEADS;                 // 24
        const int FF = 2 * ENTITY_D;                         // 192
        const int LAYERS = 2;
        const int SELF_DIMS = TACNN_SELF_DIMS;               // 56
        const int SELF_D = 96;
        const int HIDDEN = TACNN_HIDDEN;                     // 192
        const int QUERY_DIMS = 12;
        const int HEARD_D = 48;
        const int SPATIAL_D = 128;
        const int COARSE_D = 48;
        const int PATCH_FEATURES = 64 * 3 * 3;               // 576
        const int BASE_D = SPATIAL_D + COARSE_D + ENTITY_D + SELF_D;   // 368
        const int FUSION_IN = BASE_D + HEARD_D;              // 416
        const int ROLE_D = 16;
        const int PERSONALITY_D = 16;
        const int COND_D = ROLE_D + PERSONALITY_D;           // 32
        const int CAND_D = 96;
        const int TYPE_HIDDEN = 96;
        const int PARAMS_HIDDEN = 64;
        const int PARAMS_OUT = TACNN_AIM_LEVELS + TACNN_FIRE_MODES + TACNN_STANCES;  // 12
        const float NORM_EPS = 1e-5f;
        const float MASK_FILL = -1e4f;

        // ------------------------------------------------------------------ arithmetic

        inline float Dot(const float* a, const float* b, int n)
        {
            int i = 0;
            float sum = 0.0f;
#ifdef TACNN_SSE
            __m128 acc0 = _mm_setzero_ps();
            __m128 acc1 = _mm_setzero_ps();
            for(; i + 8 <= n; i += 8)
            {
                acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
                acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
            }
            for(; i + 4 <= n; i += 4)
                acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
            acc0 = _mm_add_ps(acc0, acc1);
            float lanes[4];
            _mm_storeu_ps(lanes, acc0);
            sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
#endif
            for(; i < n; ++i)
                sum += a[i] * b[i];
            return sum;
        }

        /// Four dot products sharing the right-hand vector: the loads of b are shared and the four
        /// accumulators keep the adder busy, which one accumulator chain cannot (x86-32 has eight
        /// vector registers, so four is the useful width here).
        inline void Dot4(const float* a0, const float* a1, const float* a2, const float* a3,
                         const float* b, int n, float* out)
        {
            int i = 0;
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
#ifdef TACNN_SSE
            __m128 acc0 = _mm_setzero_ps();
            __m128 acc1 = _mm_setzero_ps();
            __m128 acc2 = _mm_setzero_ps();
            __m128 acc3 = _mm_setzero_ps();
            for(; i + 4 <= n; i += 4)
            {
                const __m128 x = _mm_loadu_ps(b + i);
                acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(a0 + i), x));
                acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(a1 + i), x));
                acc2 = _mm_add_ps(acc2, _mm_mul_ps(_mm_loadu_ps(a2 + i), x));
                acc3 = _mm_add_ps(acc3, _mm_mul_ps(_mm_loadu_ps(a3 + i), x));
            }
            float lanes[4];
            _mm_storeu_ps(lanes, acc0);
            s0 = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
            _mm_storeu_ps(lanes, acc1);
            s1 = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
            _mm_storeu_ps(lanes, acc2);
            s2 = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
            _mm_storeu_ps(lanes, acc3);
            s3 = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
#endif
            for(; i < n; ++i)
            {
                const float x = b[i];
                s0 += a0[i] * x;
                s1 += a1[i] * x;
                s2 += a2[i] * x;
                s3 += a3[i] * x;
            }
            out[0] = s0;
            out[1] = s1;
            out[2] = s2;
            out[3] = s3;
        }

        inline float Gelu(float x)
        {
            return 0.5f * x * (1.0f + erff(x * 0.70710678118654752440f));
        }

        inline float Sigmoid(float x)
        {
            return 1.0f / (1.0f + expf(-x));
        }

        void Softmax(float* v, int n)
        {
            float best = v[0];
            for(int i = 1; i < n; ++i)
                if(v[i] > best)
                    best = v[i];
            float total = 0.0f;
            for(int i = 0; i < n; ++i)
            {
                v[i] = expf(v[i] - best);
                total += v[i];
            }
            const float inv = 1.0f / total;
            for(int i = 0; i < n; ++i)
                v[i] *= inv;
        }

        // ------------------------------------------------------------------ layers

        struct Linear
        {
            const float* w;     // [out][in]
            const float* b;     // [out]
            int in;
            int out;
        };

        struct Conv
        {
            const float* w;     // [oc][ic][3][3]
            const float* b;     // [oc]
            int ic;
            int oc;
        };

        struct Affine
        {
            const float* w;
            const float* b;
            int n;
        };

        struct EncoderLayer
        {
            Affine norm1;
            Linear qkv;
            Linear out;
            Affine norm2;
            Linear ff1;
            Linear ff2;
        };

        struct Net
        {
            Conv patchConv[3];
            Affine patchNorm[3];
            Linear fusePatches;
            Conv globalConv[2];
            Linear globalOut;
            Linear embed1, embed2;
            EncoderLayer layers[LAYERS];
            Affine finalNorm;
            const float* query;     // [96]
            Linear key, value;
            Linear self1, self2;
            Linear preContext;
            Linear commsQuery, commsValue;
            Linear fusion;
            Affine fusionNorm;
            const float* role;      // [8][16]
            Linear personality;
            Linear film;
            const float* gruWih;    // [576][192]
            const float* gruWhh;
            const float* gruBih;    // [576]
            const float* gruBhh;
            Linear type1, type2;
            Linear cand1, cand2;
            Linear targetQuery;
            Linear params1, params2;
            Linear message, signature;
        };

        void ApplyLinear(const Linear& L, const float* x, float* y)
        {
            int o = 0;
            for(; o + 4 <= L.out; o += 4)
            {
                const float* w = L.w + (size_t)o * L.in;
                float d[4];
                Dot4(w, w + L.in, w + 2 * L.in, w + 3 * L.in, x, L.in, d);
                y[o] = L.b[o] + d[0];
                y[o + 1] = L.b[o + 1] + d[1];
                y[o + 2] = L.b[o + 2] + d[2];
                y[o + 3] = L.b[o + 3] + d[3];
            }
            for(; o < L.out; ++o)
                y[o] = L.b[o] + Dot(L.w + (size_t)o * L.in, x, L.in);
        }

        void ApplyLinearRows(const Linear& L, const float* x, int rows, float* y)
        {
            for(int r = 0; r < rows; ++r)
                ApplyLinear(L, x + (size_t)r * L.in, y + (size_t)r * L.out);
        }

        void GeluInPlace(float* v, int n)
        {
            for(int i = 0; i < n; ++i)
                v[i] = Gelu(v[i]);
        }

        void LayerNorm(const Affine& a, const float* x, float* y)
        {
            float mean = 0.0f;
            for(int i = 0; i < a.n; ++i)
                mean += x[i];
            mean /= (float)a.n;
            float var = 0.0f;
            for(int i = 0; i < a.n; ++i)
            {
                const float d = x[i] - mean;
                var += d * d;
            }
            var /= (float)a.n;
            const float inv = 1.0f / sqrtf(var + NORM_EPS);
            for(int i = 0; i < a.n; ++i)
                y[i] = (x[i] - mean) * inv * a.w[i] + a.b[i];
        }

        /// GroupNorm(8, C) over a [C][spatial] map, in place.
        void GroupNorm8(const Affine& a, float* x, int channels, int spatial)
        {
            const int groups = 8;
            const int per = channels / groups;
            const int count = per * spatial;
            for(int g = 0; g < groups; ++g)
            {
                float* base = x + (size_t)g * per * spatial;
                float mean = 0.0f;
                for(int i = 0; i < count; ++i)
                    mean += base[i];
                mean /= (float)count;
                float var = 0.0f;
                for(int i = 0; i < count; ++i)
                {
                    const float d = base[i] - mean;
                    var += d * d;
                }
                var /= (float)count;
                const float inv = 1.0f / sqrtf(var + NORM_EPS);
                for(int c = 0; c < per; ++c)
                {
                    const int channel = g * per + c;
                    float* row = base + (size_t)c * spatial;
                    const float scale = inv * a.w[channel];
                    const float shift = a.b[channel] - mean * scale;
                    for(int i = 0; i < spatial; ++i)
                        row[i] = row[i] * scale + shift;
                }
            }
        }

        /// 3x3 convolution, stride 2, padding 1, through an im2col buffer of [positions][ic*9].
        void Conv3x3s2(const Conv& c, const float* x, int H, int W, float* col, float* y, int& OH, int& OW)
        {
            OH = (H - 1) / 2 + 1;
            OW = (W - 1) / 2 + 1;
            const int L = c.ic * 9;
            const int positions = OH * OW;
            for(int oy = 0; oy < OH; ++oy)
            {
                for(int ox = 0; ox < OW; ++ox)
                {
                    float* dst = col + (size_t)(oy * OW + ox) * L;
                    for(int ic = 0; ic < c.ic; ++ic)
                    {
                        const float* plane = x + (size_t)ic * H * W;
                        for(int ky = 0; ky < 3; ++ky)
                        {
                            const int iy = oy * 2 - 1 + ky;
                            for(int kx = 0; kx < 3; ++kx)
                            {
                                const int ix = ox * 2 - 1 + kx;
                                dst[ic * 9 + ky * 3 + kx] = (iy >= 0 && iy < H && ix >= 0 && ix < W) ? plane[iy * W + ix] : 0.0f;
                            }
                        }
                    }
                }
            }
            int oc = 0;
            for(; oc + 4 <= c.oc; oc += 4)
            {
                const float* w = c.w + (size_t)oc * L;
                float* out = y + (size_t)oc * positions;
                for(int p = 0; p < positions; ++p)
                {
                    float d[4];
                    Dot4(w, w + L, w + 2 * L, w + 3 * L, col + (size_t)p * L, L, d);
                    out[p] = c.b[oc] + d[0];
                    out[positions + p] = c.b[oc + 1] + d[1];
                    out[2 * positions + p] = c.b[oc + 2] + d[2];
                    out[3 * positions + p] = c.b[oc + 3] + d[3];
                }
            }
            for(; oc < c.oc; ++oc)
            {
                const float* w = c.w + (size_t)oc * L;
                float* out = y + (size_t)oc * positions;
                for(int p = 0; p < positions; ++p)
                    out[p] = c.b[oc] + Dot(w, col + (size_t)p * L, L);
            }
        }

        // ------------------------------------------------------------------ scratch

        struct Workspace
        {
            float patchIn[PATCH_CHANNELS * PATCH * PATCH];
            float col[100 * PATCH_CHANNELS * 9];         // the largest im2col: conv1 of a patch
            float act1[32 * 10 * 10];
            float act2[48 * 5 * 5];
            float act3[64 * 3 * 3];
            float patches[2 * PATCH_FEATURES];
            float spatial[SPATIAL_D];
            float globalIn[GLOBAL_CHANNELS * GLOBAL * GLOBAL];
            float gact1[16 * 8 * 8];
            float gact2[24 * 4 * 4];
            float coarse[COARSE_D];
            float entIn[ENTITIES * ENTITY_DIMS];         // the present entity rows, packed
            float ent[ENTITIES * ENTITY_D];
            float entTmp[ENTITIES * ENTITY_D];
            float entNorm[ENTITIES * ENTITY_D];
            float qkv[ENTITIES * 3 * ENTITY_D];
            float mixed[ENTITIES * ENTITY_D];
            float ffHidden[ENTITIES * FF];
            float scores[ENTITIES];
            float keys[ENTITIES * ENTITY_D];
            float values[ENTITIES * ENTITY_D];
            float ents[ENTITY_D];
            float me1[SELF_D];
            float me[SELF_D];
            float base[FUSION_IN];                       // base then heard
            float context[HIDDEN];
            float commsQ[QUERY_DIMS];
            float commsScores[TACNN_INBOX];
            float commsValue[HEARD_D];
            float fusionRaw[HIDDEN];
            float fused[HIDDEN];
            float persona[PERSONALITY_D];
            float cond[COND_D];
            float filmOut[2 * HIDDEN];
            float feat[HIDDEN];
            float gi[3 * HIDDEN];
            float gh[3 * HIDDEN];
            float typeHidden[TYPE_HIDDEN];
            float candHidden[TACNN_CANDIDATES * CAND_D];
            float candKeys[TACNN_CANDIDATES * CAND_D];
            float targetQuery[CAND_D];
            float paramsHidden[PARAMS_HIDDEN];
            float params[PARAMS_OUT];
        };

        // ------------------------------------------------------------------ protobuf

        struct Tensor
        {
            std::vector<int64_t> dims;
            std::vector<float> data;
        };

        typedef std::map<std::string, Tensor> TensorMap;

        struct Reader
        {
            const uint8_t* p;
            const uint8_t* end;
            bool bad;

            Reader(const uint8_t* begin, size_t size) : p(begin), end(begin + size), bad(false) {}

            size_t left() const { return (size_t)(end - p); }

            uint64_t varint()
            {
                uint64_t value = 0;
                int shift = 0;
                while(p < end && shift < 64)
                {
                    const uint8_t byte = *p++;
                    value |= (uint64_t)(byte & 0x7F) << shift;
                    if(!(byte & 0x80))
                        return value;
                    shift += 7;
                }
                bad = true;
                return 0;
            }

            /// Returns the field number, 0 at the end of the message or on error; wire type through `wire`.
            unsigned tag(unsigned& wire)
            {
                if(p >= end)
                    return 0;
                const uint64_t t = varint();
                if(bad)
                    return 0;
                wire = (unsigned)(t & 7);
                return (unsigned)(t >> 3);
            }

            /// A length-delimited field: returns a reader over it and advances past it.
            Reader sub()
            {
                const uint64_t len = varint();
                if(bad || len > left())
                {
                    bad = true;
                    return Reader(p, 0);
                }
                Reader r(p, (size_t)len);
                p += len;
                return r;
            }

            void skip(unsigned wire)
            {
                switch(wire)
                {
                    case 0: varint(); break;
                    case 1: if(left() < 8) bad = true; else p += 8; break;
                    case 2: sub(); break;
                    case 5: if(left() < 4) bad = true; else p += 4; break;
                    default: bad = true; break;
                }
            }

            std::string str()
            {
                Reader r = sub();
                return std::string((const char*)r.p, r.left());
            }
        };

        inline float ReadF32(const uint8_t* q)
        {
            float f;
            memcpy(&f, q, 4);
            return f;
        }

        /// TensorProto: dims (1), data_type (2), float_data (4), name (8), raw_data (9). Non-float tensors are dropped.
        bool ReadTensor(Reader r, TensorMap& out, std::string& error)
        {
            Tensor t;
            std::string name;
            int dataType = 0;
            std::vector<float> floats;
            const uint8_t* raw = 0;
            size_t rawBytes = 0;
            unsigned wire = 0;
            for(unsigned field = r.tag(wire); field; field = r.tag(wire))
            {
                switch(field)
                {
                    case 1:
                        if(wire == 0)
                            t.dims.push_back((int64_t)r.varint());
                        else
                        {
                            Reader packed = r.sub();
                            while(packed.left() && !packed.bad)
                                t.dims.push_back((int64_t)packed.varint());
                        }
                        break;
                    case 2:
                        dataType = (int)r.varint();
                        break;
                    case 4:
                        if(wire == 5)
                        {
                            if(r.left() < 4) { r.bad = true; break; }
                            floats.push_back(ReadF32(r.p));
                            r.p += 4;
                        }
                        else
                        {
                            Reader packed = r.sub();
                            while(packed.left() >= 4)
                            {
                                floats.push_back(ReadF32(packed.p));
                                packed.p += 4;
                            }
                        }
                        break;
                    case 8:
                        name = r.str();
                        break;
                    case 9:
                    {
                        Reader bytes = r.sub();
                        raw = bytes.p;
                        rawBytes = bytes.left();
                        break;
                    }
                    default:
                        r.skip(wire);
                        break;
                }
                if(r.bad)
                {
                    error = "malformed tensor" + (name.empty() ? std::string() : " " + name);
                    return false;
                }
            }
            if(dataType != 1)
                return true;    // int64 shape constants and the like: not weights
            size_t count = 1;
            for(size_t i = 0; i < t.dims.size(); ++i)
                count *= (size_t)(t.dims[i] < 0 ? 0 : t.dims[i]);
            if(raw)
            {
                if(rawBytes != count * 4)
                {
                    error = "tensor " + name + " has " + std::to_string(rawBytes) + " raw bytes for " + std::to_string(count) + " floats";
                    return false;
                }
                t.data.resize(count);
                if(count)
                    memcpy(&t.data[0], raw, rawBytes);
            }
            else
            {
                if(floats.size() != count)
                {
                    error = "tensor " + name + " has " + std::to_string(floats.size()) + " values for " + std::to_string(count) + " floats";
                    return false;
                }
                t.data.swap(floats);
            }
            out[name].dims.swap(t.dims);
            out[name].data.swap(t.data);
            return true;
        }

        /// GraphProto: initializer (5); everything else skipped.
        bool ReadGraph(Reader r, TensorMap& out, std::string& error)
        {
            unsigned wire = 0;
            for(unsigned field = r.tag(wire); field; field = r.tag(wire))
            {
                if(field == 5 && wire == 2)
                {
                    if(!ReadTensor(r.sub(), out, error))
                        return false;
                }
                else
                    r.skip(wire);
                if(r.bad)
                {
                    error = "malformed graph";
                    return false;
                }
            }
            return true;
        }

        /// ModelProto: graph (7), metadata_props (14: key 1, value 2).
        bool ReadModel(const uint8_t* bytes, size_t size, TensorMap& tensors, std::map<std::string, std::string>& meta,
                       std::string& error)
        {
            Reader r(bytes, size);
            unsigned wire = 0;
            bool sawGraph = false;
            for(unsigned field = r.tag(wire); field; field = r.tag(wire))
            {
                if(field == 7 && wire == 2)
                {
                    sawGraph = true;
                    if(!ReadGraph(r.sub(), tensors, error))
                        return false;
                }
                else if(field == 14 && wire == 2)
                {
                    Reader entry = r.sub();
                    std::string key, value;
                    unsigned w2 = 0;
                    for(unsigned f2 = entry.tag(w2); f2; f2 = entry.tag(w2))
                    {
                        if(f2 == 1 && w2 == 2) key = entry.str();
                        else if(f2 == 2 && w2 == 2) value = entry.str();
                        else entry.skip(w2);
                    }
                    meta[key] = value;
                }
                else
                    r.skip(wire);
                if(r.bad)
                {
                    error = "not an ONNX model (malformed protobuf)";
                    return false;
                }
            }
            if(!sawGraph)
            {
                error = "not an ONNX model (no graph)";
                return false;
            }
            return true;
        }
    }

    // ---------------------------------------------------------------------- the model

    struct PolicyModel::Impl
    {
        bool loaded;
        std::string error;
        ModelInfo info;
        TensorMap tensors;
        Net net;
        float emptyKey[CAND_D];     ///< the candidate key of an all-zero feature row, fixed by the weights
        mutable Workspace ws;

        Impl() : loaded(false) { memset(&net, 0, sizeof net); memset(emptyKey, 0, sizeof emptyKey); }

        Tensor* find(const std::string& name)
        {
            TensorMap::iterator it = tensors.find(name);
            return it == tensors.end() ? 0 : &it->second;
        }

        bool fail(const std::string& what)
        {
            error = what;
            return false;
        }

        static std::string Shape(const std::vector<int64_t>& dims)
        {
            std::string s = "[";
            for(size_t i = 0; i < dims.size(); ++i)
                s += (i ? "," : "") + std::to_string((long long)dims[i]);
            return s + "]";
        }

        bool vector(const float*& out, const std::string& name, int n)
        {
            Tensor* t = find(name);
            if(!t)
                return fail("missing weight " + name);
            if((int)t->data.size() != n)
                return fail("weight " + name + " has shape " + Shape(t->dims) + ", expected " + std::to_string(n) + " values");
            out = &t->data[0];
            info.paramCount += (uint32_t)t->data.size();
            return true;
        }

        /// `name.weight` as [out][in], or `name.weight.T` as [in][out] (a MatMul operand the exporter folded), transposed here once.
        bool linear(Linear& L, const std::string& name, int in, int out)
        {
            L.in = in;
            L.out = out;
            Tensor* w = find(name + ".weight");
            bool transposed = false;
            if(!w)
            {
                w = find(name + ".weight.T");
                transposed = true;
            }
            if(!w)
                return fail("missing weight " + name + ".weight");
            if(w->dims.size() != 2 || (int)w->data.size() != in * out)
                return fail("weight " + name + ".weight has shape " + Shape(w->dims) + ", expected [" + std::to_string(out) + "," + std::to_string(in) + "]");
            if(transposed)
            {
                if(w->dims[0] != in || w->dims[1] != out)
                    return fail("weight " + name + ".weight.T has shape " + Shape(w->dims) + ", expected [" + std::to_string(in) + "," + std::to_string(out) + "]");
                std::vector<float> t((size_t)in * out);
                for(int i = 0; i < in; ++i)
                    for(int o = 0; o < out; ++o)
                        t[(size_t)o * in + i] = w->data[(size_t)i * out + o];
                w->data.swap(t);
                w->dims[0] = out;
                w->dims[1] = in;
            }
            else if(w->dims[0] != out || w->dims[1] != in)
                return fail("weight " + name + ".weight has shape " + Shape(w->dims) + ", expected [" + std::to_string(out) + "," + std::to_string(in) + "]");
            L.w = &w->data[0];
            info.paramCount += (uint32_t)w->data.size();
            return vector(L.b, name + ".bias", out);
        }

        bool conv(Conv& c, const std::string& name, int ic, int oc)
        {
            c.ic = ic;
            c.oc = oc;
            Tensor* w = find(name + ".weight");
            if(!w)
                return fail("missing weight " + name + ".weight");
            if(w->dims.size() != 4 || w->dims[0] != oc || w->dims[1] != ic || w->dims[2] != 3 || w->dims[3] != 3)
                return fail("weight " + name + ".weight has shape " + Shape(w->dims) + ", expected [" + std::to_string(oc) + "," + std::to_string(ic) + ",3,3]");
            c.w = &w->data[0];
            info.paramCount += (uint32_t)w->data.size();
            return vector(c.b, name + ".bias", oc);
        }

        bool affine(Affine& a, const std::string& name, int n)
        {
            a.n = n;
            return vector(a.w, name + ".weight", n) && vector(a.b, name + ".bias", n);
        }

        bool resolve()
        {
            Net& n = net;
            info.paramCount = 0;
            if(!conv(n.patchConv[0], "trunk.patch.conv1", PATCH_CHANNELS, 32)) return false;
            if(!affine(n.patchNorm[0], "trunk.patch.norm1", 32)) return false;
            if(!conv(n.patchConv[1], "trunk.patch.conv2", 32, 48)) return false;
            if(!affine(n.patchNorm[1], "trunk.patch.norm2", 48)) return false;
            if(!conv(n.patchConv[2], "trunk.patch.conv3", 48, 64)) return false;
            if(!affine(n.patchNorm[2], "trunk.patch.norm3", 64)) return false;
            if(!linear(n.fusePatches, "trunk.fuse_patches", 2 * PATCH_FEATURES, SPATIAL_D)) return false;
            if(!conv(n.globalConv[0], "trunk.global_map.conv1", GLOBAL_CHANNELS, 16)) return false;
            if(!conv(n.globalConv[1], "trunk.global_map.conv2", 16, 24)) return false;
            if(!linear(n.globalOut, "trunk.global_map.out", 24 * 4 * 4, COARSE_D)) return false;
            if(!linear(n.embed1, "trunk.entities.embed.0", ENTITY_DIMS, ENTITY_D)) return false;
            if(!linear(n.embed2, "trunk.entities.embed.2", ENTITY_D, ENTITY_D)) return false;
            for(int l = 0; l < LAYERS; ++l)
            {
                const std::string prefix = "trunk.entities.layers." + std::to_string(l) + ".";
                EncoderLayer& layer = n.layers[l];
                if(!affine(layer.norm1, prefix + "norm1", ENTITY_D)) return false;
                if(!linear(layer.qkv, prefix + "qkv", ENTITY_D, 3 * ENTITY_D)) return false;
                if(!linear(layer.out, prefix + "out", ENTITY_D, ENTITY_D)) return false;
                if(!affine(layer.norm2, prefix + "norm2", ENTITY_D)) return false;
                if(!linear(layer.ff1, prefix + "ff.0", ENTITY_D, FF)) return false;
                if(!linear(layer.ff2, prefix + "ff.2", FF, ENTITY_D)) return false;
            }
            if(!affine(n.finalNorm, "trunk.entities.final_norm", ENTITY_D)) return false;
            if(!vector(n.query, "trunk.entities.query", ENTITY_D)) return false;
            if(!linear(n.key, "trunk.entities.key", ENTITY_D, ENTITY_D)) return false;
            if(!linear(n.value, "trunk.entities.value", ENTITY_D, ENTITY_D)) return false;
            if(!linear(n.self1, "trunk.self_mlp.0", SELF_DIMS, SELF_D)) return false;
            if(!linear(n.self2, "trunk.self_mlp.2", SELF_D, SELF_D)) return false;
            if(!linear(n.preContext, "trunk.pre_context", BASE_D, HIDDEN)) return false;
            if(!linear(n.commsQuery, "trunk.comms.query", HIDDEN, QUERY_DIMS)) return false;
            if(!linear(n.commsValue, "trunk.comms.value", TACNN_MESSAGE_DIMS, HEARD_D)) return false;
            if(!linear(n.fusion, "trunk.fusion", FUSION_IN, HIDDEN)) return false;
            if(!affine(n.fusionNorm, "trunk.fusion_norm", HIDDEN)) return false;
            if(!vector(n.role, "trunk.role.weight", TACNN_NUM_ROLES * ROLE_D)) return false;
            if(!linear(n.personality, "trunk.personality", TACNN_PERSONALITY_DIMS, PERSONALITY_D)) return false;
            if(!linear(n.film, "trunk.film", COND_D, 2 * HIDDEN)) return false;
            if(!vector(n.gruWih, "trunk.gru.weight_ih", 3 * HIDDEN * HIDDEN)) return false;
            if(!vector(n.gruWhh, "trunk.gru.weight_hh", 3 * HIDDEN * HIDDEN)) return false;
            if(!vector(n.gruBih, "trunk.gru.bias_ih", 3 * HIDDEN)) return false;
            if(!vector(n.gruBhh, "trunk.gru.bias_hh", 3 * HIDDEN)) return false;
            if(!linear(n.type1, "type_head.0", HIDDEN, TYPE_HIDDEN)) return false;
            if(!linear(n.type2, "type_head.2", TYPE_HIDDEN, TACNN_ACTION_TYPES)) return false;
            if(!linear(n.cand1, "cand_mlp.0", TACNN_CANDIDATE_DIMS, CAND_D)) return false;
            if(!linear(n.cand2, "cand_mlp.2", CAND_D, CAND_D)) return false;
            if(!linear(n.targetQuery, "target_query", HIDDEN, CAND_D)) return false;
            if(!linear(n.params1, "params.0", HIDDEN, PARAMS_HIDDEN)) return false;
            if(!linear(n.params2, "params.2", PARAMS_HIDDEN, PARAMS_OUT)) return false;
            if(!linear(n.message, "message", HIDDEN, TACNN_MESSAGE_DIMS)) return false;
            if(!linear(n.signature, "signature", HIDDEN, TACNN_SIGNATURE_DIMS)) return false;
            {
                // empty candidate slots all carry a zero feature row; their key is computed once here,
                // through the same two layers, so the per-slot pass can skip them and answer the same
                float zero[TACNN_CANDIDATE_DIMS];
                float hidden[CAND_D];
                memset(zero, 0, sizeof zero);
                ApplyLinear(n.cand1, zero, hidden);
                GeluInPlace(hidden, CAND_D);
                ApplyLinear(n.cand2, hidden, emptyKey);
            }
            return true;
        }

        // -------------------------------------------------------------- forward

        void patchEncoder(const uint8_t* bytes, float* features) const
        {
            Workspace& w = ws;
            const int count = PATCH_CHANNELS * PATCH * PATCH;
            for(int i = 0; i < count; ++i)
                w.patchIn[i] = bytes[i] * (1.0f / 255.0f);
            int h = PATCH, wd = PATCH, oh, ow;
            Conv3x3s2(net.patchConv[0], w.patchIn, h, wd, w.col, w.act1, oh, ow);
            GroupNorm8(net.patchNorm[0], w.act1, 32, oh * ow);
            GeluInPlace(w.act1, 32 * oh * ow);
            h = oh; wd = ow;
            Conv3x3s2(net.patchConv[1], w.act1, h, wd, w.col, w.act2, oh, ow);
            GroupNorm8(net.patchNorm[1], w.act2, 48, oh * ow);
            GeluInPlace(w.act2, 48 * oh * ow);
            h = oh; wd = ow;
            Conv3x3s2(net.patchConv[2], w.act2, h, wd, w.col, w.act3, oh, ow);
            GroupNorm8(net.patchNorm[2], w.act3, 64, oh * ow);
            GeluInPlace(w.act3, 64 * oh * ow);
            memcpy(features, w.act3, sizeof(float) * PATCH_FEATURES);
        }

        void globalEncoder(const uint8_t* bytes, float* coarse) const
        {
            Workspace& w = ws;
            const int count = GLOBAL_CHANNELS * GLOBAL * GLOBAL;
            for(int i = 0; i < count; ++i)
                w.globalIn[i] = bytes[i] * (1.0f / 255.0f);
            int oh, ow;
            Conv3x3s2(net.globalConv[0], w.globalIn, GLOBAL, GLOBAL, w.col, w.gact1, oh, ow);
            GeluInPlace(w.gact1, 16 * oh * ow);
            int h = oh, wd = ow;
            Conv3x3s2(net.globalConv[1], w.gact1, h, wd, w.col, w.gact2, oh, ow);
            GeluInPlace(w.gact2, 24 * oh * ow);
            ApplyLinear(net.globalOut, w.gact2, coarse);
            GeluInPlace(coarse, COARSE_D);
        }

        void encoderLayer(const EncoderLayer& layer, const bool* padding, int rows) const
        {
            Workspace& w = ws;
            const float scale = 1.0f / sqrtf((float)HEAD_D);
            for(int i = 0; i < rows; ++i)
                LayerNorm(layer.norm1, w.ent + i * ENTITY_D, w.entNorm + i * ENTITY_D);
            ApplyLinearRows(layer.qkv, w.entNorm, rows, w.qkv);
            for(int hd = 0; hd < HEADS; ++hd)
            {
                const int off = hd * HEAD_D;
                for(int i = 0; i < rows; ++i)
                {
                    const float* q = w.qkv + i * 3 * ENTITY_D + off;
                    for(int j = 0; j < rows; ++j)
                    {
                        const float* k = w.qkv + j * 3 * ENTITY_D + ENTITY_D + off;
                        w.scores[j] = padding[j] ? MASK_FILL : Dot(q, k, HEAD_D) * scale;
                    }
                    Softmax(w.scores, rows);
                    float* out = w.mixed + i * ENTITY_D + off;
                    for(int d = 0; d < HEAD_D; ++d)
                        out[d] = 0.0f;
                    for(int j = 0; j < rows; ++j)
                    {
                        const float a = w.scores[j];
                        const float* v = w.qkv + j * 3 * ENTITY_D + 2 * ENTITY_D + off;
                        for(int d = 0; d < HEAD_D; ++d)
                            out[d] += a * v[d];
                    }
                }
            }
            ApplyLinearRows(layer.out, w.mixed, rows, w.entTmp);
            for(int i = 0; i < rows * ENTITY_D; ++i)
                w.ent[i] += w.entTmp[i];
            for(int i = 0; i < rows; ++i)
                LayerNorm(layer.norm2, w.ent + i * ENTITY_D, w.entNorm + i * ENTITY_D);
            ApplyLinearRows(layer.ff1, w.entNorm, rows, w.ffHidden);
            GeluInPlace(w.ffHidden, rows * FF);
            ApplyLinearRows(layer.ff2, w.ffHidden, rows, w.entTmp);
            for(int i = 0; i < rows * ENTITY_D; ++i)
                w.ent[i] += w.entTmp[i];
        }

        /**
         * The entity transformer. Padded rows only ever enter the network as
         * masked keys, whose softmax weight underflows to exactly zero, and as
         * masked pooling terms, so the present rows come out the same whether
         * the padded ones are computed or not; they are dropped here and the
         * layers run on the present rows alone, in their original order. A
         * fully padded set (not something the builder produces) keeps every
         * row, as the Python model then pools all of them evenly.
         */
        void entityEncoder(const TacnnObservation& obs, float* pooled) const
        {
            Workspace& w = ws;
            bool padding[ENTITIES];
            int rows = 0;
            for(int i = 0; i < ENTITIES; ++i)
            {
                if(obs.entity_mask[i] == 0)
                    continue;
                memcpy(w.entIn + rows * ENTITY_DIMS, obs.entities[i], sizeof(float) * ENTITY_DIMS);
                padding[rows] = false;
                ++rows;
            }
            const float* input = w.entIn;
            if(rows == 0)
            {
                rows = ENTITIES;
                input = &obs.entities[0][0];
                for(int i = 0; i < ENTITIES; ++i)
                    padding[i] = true;
            }
            ApplyLinearRows(net.embed1, input, rows, w.entTmp);
            GeluInPlace(w.entTmp, rows * ENTITY_D);
            ApplyLinearRows(net.embed2, w.entTmp, rows, w.ent);
            for(int l = 0; l < LAYERS; ++l)
                encoderLayer(net.layers[l], padding, rows);
            for(int i = 0; i < rows; ++i)
                LayerNorm(net.finalNorm, w.ent + i * ENTITY_D, w.entNorm + i * ENTITY_D);
            ApplyLinearRows(net.key, w.entNorm, rows, w.keys);
            ApplyLinearRows(net.value, w.entNorm, rows, w.values);
            const float scale = 1.0f / sqrtf((float)ENTITY_D);
            for(int i = 0; i < rows; ++i)
                w.scores[i] = padding[i] ? MASK_FILL : Dot(w.keys + i * ENTITY_D, net.query, ENTITY_D) * scale;
            Softmax(w.scores, rows);
            for(int d = 0; d < ENTITY_D; ++d)
                pooled[d] = 0.0f;
            for(int i = 0; i < rows; ++i)
            {
                const float a = w.scores[i];
                const float* v = w.values + i * ENTITY_D;
                for(int d = 0; d < ENTITY_D; ++d)
                    pooled[d] += a * v[d];
            }
        }

        void commsRead(const InferInputs& in, float* heard) const
        {
            Workspace& w = ws;
            ApplyLinear(net.commsQuery, w.context, w.commsQ);
            const float scale = 1.0f / sqrtf((float)QUERY_DIMS);
            for(int s = 0; s < TACNN_INBOX; ++s)
                w.commsScores[s] = in.inbox_mask[s] <= 0.5f ? MASK_FILL : Dot(in.inbox_sig[s], w.commsQ, TACNN_SIGNATURE_DIMS) * scale;
            Softmax(w.commsScores, TACNN_INBOX);
            for(int d = 0; d < HEARD_D; ++d)
                heard[d] = 0.0f;
            for(int s = 0; s < TACNN_INBOX; ++s)
            {
                const float weight = w.commsScores[s] * in.inbox_mask[s];
                ApplyLinear(net.commsValue, in.inbox_msg[s], w.commsValue);
                for(int d = 0; d < HEARD_D; ++d)
                    heard[d] += weight * w.commsValue[d];
            }
        }

        void forward(const TacnnObservation& obs, const TacnnCandidateSet& cand, const InferInputs& in, InferOutputs& out) const
        {
            Workspace& w = ws;
            // section 4.2: the siamese patch encoder and the fused spatial summary
            patchEncoder(&obs.fine[0][0][0], w.patches);
            patchEncoder(&obs.medium[0][0][0], w.patches + PATCH_FEATURES);
            ApplyLinear(net.fusePatches, w.patches, w.spatial);
            GeluInPlace(w.spatial, SPATIAL_D);
            // 4.4 the coarse map, 4.3 the entities, the self MLP
            globalEncoder(&obs.global_map[0][0][0], w.coarse);
            entityEncoder(obs, w.ents);
            ApplyLinear(net.self1, obs.self_vec, w.me1);
            GeluInPlace(w.me1, SELF_D);
            ApplyLinear(net.self2, w.me1, w.me);
            GeluInPlace(w.me, SELF_D);
            memcpy(w.base, w.spatial, sizeof(float) * SPATIAL_D);
            memcpy(w.base + SPATIAL_D, w.coarse, sizeof(float) * COARSE_D);
            memcpy(w.base + SPATIAL_D + COARSE_D, w.ents, sizeof(float) * ENTITY_D);
            memcpy(w.base + SPATIAL_D + COARSE_D + ENTITY_D, w.me, sizeof(float) * SELF_D);
            // 4.6 the inbox, keyed by a context without it
            ApplyLinear(net.preContext, w.base, w.context);
            GeluInPlace(w.context, HIDDEN);
            commsRead(in, w.base + BASE_D);
            // fusion, FiLM
            ApplyLinear(net.fusion, w.base, w.fusionRaw);
            LayerNorm(net.fusionNorm, w.fusionRaw, w.fused);
            GeluInPlace(w.fused, HIDDEN);
            const int role = obs.role < TACNN_NUM_ROLES ? obs.role : 0;
            memcpy(w.cond, net.role + role * ROLE_D, sizeof(float) * ROLE_D);
            ApplyLinear(net.personality, obs.personality, w.persona);
            for(int i = 0; i < PERSONALITY_D; ++i)
                w.cond[ROLE_D + i] = tanhf(w.persona[i]);
            ApplyLinear(net.film, w.cond, w.filmOut);
            for(int i = 0; i < HIDDEN; ++i)
                w.feat[i] = w.filmOut[i] * w.fused[i] + w.filmOut[HIDDEN + i];
            // the GRU cell: gates r, z, n in torch's order
            {
                Linear ih = { net.gruWih, net.gruBih, HIDDEN, 3 * HIDDEN };
                Linear hh = { net.gruWhh, net.gruBhh, HIDDEN, 3 * HIDDEN };
                ApplyLinear(ih, w.feat, w.gi);
                ApplyLinear(hh, in.h_in, w.gh);
            }
            for(int i = 0; i < HIDDEN; ++i)
            {
                const float r = Sigmoid(w.gi[i] + w.gh[i]);
                const float z = Sigmoid(w.gi[HIDDEN + i] + w.gh[HIDDEN + i]);
                const float nn = tanhf(w.gi[2 * HIDDEN + i] + r * w.gh[2 * HIDDEN + i]);
                out.h_out[i] = (1.0f - z) * nn + z * in.h_in[i];
            }
            const float* h = out.h_out;
            // 4.7 the heads
            ApplyLinear(net.type1, h, w.typeHidden);
            GeluInPlace(w.typeHidden, TYPE_HIDDEN);
            ApplyLinear(net.type2, w.typeHidden, out.type_logits);
            ApplyLinear(net.targetQuery, h, w.targetQuery);
            const float targetScale = 1.0f / sqrtf((float)CAND_D);
            float emptyLogit = 0.0f;
            bool emptySeen = false;
            for(int k = 0; k < TACNN_CANDIDATES; ++k)
            {
                const float* f = cand.features[k];
                bool zero = true;
                for(int d = 0; d < TACNN_CANDIDATE_DIMS && zero; ++d)
                    zero = f[d] == 0.0f;
                if(zero)
                {
                    if(!emptySeen)
                    {
                        emptyLogit = Dot(emptyKey, w.targetQuery, CAND_D) * targetScale;
                        emptySeen = true;
                    }
                    out.target_logits[k] = emptyLogit;
                    continue;
                }
                ApplyLinear(net.cand1, f, w.candHidden);
                GeluInPlace(w.candHidden, CAND_D);
                ApplyLinear(net.cand2, w.candHidden, w.candKeys);
                out.target_logits[k] = Dot(w.candKeys, w.targetQuery, CAND_D) * targetScale;
            }
            ApplyLinear(net.params1, h, w.paramsHidden);
            GeluInPlace(w.paramsHidden, PARAMS_HIDDEN);
            ApplyLinear(net.params2, w.paramsHidden, w.params);
            memcpy(out.aim_logits, w.params, sizeof(float) * TACNN_AIM_LEVELS);
            memcpy(out.mode_logits, w.params + TACNN_AIM_LEVELS, sizeof(float) * TACNN_FIRE_MODES);
            memcpy(out.stance_logits, w.params + TACNN_AIM_LEVELS + TACNN_FIRE_MODES, sizeof(float) * TACNN_STANCES);
            ApplyLinear(net.message, h, out.message);
            for(int i = 0; i < TACNN_MESSAGE_DIMS; ++i)
                out.message[i] = tanhf(out.message[i]);
            ApplyLinear(net.signature, h, out.signature);
        }
    };

    PolicyModel::PolicyModel() : impl_(new Impl()) {}

    PolicyModel::~PolicyModel()
    {
        delete impl_;
    }

    bool PolicyModel::load(const uint8_t* bytes, size_t size, uint32_t expectedHash)
    {
        Impl& m = *impl_;
        unload();
        std::map<std::string, std::string> meta;
        if(!ReadModel(bytes, size, m.tensors, meta, m.error))
        {
            m.tensors.clear();
            return false;
        }
        m.info.fileBytes = size;
        m.info.initializers = m.tensors.size();
        m.info.generation = meta["generation"];
        m.info.gameBuild = meta["game_build"];
        m.info.schemaHash = meta.count("obs_schema") ? (uint32_t)strtoul(meta["obs_schema"].c_str(), 0, 0) : 0;
        m.info.schemaVersion = meta.count("obs_schema_version") ? atoi(meta["obs_schema_version"].c_str()) : 0;
        m.info.trainedSteps = meta.count("trained_steps") ? (uint32_t)strtoul(meta["trained_steps"].c_str(), 0, 10) : 0;
        if(expectedHash != 0)
        {
            if(!meta.count("obs_schema"))
            {
                m.tensors.clear();
                return m.fail("model carries no obs_schema metadata; export it with tools/tacrl/export.py");
            }
            if(m.info.schemaHash != expectedHash)
            {
                char text[160];
                snprintf(text, sizeof text, "model schema 0x%08X (version %d) does not match this build's 0x%08X (version %d)",
                         m.info.schemaHash, m.info.schemaVersion, expectedHash, TACNN_SCHEMA_VERSION);
                m.tensors.clear();
                return m.fail(text);
            }
        }
        if(!m.resolve())
        {
            m.tensors.clear();
            return false;
        }
        m.error.clear();
        m.loaded = true;
        return true;
    }

    bool PolicyModel::loadFile(const char* path, uint32_t expectedHash)
    {
        FILE* f = fopen(path, "rb");
        if(!f)
            return impl_->fail(std::string("cannot open ") + path);
        std::vector<uint8_t> bytes;
        uint8_t chunk[65536];
        size_t got;
        while((got = fread(chunk, 1, sizeof chunk, f)) > 0)
            bytes.insert(bytes.end(), chunk, chunk + got);
        fclose(f);
        if(bytes.empty())
            return impl_->fail(std::string("empty file ") + path);
        return load(&bytes[0], bytes.size(), expectedHash);
    }

    void PolicyModel::unload()
    {
        impl_->loaded = false;
        impl_->tensors.clear();
        impl_->info = ModelInfo();
        memset(&impl_->net, 0, sizeof impl_->net);
    }

    bool PolicyModel::loaded() const
    {
        return impl_->loaded;
    }

    const std::string& PolicyModel::error() const
    {
        return impl_->error;
    }

    const ModelInfo& PolicyModel::info() const
    {
        return impl_->info;
    }

    void PolicyModel::infer(const TacnnObservation& obs, const TacnnCandidateSet& cand, const InferInputs& in,
                            InferOutputs& out) const
    {
        if(!impl_->loaded)
        {
            memset(&out, 0, sizeof out);
            return;
        }
        impl_->forward(obs, cand, in, out);
    }

    // ---------------------------------------------------------------------- decode

    int MaskedArgmax(const float* logits, const uint8_t* mask, int count)
    {
        int best = -1;
        for(int i = 0; i < count; ++i)
        {
            if(!mask[i])
                continue;
            if(best < 0 || logits[i] > logits[best])
                best = i;
        }
        return best < 0 ? 0 : best;
    }

    void DecodeGreedy(const InferOutputs& out, const TacnnActionMask& mask, const TacnnCandidateSet& cand,
                      TacnnPolicyAction& action)
    {
        memset(&action, 0, sizeof action);
        const int type = MaskedArgmax(out.type_logits, mask.type, TACNN_ACTION_TYPES);
        uint8_t row[TACNN_CANDIDATES];
        bool any = false;
        for(int k = 0; k < TACNN_CANDIDATES; ++k)
        {
            const int kind = cand.kind[k] < TACNN_CAND_COUNT ? (int)cand.kind[k] : (int)TACNN_CAND_EMPTY;
            row[k] = (mask.target[k] && tacnn_kind_compat[type][kind]) ? 1 : 0;
            any = any || row[k];
        }
        action.type = (uint8_t)type;
        action.target_slot = any ? (uint8_t)MaskedArgmax(out.target_logits, row, TACNN_CANDIDATES) : 0;
        action.aim = (uint8_t)MaskedArgmax(out.aim_logits, mask.aim, TACNN_AIM_LEVELS);
        action.mode = (uint8_t)MaskedArgmax(out.mode_logits, mask.mode, TACNN_FIRE_MODES);
        action.stance = (uint8_t)MaskedArgmax(out.stance_logits, mask.stance, TACNN_STANCES);
        float checksum = 0.0f;
        for(int i = 0; i < TACNN_ACTION_TYPES; ++i)
            checksum += out.type_logits[i];
        action.logits_checksum = checksum;
    }

    // ---------------------------------------------------------------------- the radio

    CommsRing::CommsRing(int worldCols) : cols_(worldCols > 0 ? worldCols : 160) {}

    void CommsRing::clear()
    {
        entries_.clear();
    }

    void CommsRing::setColumns(int worldCols)
    {
        if(worldCols > 0)
            cols_ = worldCols;
    }

    int CommsRing::TileDistance(int32_t a, int32_t b, int cols)
    {
        // Isometric Utils.cpp PythSpacesAway, on a square map where MAXCOL == MAXROW
        const int rows = (a / cols) - (b / cols);
        const int columns = (a % cols) - (b % cols);
        return (int)sqrt((double)(rows * rows + columns * columns));
    }

    void CommsRing::push(int agent, int32_t gridno, const float* message, const float* signature)
    {
        Entry e;
        e.agent = agent;
        e.gridno = gridno;
        for(int i = 0; i < TACNN_MESSAGE_DIMS; ++i)
        {
            float v = message[i];
            if(v > 1.0f) v = 1.0f;
            if(v < -1.0f) v = -1.0f;
            // numpy rounds half to even; so does nearbyint under the default rounding mode
            e.message[i] = nearbyintf(v * 127.0f) / 127.0f;
        }
        memcpy(e.signature, signature, sizeof e.signature);
        entries_.push_back(e);
        if(entries_.size() > (size_t)TACNN_INBOX)
            entries_.erase(entries_.begin(), entries_.begin() + (entries_.size() - TACNN_INBOX));
    }

    void CommsRing::inboxFor(int agent, int32_t gridno, InferInputs& in) const
    {
        memset(in.inbox_msg, 0, sizeof in.inbox_msg);
        memset(in.inbox_sig, 0, sizeof in.inbox_sig);
        memset(in.inbox_mask, 0, sizeof in.inbox_mask);
        for(size_t slot = 0; slot < entries_.size() && slot < (size_t)TACNN_INBOX; ++slot)
        {
            const Entry& e = entries_[slot];
            if(e.agent == agent)
                continue;
            if(gridno >= 0 && e.gridno >= 0 && TileDistance(e.gridno, gridno, cols_) > TACNN_RADIO_RANGE_TILES)
                continue;
            memcpy(in.inbox_msg[slot], e.message, sizeof e.message);
            memcpy(in.inbox_sig[slot], e.signature, sizeof e.signature);
            in.inbox_mask[slot] = 1.0f;
        }
    }

    // ---------------------------------------------------------------------- hidden state

    HiddenStore::HiddenStore()
        : state_((size_t)TACNN_MAX_SOLDIERS * TACNN_HIDDEN, 0.0f), present_(TACNN_MAX_SOLDIERS, 0) {}

    void HiddenStore::clear()
    {
        std::fill(state_.begin(), state_.end(), 0.0f);
        std::fill(present_.begin(), present_.end(), (uint8_t)0);
    }

    bool HiddenStore::get(int soldier, float* h) const
    {
        if(soldier < 0 || soldier >= TACNN_MAX_SOLDIERS || !present_[soldier])
        {
            memset(h, 0, sizeof(float) * TACNN_HIDDEN);
            return false;
        }
        memcpy(h, &state_[(size_t)soldier * TACNN_HIDDEN], sizeof(float) * TACNN_HIDDEN);
        return true;
    }

    void HiddenStore::set(int soldier, const float* h)
    {
        if(soldier < 0 || soldier >= TACNN_MAX_SOLDIERS)
            return;
        memcpy(&state_[(size_t)soldier * TACNN_HIDDEN], h, sizeof(float) * TACNN_HIDDEN);
        present_[soldier] = 1;
    }

    bool HiddenStore::has(int soldier) const
    {
        return soldier >= 0 && soldier < TACNN_MAX_SOLDIERS && present_[soldier] != 0;
    }

    int64_t NowUs()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }
}
