/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 */

#include "../include/DecisionLog.h"

#include <string.h>
#include <time.h>
#include <algorithm>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace tacnn
{
    namespace
    {
        const uint64_t DEFAULT_TOTAL_CAP = 2ull * 1024 * 1024 * 1024;
        const uint64_t DEFAULT_FILE_CAP = 256ull * 1024 * 1024;
        const char* const EXTENSION = ".tlog";

        struct Entry
        {
            fs::path path;
            fs::file_time_type time;
            uint64_t size;
        };

        void listLogs(const fs::path& dir, std::vector<Entry>& out)
        {
            std::error_code ec;
            for(fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                const fs::directory_entry& e = *it;
                if(!e.is_regular_file(ec) || e.path().extension() != EXTENSION)
                    continue;
                Entry entry;
                entry.path = e.path();
                entry.time = e.last_write_time(ec);
                entry.size = static_cast<uint64_t>(e.file_size(ec));
                out.push_back(entry);
            }
            std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
                if(a.time != b.time)
                    return a.time < b.time;
                return a.path.filename().string() < b.path.filename().string();
            });
        }
    }

    DecisionLog::DecisionLog()
        : file_(0), currentBytes_(0), totalWritten_(0), totalCap_(DEFAULT_TOTAL_CAP), fileCap_(DEFAULT_FILE_CAP),
          sequence_(0), records_(0)
    {
    }

    DecisionLog::~DecisionLog()
    {
        close();
    }

    bool DecisionLog::open(const std::string& directory, uint64_t totalCapBytes, uint64_t fileCapBytes)
    {
        close();
        directory_ = directory;
        totalCap_ = totalCapBytes ? totalCapBytes : DEFAULT_TOTAL_CAP;
        fileCap_ = fileCapBytes ? fileCapBytes : DEFAULT_FILE_CAP;
        std::error_code ec;
        fs::create_directories(fs::path(directory_), ec);
        if(!fs::is_directory(fs::path(directory_), ec))
            return false;
        return openNewFile();
    }

    void DecisionLog::close()
    {
        if(file_)
        {
            fclose(file_);
            file_ = 0;
        }
        currentPath_.clear();
        currentBytes_ = 0;
    }

    bool DecisionLog::openNewFile()
    {
        if(file_)
        {
            fclose(file_);
            file_ = 0;
        }
        time_t now = time(0);
        struct tm stamp;
#ifdef _WIN32
        localtime_s(&stamp, &now);
#else
        localtime_r(&now, &stamp);
#endif
        char name[64];
        strftime(name, sizeof name, "tacnn-%Y%m%d-%H%M%S", &stamp);
        char full[96];
        snprintf(full, sizeof full, "%s-%03u%s", name, sequence_++ % 1000, EXTENSION);
        fs::path path = fs::path(directory_) / full;
        currentPath_ = path.string();
        file_ = fopen(currentPath_.c_str(), "wb");
        currentBytes_ = 0;
        if(!file_)
            return false;
        enforceTotalCap();
        return true;
    }

    void DecisionLog::enforceTotalCap()
    {
        std::vector<Entry> logs;
        listLogs(fs::path(directory_), logs);
        uint64_t total = 0;
        for(size_t i = 0; i < logs.size(); ++i)
            total += logs[i].size;
        // the newest file is the one just opened; never delete it
        for(size_t i = 0; i + 1 < logs.size() && total > totalCap_; ++i)
        {
            if(logs[i].path.string() == currentPath_)
                continue;
            std::error_code ec;
            if(fs::remove(logs[i].path, ec))
                total -= logs[i].size;
        }
    }

    void DecisionLog::writeRecord(uint16_t type, const void* payload, size_t bytes)
    {
        const void* parts[1] = { payload };
        size_t sizes[1] = { bytes };
        writeRecord(type, parts, sizes, 1);
    }

    void DecisionLog::writeRecord(uint16_t type, const void* const* parts, const size_t* sizes, size_t count)
    {
        if(!file_)
            return;
        size_t payload = 0;
        for(size_t i = 0; i < count; ++i)
            payload += sizes[i];

        TacnnRecordHeader header;
        header.magic = TACNN_MAGIC_RECORD;
        header.record_type = type;
        header.schema_version = TACNN_SCHEMA_VERSION;
        header.payload_bytes = static_cast<uint32_t>(payload);
        header.schema_hash = TACNN_SCHEMA_HASH;

        std::vector<uint8_t> buffer(sizeof header + payload);
        memcpy(&buffer[0], &header, sizeof header);
        size_t at = sizeof header;
        for(size_t i = 0; i < count; ++i)
        {
            if(sizes[i])
                memcpy(&buffer[at], parts[i], sizes[i]);
            at += sizes[i];
        }
        if(fwrite(&buffer[0], 1, buffer.size(), file_) != buffer.size())
        {
            // the disk is gone or full: stop logging rather than crash the game
            fclose(file_);
            file_ = 0;
            return;
        }
        currentBytes_ += buffer.size();
        totalWritten_ += buffer.size();
        ++records_;
        if(currentBytes_ >= fileCap_)
            openNewFile();
    }

    void DecisionLog::writeBattleStart(const TacnnBattleStart& start)
    {
        writeRecord(TACNN_REC_BATTLE_START, &start, sizeof start);
    }

    void DecisionLog::writeDecision(const TacnnDecision& head, const TacnnObservation& obs, const TacnnActionMask& mask,
                                    const TacnnCandidateSet& cand, const TacnnPolicyAction& action)
    {
        const void* parts[5] = { &head, &obs, &mask, &cand, &action };
        size_t sizes[5] = { sizeof head, sizeof obs, sizeof mask, sizeof cand, sizeof action };
        writeRecord(TACNN_REC_DECISION, parts, sizes, 5);
    }

    void DecisionLog::writeOutcome(const TacnnOutcome& outcome)
    {
        writeRecord(TACNN_REC_OUTCOME, &outcome, sizeof outcome);
    }

    void DecisionLog::writeEmbedded(const TacnnEmbedded& embedded)
    {
        writeRecord(TACNN_REC_EMBEDDED, &embedded, sizeof embedded);
    }

    void DecisionLog::writeEpisodeEnd(const TacnnEpisodeEnd& end)
    {
        writeRecord(TACNN_REC_EPISODE_END, &end, sizeof end);
        flush();
    }

    void DecisionLog::flush()
    {
        if(file_)
            fflush(file_);
    }

    DecisionLog& GetDecisionLog()
    {
        static DecisionLog log;
        return log;
    }
}
