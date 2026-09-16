/**
 * @file
 * @author ja2mod
 *
 * Per-observer memory of opponents. Added by the ja2mod fork on 2026-09-15;
 * see CHANGES-ja2mod.md.
 *
 * The engine remembers, for every pair of soldiers, only where the observer
 * last knew the opponent to be (gsLastKnownOppLoc) and how fresh that
 * knowledge is (gbOppList). The belief store runs parallel to those arrays
 * and keeps what the observer could also have noticed at the time: the
 * opponent's stance, the class and reach of his weapon, how hurt he looked,
 * and the fire the two have exchanged. It is written from the same places
 * the engine writes its own arrays (a sighting in ManSeesMan, a noise in
 * HearNoise, a hit in SoldierTakeDamage) and read only when the observation
 * for the observer is built, so nothing in it can be known before the engine
 * says the observer knows it.
 *
 * The store has no engine dependencies; it is plain data with a serializer,
 * saved after the opponent list in a savegame (SAVE_GAME_VERSION 187).
 */

#ifndef TACNN_BELIEF_STORE_H
#define TACNN_BELIEF_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace tacnn
{
    struct BeliefRecord
    {
        uint32_t seenTurn;          ///< battle turn of the last sighting, 0 never
        uint32_t seenSerial;        ///< event counter at that sighting, orders events within a turn
        int32_t seenGridno;
        int8_t seenLevel;
        uint8_t seenStance;         ///< Stance
        uint8_t seenGunClass;       ///< GunClass
        uint8_t seenGunRange;       ///< tiles
        uint8_t seenLifeBand;       ///< 0 unknown, 1 dying .. 4 unhurt
        uint8_t attacksOnMe;        ///< saturating
        uint8_t damageToMe;         ///< life points, saturating
        uint8_t damageByMe;         ///< life points, saturating
        uint32_t heardTurn;         ///< 0 never
        int32_t heardGridno;
        int8_t heardLevel;
        uint8_t heardVolume;
        uint8_t heardType;
        uint8_t reserved;
    };

    class BeliefStore
    {
        public:
            enum { CAPACITY = 256 };

            BeliefStore();

            /// Forget everything; a new battle starts at turn 1.
            void reset();
            /// The player's team began a turn: one battle turn has passed.
            void beginTurn();
            uint32_t turn() const { return turn_; }
            uint32_t serial() const { return serial_; }

            void recordSighting(uint8_t observer, uint8_t opponent, int32_t gridno, int8_t level,
                                uint8_t stance, uint8_t gunClass, uint8_t gunRange, uint8_t lifeBand);
            void recordHearing(uint8_t observer, uint8_t opponent, int32_t gridno, int8_t level,
                               uint8_t volume, uint8_t type);
            /// `attacker` hurt `victim` for `damage` life points (0 for a miss the victim noticed).
            void recordAttack(uint8_t attacker, uint8_t victim, int damage);
            /// A soldier slot was freed or reused: nothing is known by or about it any more.
            void forgetSoldier(uint8_t id);

            const BeliefRecord& get(uint8_t observer, uint8_t opponent) const;

            /// Number of records with anything in them.
            size_t populated() const;

            /// Flat little-endian blob, self-describing; empty store is a small header.
            void serialize(std::vector<uint8_t>& out) const;
            /// Replaces the contents; false and an empty store if the blob is malformed.
            bool deserialize(const uint8_t* data, size_t size);

        private:
            BeliefRecord& at(uint8_t observer, uint8_t opponent);
            std::vector<BeliefRecord> records_;
            uint32_t turn_;
            uint32_t serial_;
    };

    /// The one store of the running game.
    BeliefStore& GetBeliefStore();
}

#endif
