/**
 * @file
 * @author ja2mod
 *
 * Client of the policy sidecar. Added by the ja2mod fork on 2026-09-15; see
 * CHANGES-ja2mod.md.
 *
 * The game sends one TacnnSidecarRequest frame (fixed head, then the
 * observation, the action mask and the candidate set) per decision and
 * expects one TacnnSidecarResponse back within the deadline. Any failure --
 * no server, a timeout, a short read, a wrong magic or schema hash -- makes
 * request() return false, drops the connection, and leaves the decision to
 * the legacy AI. The next request reconnects, after a short back-off so a
 * missing server does not cost every decision the full timeout.
 *
 * On Windows the transport is the named pipe `\\.\pipe\ja2mod-policy` with
 * overlapped I/O, which is what gives the deadline teeth. On other systems
 * (the Linux syntax check and the local round-trip test) the same framing
 * runs over a TCP socket to 127.0.0.1, port taken from a `tcp:<port>` name.
 * No engine dependencies.
 */

#ifndef TACNN_SIDECAR_CLIENT_H
#define TACNN_SIDECAR_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <string>

#include "tacnn_schema.h"

namespace tacnn
{
    /// The bytes on the wire, without any OS I/O: shared by the client and its tests.
    namespace protocol
    {
        enum { REQUEST_BYTES = sizeof(TacnnSidecarRequest) + TACNN_REQUEST_BLOB_BYTES };
        enum { RESPONSE_BYTES = sizeof(TacnnSidecarResponse) };

        /// Fill `out[REQUEST_BYTES]`; the head's magic, version and hash are set here.
        void encodeRequest(TacnnSidecarRequest head, const TacnnObservation& obs, const TacnnActionMask& mask,
                           const TacnnCandidateSet& cand, uint8_t* out);
        /// False unless the magic, sequence and schema hash match.
        bool decodeResponse(const uint8_t* in, uint32_t expectedSequence, TacnnPolicyAction& action);
    }

    class Transport;

    class SidecarClient
    {
        public:
            SidecarClient();
            ~SidecarClient();

            /**@param name Pipe name on Windows (e.g. `\\.\pipe\ja2mod-policy`), `tcp:<port>` elsewhere.
             * @param timeoutMs Deadline for connect, write and read together.
             */
            void configure(const std::string& name, unsigned timeoutMs);
            const std::string& name() const { return name_; }
            unsigned timeoutMs() const { return timeoutMs_; }

            /**@brief One decision round trip.
             * @return true and `action` when the server answered in time with a valid frame that
             *         did not decline; false otherwise. `declined()` tells the two apart.
             */
            bool request(TacnnSidecarRequest head, const TacnnObservation& obs, const TacnnActionMask& mask,
                         const TacnnCandidateSet& cand, TacnnPolicyAction& action);

            bool connected() const;
            bool declined() const { return lastDeclined_; }
            const char* lastError() const { return lastError_; }
            unsigned failures() const { return failures_; }
            unsigned answered() const { return answered_; }
            void disconnect();

        private:
            bool ensureConnected(int64_t deadline);

            std::string name_;
            unsigned timeoutMs_;
            Transport* transport_;
            uint32_t sequence_;
            unsigned failures_;
            unsigned answered_;
            int64_t retryAfter_;
            bool lastDeclined_;
            const char* lastError_;

            SidecarClient(const SidecarClient&);
            SidecarClient& operator=(const SidecarClient&);
    };

    /// Monotonic milliseconds, for deadlines.
    int64_t NowMs();

    /// The one client of the running game.
    SidecarClient& GetSidecarClient();
}

#endif
