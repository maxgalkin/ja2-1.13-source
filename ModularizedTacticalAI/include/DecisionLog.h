/**
 * @file
 * @author ja2mod
 *
 * Binary per-decision log. Added by the ja2mod fork on 2026-09-15; see
 * CHANGES-ja2mod.md.
 *
 * Every record is a TacnnRecordHeader followed by its payload, laid out as
 * tacnn_schema.h says; tools/tacsim/logreader.py in the ja2mod repository
 * reads the files back. Records go to `tacnn-<date>-<time>-<n>.tlog` in the
 * configured directory. A file is closed and a new one started when it
 * passes the per-file cap, and after each rollover the oldest files are
 * deleted until the directory is under the total cap, so the log is a FIFO
 * over battles that never fills the disk.
 *
 * No engine dependencies; the glue (NeuralHooks.cpp) decides what to record.
 */

#ifndef TACNN_DECISION_LOG_H
#define TACNN_DECISION_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string>

#include "tacnn_schema.h"

namespace tacnn
{
    class DecisionLog
    {
        public:
            DecisionLog();
            ~DecisionLog();

            /**@brief Start logging into `directory`.
             * @param directory Created if missing.
             * @param totalCapBytes Oldest files are removed past this; 0 keeps the default of 2 GiB.
             * @param fileCapBytes A new file is started past this; 0 keeps the default of 256 MiB.
             * @return false if the directory or the first file could not be created.
             */
            bool open(const std::string& directory, uint64_t totalCapBytes = 0, uint64_t fileCapBytes = 0);
            void close();
            bool isOpen() const { return file_ != 0; }

            void writeBattleStart(const TacnnBattleStart& start);
            void writeDecision(const TacnnDecision& head, const TacnnObservation& obs, const TacnnActionMask& mask,
                               const TacnnCandidateSet& cand, const TacnnPolicyAction& action);
            void writeOutcome(const TacnnOutcome& outcome);
            /// Right after the decision record of a decision the in-process network made (2026-09-16).
            void writeEmbedded(const TacnnEmbedded& embedded);
            void writeEpisodeEnd(const TacnnEpisodeEnd& end);
            void flush();

            uint64_t bytesWritten() const { return totalWritten_; }
            unsigned recordsWritten() const { return records_; }
            const std::string& currentFile() const { return currentPath_; }
            const std::string& directory() const { return directory_; }

            /// Raw access for tests and for record types added later.
            void writeRecord(uint16_t type, const void* payload, size_t bytes);
            /// Same, payload in pieces (a decision record is five of them).
            void writeRecord(uint16_t type, const void* const* parts, const size_t* sizes, size_t count);

        private:
            bool openNewFile();
            void enforceTotalCap();

            FILE* file_;
            std::string directory_;
            std::string currentPath_;
            uint64_t currentBytes_;
            uint64_t totalWritten_;
            uint64_t totalCap_;
            uint64_t fileCap_;
            unsigned sequence_;
            unsigned records_;
    };

    /// The one log of the running game.
    DecisionLog& GetDecisionLog();
}

#endif
