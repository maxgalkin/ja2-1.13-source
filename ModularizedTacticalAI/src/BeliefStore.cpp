/**
 * @file
 * @author ja2mod
 *
 * Added by the ja2mod fork on 2026-09-15; see CHANGES-ja2mod.md.
 */

#include "../include/BeliefStore.h"

#include <string.h>

namespace tacnn
{
    namespace
    {
        const uint32_t BLOB_MAGIC = 0x464C4254u;    // "TBLF"
        const uint32_t BLOB_VERSION = 1;

        template<typename T>
        void put(std::vector<uint8_t>& out, T value)
        {
            for(size_t i = 0; i < sizeof(T); ++i)
                out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF));
        }

        template<typename T>
        bool take(const uint8_t*& p, const uint8_t* end, T& value)
        {
            if(static_cast<size_t>(end - p) < sizeof(T))
                return false;
            uint64_t v = 0;
            for(size_t i = 0; i < sizeof(T); ++i)
                v |= static_cast<uint64_t>(p[i]) << (8 * i);
            value = static_cast<T>(v);
            p += sizeof(T);
            return true;
        }

        uint8_t saturate(int value)
        {
            if(value < 0) return 0;
            if(value > 255) return 255;
            return static_cast<uint8_t>(value);
        }

        bool empty(const BeliefRecord& r)
        {
            return r.seenTurn == 0 && r.heardTurn == 0 && r.attacksOnMe == 0
                && r.damageToMe == 0 && r.damageByMe == 0;
        }

        void putRecord(std::vector<uint8_t>& out, const BeliefRecord& r)
        {
            put(out, r.seenTurn);
            put(out, r.seenSerial);
            put(out, r.seenGridno);
            put(out, r.seenLevel);
            put(out, r.seenStance);
            put(out, r.seenGunClass);
            put(out, r.seenGunRange);
            put(out, r.seenLifeBand);
            put(out, r.attacksOnMe);
            put(out, r.damageToMe);
            put(out, r.damageByMe);
            put(out, r.heardTurn);
            put(out, r.heardGridno);
            put(out, r.heardLevel);
            put(out, r.heardVolume);
            put(out, r.heardType);
            put(out, r.reserved);
        }

        bool takeRecord(const uint8_t*& p, const uint8_t* end, BeliefRecord& r)
        {
            return take(p, end, r.seenTurn) && take(p, end, r.seenSerial)
                && take(p, end, r.seenGridno) && take(p, end, r.seenLevel)
                && take(p, end, r.seenStance) && take(p, end, r.seenGunClass)
                && take(p, end, r.seenGunRange) && take(p, end, r.seenLifeBand)
                && take(p, end, r.attacksOnMe) && take(p, end, r.damageToMe)
                && take(p, end, r.damageByMe) && take(p, end, r.heardTurn)
                && take(p, end, r.heardGridno) && take(p, end, r.heardLevel)
                && take(p, end, r.heardVolume) && take(p, end, r.heardType)
                && take(p, end, r.reserved);
        }
    }

    BeliefStore::BeliefStore()
        : records_(static_cast<size_t>(CAPACITY) * CAPACITY), turn_(1), serial_(0)
    {
        reset();
    }

    void BeliefStore::reset()
    {
        memset(&records_[0], 0, records_.size() * sizeof(BeliefRecord));
        for(size_t i = 0; i < records_.size(); ++i)
        {
            records_[i].seenGridno = -1;
            records_[i].heardGridno = -1;
        }
        turn_ = 1;
        serial_ = 0;
    }

    void BeliefStore::beginTurn()
    {
        ++turn_;
    }

    BeliefRecord& BeliefStore::at(uint8_t observer, uint8_t opponent)
    {
        return records_[static_cast<size_t>(observer) * CAPACITY + opponent];
    }

    const BeliefRecord& BeliefStore::get(uint8_t observer, uint8_t opponent) const
    {
        return records_[static_cast<size_t>(observer) * CAPACITY + opponent];
    }

    void BeliefStore::recordSighting(uint8_t observer, uint8_t opponent, int32_t gridno, int8_t level,
                                     uint8_t stance, uint8_t gunClass, uint8_t gunRange, uint8_t lifeBand)
    {
        BeliefRecord& r = at(observer, opponent);
        r.seenTurn = turn_;
        r.seenSerial = ++serial_;
        r.seenGridno = gridno;
        r.seenLevel = level;
        r.seenStance = stance;
        r.seenGunClass = gunClass;
        r.seenGunRange = gunRange;
        r.seenLifeBand = lifeBand;
    }

    void BeliefStore::recordHearing(uint8_t observer, uint8_t opponent, int32_t gridno, int8_t level,
                                    uint8_t volume, uint8_t type)
    {
        BeliefRecord& r = at(observer, opponent);
        r.heardTurn = turn_;
        r.heardGridno = gridno;
        r.heardLevel = level;
        r.heardVolume = volume;
        r.heardType = type;
        ++serial_;
    }

    void BeliefStore::recordAttack(uint8_t attacker, uint8_t victim, int damage)
    {
        if(attacker == victim)
            return;
        BeliefRecord& mine = at(victim, attacker);
        mine.attacksOnMe = saturate(mine.attacksOnMe + 1);
        mine.damageToMe = saturate(mine.damageToMe + (damage > 0 ? damage : 0));
        BeliefRecord& theirs = at(attacker, victim);
        theirs.damageByMe = saturate(theirs.damageByMe + (damage > 0 ? damage : 0));
        ++serial_;
    }

    void BeliefStore::forgetSoldier(uint8_t id)
    {
        for(int other = 0; other < CAPACITY; ++other)
        {
            BeliefRecord& row = at(id, static_cast<uint8_t>(other));
            memset(&row, 0, sizeof row);
            row.seenGridno = -1;
            row.heardGridno = -1;
            BeliefRecord& col = at(static_cast<uint8_t>(other), id);
            memset(&col, 0, sizeof col);
            col.seenGridno = -1;
            col.heardGridno = -1;
        }
    }

    size_t BeliefStore::populated() const
    {
        size_t n = 0;
        for(size_t i = 0; i < records_.size(); ++i)
            if(!empty(records_[i]))
                ++n;
        return n;
    }

    void BeliefStore::serialize(std::vector<uint8_t>& out) const
    {
        out.clear();
        put(out, BLOB_MAGIC);
        put(out, BLOB_VERSION);
        put(out, turn_);
        put(out, serial_);
        put(out, static_cast<uint32_t>(populated()));
        for(int observer = 0; observer < CAPACITY; ++observer)
        {
            for(int opponent = 0; opponent < CAPACITY; ++opponent)
            {
                const BeliefRecord& r = get(static_cast<uint8_t>(observer), static_cast<uint8_t>(opponent));
                if(empty(r))
                    continue;
                put(out, static_cast<uint8_t>(observer));
                put(out, static_cast<uint8_t>(opponent));
                putRecord(out, r);
            }
        }
    }

    bool BeliefStore::deserialize(const uint8_t* data, size_t size)
    {
        reset();
        const uint8_t* p = data;
        const uint8_t* end = data + size;
        uint32_t magic = 0, version = 0, turn = 0, serial = 0, count = 0;
        if(!take(p, end, magic) || magic != BLOB_MAGIC)
            return false;
        if(!take(p, end, version) || version != BLOB_VERSION)
            return false;
        if(!take(p, end, turn) || !take(p, end, serial) || !take(p, end, count))
            return false;
        for(uint32_t i = 0; i < count; ++i)
        {
            uint8_t observer = 0, opponent = 0;
            BeliefRecord r;
            if(!take(p, end, observer) || !take(p, end, opponent) || !takeRecord(p, end, r))
            {
                reset();
                return false;
            }
            at(observer, opponent) = r;
        }
        turn_ = turn == 0 ? 1 : turn;
        serial_ = serial;
        return true;
    }

    BeliefStore& GetBeliefStore()
    {
        static BeliefStore store;
        return store;
    }
}
