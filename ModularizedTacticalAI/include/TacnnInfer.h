/**
 * @file
 * @author ja2mod
 *
 * In-process inference for the exported policy. Added by the ja2mod fork on
 * 2026-09-16; see CHANGES-ja2mod.md.
 *
 * The exported graph (tools/tacrl/export.py in the ja2mod repository) is the
 * actor of tools/tacrl/model/policy.py with every shape fixed: two 16x20x20
 * patches, the 6x16x16 map, 20 entities, 48 candidates, an inbox of 4 and a
 * 192-wide GRU state. Rather than link ONNX Runtime, which has no 32-bit
 * static /MT build, this file reads the weights straight out of the .onnx
 * file (a protobuf; only the initializers and the metadata are read) and
 * runs the same network in plain fp32 C++: convolutions, linear layers,
 * group and layer normalisation, the two-layer entity transformer with its
 * attention pooling, the TarMAC inbox read, FiLM, the GRU cell and the five
 * heads. The architecture is hard-wired here; the weights are looked up by
 * their PyTorch parameter names, which the exporter restores after ONNX's
 * constant folding. A missing or misshapen weight fails the load, never the
 * forward pass.
 *
 * Also here, because they are the network's contract with the world rather
 * than the engine's: the greedy masked decode of the heads (the same rule as
 * tacrl.model.policy.ActionHeads.sample(greedy=True)), the team radio ring of
 * tools/tacrl/comms.py, and the per-soldier hidden-state store. No engine
 * dependencies, so tools/test_tacrl_embedded.py compiles this file with g++
 * and compares it against onnxruntime.
 */

#ifndef TACNN_INFER_H
#define TACNN_INFER_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

#include "tacnn_schema.h"

namespace tacnn
{
    /// What the model file says about itself (metadata_props written by export.py).
    struct ModelInfo
    {
        std::string generation;
        std::string gameBuild;
        uint32_t schemaHash;        ///< obs_schema; 0 when the file carries none
        int schemaVersion;
        uint32_t paramCount;        ///< floats in all initializers the network uses
        uint32_t trainedSteps;
        size_t fileBytes;
        size_t initializers;        ///< tensors found in the file
        ModelInfo() : schemaHash(0), schemaVersion(0), paramCount(0), trainedSteps(0), fileBytes(0), initializers(0) {}
    };

    /// The network inputs that are neither in the observation nor in the candidate set.
    struct InferInputs
    {
        float h_in[TACNN_HIDDEN];
        float inbox_msg[TACNN_INBOX][TACNN_MESSAGE_DIMS];
        float inbox_sig[TACNN_INBOX][TACNN_SIGNATURE_DIMS];
        float inbox_mask[TACNN_INBOX];
    };

    /// Everything one forward pass answers, in the exporter's output order.
    struct InferOutputs
    {
        float type_logits[TACNN_ACTION_TYPES];
        float target_logits[TACNN_CANDIDATES];
        float aim_logits[TACNN_AIM_LEVELS];
        float mode_logits[TACNN_FIRE_MODES];
        float stance_logits[TACNN_STANCES];
        float message[TACNN_MESSAGE_DIMS];
        float signature[TACNN_SIGNATURE_DIMS];
        float h_out[TACNN_HIDDEN];
    };

    /**@class PolicyModel
     * @brief The exported actor, loaded from its .onnx bytes and run in fp32.
     */
    class PolicyModel
    {
        public:
            PolicyModel();
            ~PolicyModel();

            /**@brief Parse a model file already in memory and resolve every weight.
             * @param bytes The .onnx file.
             * @param size Its length.
             * @param expectedHash The schema hash the model must have been exported against
             *        (normally TACNN_SCHEMA_HASH); 0 skips the check. A file without the
             *        metadata fails the check unless it is skipped.
             * @return false with error() set; the model is then not loaded.
             */
            bool load(const uint8_t* bytes, size_t size, uint32_t expectedHash = TACNN_SCHEMA_HASH);
            /// Same from a path through stdio, for tests and tools; the engine reads through its VFS and calls load().
            bool loadFile(const char* path, uint32_t expectedHash = TACNN_SCHEMA_HASH);
            void unload();

            bool loaded() const;
            const std::string& error() const;
            const ModelInfo& info() const;

            /**@brief One decision: the forward pass at batch 1.
             *
             * Patch and map bytes of the observation are scaled to [0, 1] here, as
             * tacrl.model.policy.encode_batch does. No allocation, no failure path:
             * a loaded model always answers.
             */
            void infer(const TacnnObservation& obs, const TacnnCandidateSet& cand, const InferInputs& in,
                       InferOutputs& out) const;

        private:
            struct Impl;
            Impl* impl_;
            PolicyModel(const PolicyModel&);
            PolicyModel& operator=(const PolicyModel&);
    };

    /**@brief Greedy decode of the heads under the masks.
     *
     * The type is the legal type with the highest logit; the slot the highest
     * target logit among the slots whose candidate kind suits that type
     * (tacnn_kind_compat) and that the target mask allows, slot 0 when there is
     * none; aim, mode and stance the highest legal logit of their heads. Ties
     * go to the lowest index, as torch.argmax does. Identical to
     * tacrl.model.policy.ActionHeads.sample(greedy=True), which is what the
     * parity tool checks.
     */
    void DecodeGreedy(const InferOutputs& out, const TacnnActionMask& mask, const TacnnCandidateSet& cand,
                      TacnnPolicyAction& action);

    /// Index of the highest logit among the entries with mask set; 0 when none is set. Ties go to the lowest index.
    int MaskedArgmax(const float* logits, const uint8_t* mask, int count);

    /**@class CommsRing
     * @brief One team's radio in one battle: the FIFO of tools/tacrl/comms.py.
     *
     * A message written during one soldier's decision is heard from the next
     * decision on; a reader never hears his own last call, nor a sender more
     * than TACNN_RADIO_RANGE_TILES away; messages are quantised to 8 bits per
     * dimension on the way in. The training environment also drops a share of
     * the writes at random; the engine keeps every one (drop 0), the
     * deterministic choice, and the record of what was heard goes to the log.
     */
    class CommsRing
    {
        public:
            explicit CommsRing(int worldCols = 160);
            void clear();
            /// The map's column count, for the range test; the engine learns it when a sector loads.
            void setColumns(int worldCols);
            /// A soldier transmits at the end of his decision.
            void push(int agent, int32_t gridno, const float* message, const float* signature);
            /// Fill the inbox part of `in` for a reader; the hidden state is left alone.
            void inboxFor(int agent, int32_t gridno, InferInputs& in) const;
            size_t size() const { return entries_.size(); }
            /// The engine's PythSpacesAway: truncated Euclidean tile distance.
            static int TileDistance(int32_t a, int32_t b, int cols);

        private:
            struct Entry
            {
                int agent;
                int32_t gridno;
                float message[TACNN_MESSAGE_DIMS];
                float signature[TACNN_SIGNATURE_DIMS];
            };
            std::vector<Entry> entries_;
            int cols_;
    };

    /**@class HiddenStore
     * @brief The recurrent state of every soldier, zero until his first decision of the battle.
     */
    class HiddenStore
    {
        public:
            HiddenStore();
            void clear();
            /// Copy a soldier's state into `h`; zeros when he has none yet. Returns whether he had one.
            bool get(int soldier, float* h) const;
            void set(int soldier, const float* h);
            bool has(int soldier) const;

        private:
            std::vector<float> state_;
            std::vector<uint8_t> present_;
    };

    /// Monotonic microseconds, for the latency fields of the log.
    int64_t NowUs();
}

#endif
