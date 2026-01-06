#include "core/Iattackable.hpp"
#include "core/gameworld.hpp"
#include "core/render_config.hpp"
#include <chrono>
#include <algorithm>
#include <SFML/Graphics.hpp>
#include <optional>
#include <iterator>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cctype>
#include <limits>


namespace {
    constexpr int MAP_WIDTH = 30;
    constexpr int MAP_HEIGHT = 30;
    constexpr float kHeatAlpha = 0.35f;
    constexpr float kHeatOcc = 1.0f;
    constexpr float kHeatTarget = 0.6f;
    constexpr float kHeatNeighborScale = 0.5f;

    // 在一个矩形区域内寻找第一个可通行格子
    bool findPassableInRegion(const Map& map,
                              int x0, int y0, int x1, int y1,
                              Coord& out)
    {
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(map.getWidth()  - 1, x1);
        y1 = std::min(map.getHeight() - 1, y1);

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                Coord c{x, y};
                if (map.getTile(c).isPassable()) {
                    out = c;
                    return true;
                }
            }
        }
        return false;
    }
    bool pickBasePositions(const Map& map, Coord& baseA, Coord& baseB)
    {
        const int w = map.getWidth();
        const int h = map.getHeight();

        // 左上角附近
        Coord a;
        if (!findPassableInRegion(map,
                                  1, 1,
                                  std::min(5, w-2), std::min(5, h-2),
                                  a)) {
            return false;
        }

        // 右下角附近
        Coord b;
        if (!findPassableInRegion(map,
                                  std::max(w-6, 1), std::max(h-6, 1),
                                  w-2, h-2,
                                  b)) {
            return false;
        }

        if (!map.isReachable(a, b)) {
            return false;
        }

        baseA = a;
        baseB = b;
        return true;
    }

    AttackableKey attackableKey(const std::shared_ptr<IAttackable>& target) {
        AttackableKey key;
        if (!target) return key;
        key.type = target->getAttackType();
        if (key.type == AttackableType::BASE) {
            auto basePtr = std::dynamic_pointer_cast<Base>(target);
            key.id = basePtr ? basePtr->getId() : -1;
        } else {
            auto unitPtr = std::dynamic_pointer_cast<Unit>(target);
            key.id = unitPtr ? unitPtr->getId() : -1;
        }
        return key;
    }

    using UnitSnapshot = AiScoring::UnitSnapshot;
    using AttackableSnapshot = AiScoring::AttackableSnapshot;
    using TargetScoreParams = AiScoring::TargetScoreParams;
    using ActionParams = AiScoring::ActionParams;
    using RetreatParams = AiScoring::RetreatParams;
    using OocParams = AiScoring::OocParams;
    using ReachableGrid = AiScoring::ReachableGrid;
    using TargetChoice = AiScoring::TargetChoice;

    const TargetScoreParams& kTargetParams = AiScoring::kTargetParams;
    const ActionParams& kActionParams = AiScoring::kActionParams;
    const RetreatParams& kRetreatParams = AiScoring::kRetreatParams;
    const OocParams& kOocParams = AiScoring::kOocParams;
    constexpr float kThreatRadius = AiScoring::kThreatRadius;

    float unitPreference(UnitType self, UnitType target) {
        static const float prefs[3][3] = {
            {1.5f, 1.0f, 1.2f},  // Infantry vs {Infantry, Archer, Knight}
            {1.0f, 1.5f, 1.2f},  // Archer
            {1.2f, 0.9f, 1.3f}   // Knight
        };
        return prefs[static_cast<int>(self)][static_cast<int>(target)];
    }

    float basePreference(UnitType self) {
        switch (self) {
            case UnitType::Infantry: return 0.9f;
            case UnitType::Archer:   return 0.7f;
            case UnitType::Knight:   return 1.1f;
        }
        return 1.0f;
    }

    float threatScore(const AttackableSnapshot& target) {
        if (target.isBase) return 0.f;
        return target.attack * (0.6f + 0.12f * target.attackRange);
    }

    float distanceScore(float dist) {
        return 1.0f / (1.0f + dist);
    }

    float ttkScore(const UnitSnapshot& self, const AttackableSnapshot& target) {
        float dps = std::max(1.0f, self.stats.attack);
        float ttk = target.hp / dps;
        return 1.0f / (1.0f + ttk);
    }

    bool containsKey(const std::vector<const AttackableSnapshot*>& visible,
                     const AttackableKey& key) {
        for (const auto* v : visible) {
            if (v && v->key == key) return true;
        }
        return false;
    }

    std::vector<const AttackableSnapshot*> collectVisibleEnemies(
        const UnitSnapshot& unit,
        const Map& map,
        const std::vector<AttackableSnapshot>& enemies,
        const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
        const std::vector<AttackableKey>& forcedKeys)
    {
        std::vector<const AttackableSnapshot*> visible;
        if (!map.inBounds(unit.pos)) return visible;

        const Tile& tile = map.getTile(unit.pos);
        float effVision = unit.stats.visionRange + tile.getVisionBonus();

        for (const auto& e : enemies) {
            if (!map.inBounds(e.pos)) continue;
            float d = unit.pos.mhtDistanceTo(e.pos);
            if (d <= effVision) {
                visible.push_back(&e);
            }
        }

        for (const auto& key : forcedKeys) {
            auto it = enemyIndex.find(key);
            if (it == enemyIndex.end()) continue;
            const auto* target = &enemies[it->second];
            if (!containsKey(visible, target->key)) {
                visible.push_back(target);
            }
        }

        return visible;
    }

    float computeLocalThreat(const UnitSnapshot& self,
                             const std::vector<const AttackableSnapshot*>& enemies,
                             float radius) {
        float threat = 0.f;
        for (const auto* e : enemies) {
            if (!e || e->isBase) continue;
            float dist = self.pos.mhtDistanceTo(e->pos);
            if (dist <= radius) {
                threat += threatScore(*e);
            }
        }
        return threat;
    }

    int countNearbyAllies(const std::vector<UnitSnapshot>& allies,
                          const Coord& pos,
                          int radius) {
        int count = 0;
        for (const auto& ally : allies) {
            if (ally.hp <= 0.f) continue;
            if (ally.pos.mhtDistanceTo(pos) <= radius) {
                ++count;
            }
        }
        return count;
    }

    int countNearbyEnemies(const std::vector<const AttackableSnapshot*>& enemies,
                           const Coord& pos,
                           int radius) {
        int count = 0;
        for (const auto* e : enemies) {
            if (!e || e->isBase) continue;
            if (e->pos.mhtDistanceTo(pos) <= radius) {
                ++count;
            }
        }
        return count;
    }

    float baseOpportunityScore(const UnitSnapshot& self,
                               const AttackableSnapshot& base,
                               const std::vector<UnitSnapshot>& allies,
                               const std::vector<const AttackableSnapshot*>& enemies,
                               const TargetScoreParams& params,
                               float localThreat) {
        int allyNear = countNearbyAllies(allies, base.pos, 6);
        int enemyNear = countNearbyEnemies(enemies, base.pos, 6);
        float advantage = (allyNear + 1.0f) / (enemyNear + 1.0f);
        float hpFrac = (base.maxHp > 0.f) ? (base.hp / base.maxHp) : 1.0f;
        float finishBonus = 1.0f + (1.0f - hpFrac) * params.baseFinishBonus;
        float threatPenalty = (localThreat >= params.threatRetreat) ? 0.6f : 1.0f;
        return advantage * finishBonus * threatPenalty;
    }

    std::uint64_t packCoord(const Coord& c);

    ReachableGrid buildReachableGrid(const Map& map, const Coord& start) {
        ReachableGrid grid;
        grid.width = map.getWidth();
        grid.height = map.getHeight();
        grid.reachable.assign(static_cast<std::size_t>(grid.width * grid.height), 0);

        if (!map.inBounds(start)) return grid;
        if (!map.getTile(start).isPassable()) return grid;

        std::queue<Coord> q;
        std::vector<Coord> nbrs;
        q.push(start);
        grid.reachable[static_cast<std::size_t>(start.y) * grid.width + start.x] = 1;

        while (!q.empty()) {
            Coord cur = q.front();
            q.pop();
            map.getNeighbors(cur, nbrs);
            for (const auto& n : nbrs) {
                if (!map.inBounds(n)) continue;
                if (!map.getTile(n).isPassable()) continue;
                std::size_t idx = static_cast<std::size_t>(n.y) * grid.width + n.x;
                if (grid.reachable[idx]) continue;
                grid.reachable[idx] = 1;
                q.push(n);
            }
        }

        return grid;
    }

    float firingPositionAvailability(const UnitSnapshot& self,
                                     const AttackableSnapshot& target,
                                     const Map& map,
                                     const ReachableGrid& reachable,
                                     const std::unordered_set<std::uint64_t>& occ,
                                     int cap) {
        int range = static_cast<int>(std::floor(self.stats.attackRange + 0.001f));
        if (range <= 0) return 0.f;

        int count = 0;
        int minX = target.pos.x - range;
        int maxX = target.pos.x + range;
        int minY = target.pos.y - range;
        int maxY = target.pos.y + range;

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                Coord cand{x, y};
                if (!map.inBounds(cand)) continue;
                if (std::abs(cand.x - target.pos.x) + std::abs(cand.y - target.pos.y) > range) {
                    continue;
                }
                if (!map.getTile(cand).isPassable()) continue;
                if (!reachable.isReachable(cand)) continue;
                if (map.hasMountainBetween(cand, target.pos)) continue;
                auto key = packCoord(cand);
                if (cand != self.pos && occ.find(key) != occ.end()) continue;
                ++count;
                if (count >= cap) {
                    return 1.0f;
                }
            }
        }
        if (cap <= 0) return 0.f;
        return static_cast<float>(count) / static_cast<float>(cap);
    }

    float scoreTargetCandidate(
        const UnitSnapshot& self,
        const AttackableSnapshot& target,
        const std::vector<UnitSnapshot>& allies,
        const std::vector<const AttackableSnapshot*>& visible,
        const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
        const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
        const TargetScoreParams& params,
        const Map& map,
        const ReachableGrid& reachable,
        const std::unordered_set<std::uint64_t>& occ,
        float localThreat,
        float* baseOppOut) {
        float dist = self.pos.mhtDistanceTo(target.pos);
        float score = 0.f;
        score += params.distanceWeight * distanceScore(dist);
        score += params.threatWeight * threatScore(target);
        score += params.ttkWeight * ttkScore(self, target);
        float hpFrac = (target.maxHp > 0.f) ? (target.hp / target.maxHp) : 1.0f;
        score += params.executeWeight * (1.0f - hpFrac);

        if (map.hasMountainBetween(self.pos, target.pos)) {
            score -= params.losPenalty;
        }
        float availability = firingPositionAvailability(self, target, map, reachable,
                                                         occ, params.availabilityCap);
        score += params.availabilityWeight * availability;
        if (availability <= 0.f) {
            score -= params.unreachablePenalty;
        }

        float baseOpportunity = 0.f;
        if (target.isBase) {
            baseOpportunity = baseOpportunityScore(self, target, allies, visible,
                                                   params, localThreat);
            score += params.baseWeight * basePreference(self.type) * baseOpportunity;
            score -= params.baseThreatPenalty * (localThreat / params.threatRetreat);
        } else {
            score += params.preferenceWeight * unitPreference(self.type, target.unitType);
        }

        auto incIt = incomingDamage.find(target.key);
        if (incIt != incomingDamage.end() && target.maxHp > 0.f) {
            score -= params.overkillWeight * (incIt->second / target.maxHp);
        }
        auto lockIt = lockedCounts.find(target.key);
        if (lockIt != lockedCounts.end()) {
            score -= params.lockWeight * static_cast<float>(lockIt->second);
        }

        if (target.key == self.commitTarget) {
            score += params.commitBonus;
        }
        if (target.key == self.currentTarget) {
            score += params.commitBonus * 0.5f;
        }

        if (baseOppOut) {
            *baseOppOut = baseOpportunity;
        }
        return score;
    }

    TargetChoice chooseTarget(const UnitSnapshot& self,
                              const std::vector<const AttackableSnapshot*>& visible,
                              const std::vector<UnitSnapshot>& allies,
                              const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                              const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                              const TargetScoreParams& params,
                              const Map& map,
                              const ReachableGrid& reachable,
                              const std::unordered_set<std::uint64_t>& occ) {
        TargetChoice best;
        if (visible.empty()) return best;

        float localThreat = computeLocalThreat(self, visible, kThreatRadius);
        float hpFrac = (self.maxHp > 0.f) ? (self.hp / self.maxHp) : 1.0f;
        if (self.commitTimer > 0.f &&
            hpFrac > params.retreatHpFrac &&
            localThreat < params.threatRetreat) {
            for (const auto* v : visible) {
                if (v && v->key == self.commitTarget) {
                    best.target = v;
                    best.score = 0.f;
                    if (v->isBase) {
                        best.baseOpportunity = baseOpportunityScore(self, *v, allies, visible,
                                                                   params, localThreat);
                    }
                    return best;
                }
            }
        }

        for (const auto* v : visible) {
            if (!v) continue;
            float baseOpp = 0.f;
            float score = scoreTargetCandidate(self, *v, allies, visible,
                                               incomingDamage, lockedCounts,
                                               params, map, reachable, occ,
                                               localThreat, &baseOpp);

            bool better = false;
            if (!best.target) {
                better = true;
            } else if (score > best.score + 1e-4f) {
                better = true;
            } else if (std::abs(score - best.score) <= 1e-4f) {
                if (v->key.type < best.target->key.type ||
                    (v->key.type == best.target->key.type && v->key.id < best.target->key.id)) {
                    better = true;
                }
            }

            if (better) {
                best.target = v;
                best.score = score;
                best.baseOpportunity = baseOpp;
            }
        }

        return best;
    }

    bool inAttackRange(const UnitSnapshot& self,
                       const Map& map,
                       const AttackableSnapshot& target) {
        const Tile& tile = map.getTile(self.pos);
        float effectiveRange = self.stats.attackRange + tile.getVisionBonus();
        float dist = self.pos.mhtDistanceTo(target.pos);
        if (dist > effectiveRange) return false;
        if (map.hasMountainBetween(self.pos, target.pos)) return false;
        if (self.stats.attackRange <= 2.0f &&
            map.hasRiverBetween(self.pos, target.pos)) {
            return false;
        }
        return true;
    }

    bool inAttackRangeFrom(const UnitSnapshot& self,
                           const Map& map,
                           const AttackableSnapshot& target,
                           const Coord& from) {
        if (!map.inBounds(from)) return false;
        const Tile& tile = map.getTile(from);
        float effectiveRange = self.stats.attackRange + tile.getVisionBonus();
        float dist = from.mhtDistanceTo(target.pos);
        if (dist > effectiveRange) return false;
        if (map.hasMountainBetween(from, target.pos)) return false;
        if (self.stats.attackRange <= 2.0f &&
            map.hasRiverBetween(from, target.pos)) {
            return false;
        }
        return true;
    }

    float computeAttackDamage(const UnitSnapshot& self, const Map& map) {
        const Tile& tile = map.getTile(self.pos);
        float dmg = self.stats.attack + tile.getAttackBonus();
        if (tile.getType() == TileType::HILL) {
            dmg *= 0.85f;
        }
        return dmg;
    }

    float computeAttackCooldown(const UnitSnapshot& self, const Map& map) {
        const Tile& tile = map.getTile(self.pos);
        float cdBase = 0.8f;
        if (tile.getType() == TileType::HILL) {
            cdBase *= 1.25f;
        }
        return cdBase;
    }

    CombatAction decideCombatAction(const UnitSnapshot& self,
                                    const AttackableSnapshot* target,
                                    float dist,
                                    bool inRange,
                                    float cdAfter,
                                    float localThreat,
                                    float baseOpportunity,
                                    const ActionParams& params) {
        if (!target) return CombatAction::None;

        (void)localThreat;
        float hpFrac = (self.maxHp > 0.f) ? (self.hp / self.maxHp) : 1.0f;
        if (hpFrac <= params.retreatHpFrac) {
            return CombatAction::Retreat;
        }
        if (target->isBase && baseOpportunity >= params.siegeOpportunity) {
            return CombatAction::Siege;
        }

        bool ranged = self.stats.attackRange > 2.5f;
        if (inRange) {
            if (ranged && cdAfter > 0.f &&
                dist <= (self.stats.attackRange - params.kiteMargin)) {
                return CombatAction::Kite;
            }
            return CombatAction::Attack;
        }
        return CombatAction::Chase;
    }

    AttackIntent planAttackIntent(const UnitSnapshot& unit,
                                  float dt,
                                  const Map& map,
                                  const std::vector<AttackableSnapshot>& enemies,
                                  const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                                  const std::vector<AttackableKey>& forcedKeys,
                                  const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                                  const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                                  const std::vector<UnitSnapshot>& allies,
                                  const std::unordered_set<std::uint64_t>& occ) {
        AttackIntent intent;
        intent.attackerId = unit.id;
        intent.nextCooldown = unit.cooldown;
        intent.nextTargetId = unit.currentTarget.id;
        intent.nextTargetType = unit.currentTarget.type;
        intent.nextCommitTimer = std::max(0.0f, unit.commitTimer - dt);
        intent.nextCommitTargetId = unit.commitTarget.id;
        intent.nextCommitTargetType = unit.commitTarget.type;

        if (unit.hp <= 0.f) return intent;

        float cdAfter = std::max(0.0f, unit.cooldown - dt);

        std::vector<const AttackableSnapshot*> visible =
            collectVisibleEnemies(unit, map, enemies, enemyIndex, forcedKeys);
        ReachableGrid reachable = buildReachableGrid(map, unit.pos);

        TargetChoice choice = chooseTarget(unit, visible, allies,
                                           incomingDamage, lockedCounts,
                                           kTargetParams, map, reachable,
                                           occ);
        const AttackableSnapshot* target = choice.target;
        bool commandOverride = false;
        if (unit.commandAttackActive && unit.commandTarget.id >= 0) {
            auto it = enemyIndex.find(unit.commandTarget);
            if (it != enemyIndex.end()) {
                target = &enemies[it->second];
                commandOverride = true;
            }
        }
        if (!commandOverride && unit.currentTarget.id >= 0) {
            auto curIt = enemyIndex.find(unit.currentTarget);
            if (curIt != enemyIndex.end()) {
                const AttackableSnapshot* curTarget = &enemies[curIt->second];
                if (curTarget && inAttackRange(unit, map, *curTarget)) {
                    float localThreat = computeLocalThreat(unit, visible, kThreatRadius);
                    float baseOpp = 0.f;
                    float curScore = scoreTargetCandidate(unit, *curTarget, allies, visible,
                                                          incomingDamage, lockedCounts,
                                                          kTargetParams, map, reachable, occ,
                                                          localThreat, &baseOpp);
                    if (!target || (choice.score - curScore) <= kTargetParams.commitBonus) {
                        target = curTarget;
                        choice.baseOpportunity = baseOpp;
                        choice.score = curScore;
                    }
                }
            }
        }
        if (!target) {
            intent.nextCooldown = cdAfter;
            intent.nextTargetId = -1;
            intent.nextCommitTimer = 0.f;
            intent.nextCommitTargetId = -1;
            intent.nextCommitTargetType = AttackableType::UNIT;
            intent.action = CombatAction::None;
            return intent;
        }

        intent.nextTargetId = target->key.id;
        intent.nextTargetType = target->key.type;
        intent.nextCommitTargetId = target->key.id;
        intent.nextCommitTargetType = target->key.type;

        float dist = unit.pos.mhtDistanceTo(target->pos);
        bool inRange = inAttackRange(unit, map, *target);
        float localThreat = computeLocalThreat(unit, visible, kThreatRadius);
        intent.action = decideCombatAction(unit, target, dist, inRange, cdAfter,
                                           localThreat, choice.baseOpportunity,
                                           kActionParams);
        float hpRatio = (unit.maxHp > 0.f) ? (unit.hp / unit.maxHp) : 1.0f;
        bool enterRetreat = (hpRatio <= kRetreatParams.hpEnter);
        bool exitRetreat = (hpRatio >= kRetreatParams.hpExit &&
                            unit.retreatTimer <= 0.f);
        bool retreatNow = unit.retreating ? !exitRetreat : enterRetreat;
        if (retreatNow) {
            intent.action = CombatAction::Retreat;
        }
        bool commitSame = (target->key == unit.commitTarget && unit.commitTimer > 0.f);
        float commitDuration = (intent.action == CombatAction::Siege)
            ? kTargetParams.siegeHoldTime
            : kTargetParams.commitDuration;
        intent.nextCommitTimer = commitSame ? std::max(0.0f, unit.commitTimer - dt)
                                            : commitDuration;

        bool allowRetreatFire = (intent.action != CombatAction::Retreat ||
                                 localThreat < kRetreatParams.retreatFireThreat);
        if (inRange && cdAfter <= 0.f && allowRetreatFire) {
            intent.didAttack = true;
            intent.targetId = target->key.id;
            intent.targetType = target->key.type;
            intent.damage = computeAttackDamage(unit, map);
            intent.nextCooldown = computeAttackCooldown(unit, map);
            return intent;
        }

        intent.nextCooldown = cdAfter;
        return intent;
    }

    std::uint64_t packCoord(const Coord& c) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y)));
    }

    Coord unpackCoord(std::uint64_t v) {
        int x = static_cast<int>(static_cast<std::uint32_t>(v >> 32));
        int y = static_cast<int>(static_cast<std::uint32_t>(v & 0xffffffffu));
        return Coord{x, y};
    }

    std::uint32_t stableHash(int unitId, const Coord& c) {
        std::uint32_t h = static_cast<std::uint32_t>(unitId) * 2654435761u;
        h ^= static_cast<std::uint32_t>(c.x) * 2246822519u;
        h ^= static_cast<std::uint32_t>(c.y) * 3266489917u;
        return h;
    }

    float movementSpend(const UnitSnapshot& unit, const Tile& tile) {
        float s = unit.stats.moveSpeed / tile.getMoveCost();
        if (unit.type == UnitType::Knight) {
            switch (tile.getType()) {
                case TileType::PLAIN:  return s * 1.1f;
                case TileType::FOREST: return s * 0.6f;
                case TileType::HILL:   return s * 0.7f;
                case TileType::SWAMP:  return s * 0.4f;
                default: return s;
            }
        }
        return s;
    }

    float computeThreatAtCoord(const Coord& pos,
                               const std::vector<AttackableSnapshot>& enemies,
                               float radius);

    float heatAt(const std::vector<float>& heat, int width, const Coord& c) {
        if (width <= 0 || heat.empty()) return 0.f;
        std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
        if (idx >= heat.size()) return 0.f;
        return heat[idx];
    }

    void addHeat(std::vector<float>& heat, int width, int height,
                 const Coord& c, float value) {
        if (c.x < 0 || c.y < 0 || c.x >= width || c.y >= height) return;
        std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
        heat[idx] += value;
    }

    std::vector<float> buildHeatMap(const Map& map,
                                    const std::vector<UnitSnapshot>& allies,
                                    const std::unordered_set<std::uint64_t>& occ,
                                    int selfId) {
        const int width = map.getWidth();
        const int height = map.getHeight();
        std::vector<float> heat(static_cast<std::size_t>(width * height), 0.f);
        static const std::array<Coord, 4> neighbors = {
            Coord{1, 0}, Coord{-1, 0}, Coord{0, 1}, Coord{0, -1}
        };

        for (const auto& packed : occ) {
            Coord c = unpackCoord(packed);
            addHeat(heat, width, height, c, kHeatOcc);
            for (const auto& off : neighbors) {
                Coord n{c.x + off.x, c.y + off.y};
                addHeat(heat, width, height, n, kHeatOcc * kHeatNeighborScale);
            }
        }

        for (const auto& ally : allies) {
            if (ally.id == selfId) continue;
            Coord target{};
            bool hasTarget = false;
            if (!ally.movementState.path.empty()) {
                target = ally.movementState.path.back();
                hasTarget = true;
            } else if (ally.movementState.hasLast) {
                target = ally.movementState.lastTarget;
                hasTarget = true;
            }
            if (!hasTarget) continue;
            addHeat(heat, width, height, target, kHeatTarget);
            for (const auto& off : neighbors) {
                Coord n{target.x + off.x, target.y + off.y};
                addHeat(heat, width, height, n, kHeatTarget * kHeatNeighborScale);
            }
        }

        return heat;
    }

    bool findPathAStarHeat(const Map& map,
                           const Coord& start,
                           const Coord& goal,
                           const std::vector<float>& heat,
                           int unitId,
                           std::vector<Coord>& outpath) {
        outpath.clear();
        if (!map.inBounds(start) || !map.inBounds(goal)) return false;
        if (!map.getTile(start).isPassable() || !map.getTile(goal).isPassable()) return false;

        const int width = map.getWidth();
        const int height = map.getHeight();
        struct Node {
            Coord pos;
            float g;
            float f;
            std::uint32_t tie;
        };
        auto cmp = [](const Node& a, const Node& b) {
            if (std::abs(a.f - b.f) > 1e-4f) return a.f > b.f;
            if (std::abs(a.g - b.g) > 1e-4f) return a.g > b.g;
            return a.tie > b.tie;
        };

        std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
        std::vector<std::vector<float>> gScore(
            height, std::vector<float>(width, 1e9f));
        std::vector<std::vector<std::uint32_t>> tieScore(
            height, std::vector<std::uint32_t>(width, std::numeric_limits<std::uint32_t>::max()));
        std::vector<std::vector<Coord>> cameFrom(
            height, std::vector<Coord>(width, Coord(-1, -1)));

        gScore[start.y][start.x] = 0.0f;
        std::uint32_t startTie = stableHash(unitId, start);
        tieScore[start.y][start.x] = startTie;
        open.push({start, 0.0f, static_cast<float>(Coord::mhtDistance(start, goal)), startTie});
        std::vector<Coord> nbrs;

        while (!open.empty()) {
            Node cur = open.top();
            open.pop();
            if (cur.g > gScore[cur.pos.y][cur.pos.x] + 1e-4f) continue;

            if (cur.pos == goal) {
                Coord p = goal;
                while (!(p == start)) {
                    outpath.push_back(p);
                    p = cameFrom[p.y][p.x];
                }
                outpath.push_back(start);
                std::reverse(outpath.begin(), outpath.end());
                return true;
            }

            map.getNeighbors(cur.pos, nbrs);
            for (const auto& n : nbrs) {
                const Tile& t = map.getTile(n);
                if (!t.isPassable()) continue;

                float heatCost = kHeatAlpha * heatAt(heat, width, n);
                float tentativeG = cur.g + t.getMoveCost() + heatCost;
                std::uint32_t tie = stableHash(unitId, n);

                bool better = false;
                float prevG = gScore[n.y][n.x];
                if (tentativeG < prevG - 1e-4f) {
                    better = true;
                } else if (std::abs(tentativeG - prevG) <= 1e-4f && tie < tieScore[n.y][n.x]) {
                    better = true;
                }
                if (better) {
                    gScore[n.y][n.x] = tentativeG;
                    tieScore[n.y][n.x] = tie;
                    cameFrom[n.y][n.x] = cur.pos;
                    float f = tentativeG + static_cast<float>(Coord::mhtDistance(n, goal));
                    open.push({n, tentativeG, f, tie});
                }
            }
        }
        return false;
    }

    void rebuildPathStateHeat(const UnitSnapshot& unit,
                              const Map& map,
                              const Coord& target,
                              const std::vector<float>& heat,
                              IMovementBehavior::MovementState& state) {
        state.path.clear();
        findPathAStarHeat(map, unit.pos, target, heat, unit.id, state.path);
        state.idx = 0;
        state.accumulator = 0.f;
        state.lastTarget = target;
        state.hasLast = true;
    }

    Coord applyAnchorSlot(const Map& map,
                          const std::unordered_set<std::uint64_t>& occ,
                          const Coord& anchor,
                          int unitId,
                          const Coord& selfPos) {
        static const std::array<Coord, 13> slots = {
            Coord{0, 0},
            Coord{1, 0}, Coord{-1, 0}, Coord{0, 1}, Coord{0, -1},
            Coord{1, 1}, Coord{-1, 1}, Coord{1, -1}, Coord{-1, -1},
            Coord{2, 0}, Coord{-2, 0}, Coord{0, 2}, Coord{0, -2}
        };
        const std::size_t start = static_cast<std::size_t>(stableHash(unitId, anchor) % slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& off = slots[(start + i) % slots.size()];
            Coord cand{anchor.x + off.x, anchor.y + off.y};
            if (!map.inBounds(cand)) continue;
            if (!map.getTile(cand).isPassable()) continue;
            if (cand != selfPos && occ.find(packCoord(cand)) != occ.end()) continue;
            return cand;
        }
        return anchor;
    }

    Coord stepAlongManhattan(const Coord& from, const Coord& to, int steps) {
        Coord out = from;
        int dx = to.x - from.x;
        int dy = to.y - from.y;
        int sx = (dx > 0) - (dx < 0);
        int sy = (dy > 0) - (dy < 0);
        int ax = std::abs(dx);
        int ay = std::abs(dy);
        int remaining = steps;
        while (remaining-- > 0 && (ax > 0 || ay > 0)) {
            if (ax >= ay && ax > 0) {
                out.x += sx;
                --ax;
            } else if (ay > 0) {
                out.y += sy;
                --ay;
            } else {
                break;
            }
        }
        return out;
    }

    Coord clampToMap(const Map& map, Coord c) {
        c.x = std::max(0, std::min(c.x, map.getWidth() - 1));
        c.y = std::max(0, std::min(c.y, map.getHeight() - 1));
        return c;
    }

    Coord nearestPassable(const Map& map, Coord c, int radius) {
        c = clampToMap(map, c);
        if (map.inBounds(c) && map.getTile(c).isPassable()) {
            return c;
        }
        for (int r = 1; r <= radius; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    Coord cand{c.x + dx, c.y + dy};
                    if (!map.inBounds(cand)) continue;
                    if (map.getTile(cand).isPassable()) return cand;
                }
            }
        }
        return c;
    }

    Coord nearestReachableToTarget(const Map& map,
                                   const ReachableGrid& reachable,
                                   const Coord& target) {
        Coord best = target;
        int bestDist = 0;
        bool found = false;
        for (int y = 0; y < map.getHeight(); ++y) {
            for (int x = 0; x < map.getWidth(); ++x) {
                Coord cand{x, y};
                if (!map.getTile(cand).isPassable()) continue;
                if (!reachable.isReachable(cand)) continue;
                int dist = cand.mhtDistanceTo(target);
                if (!found || dist < bestDist ||
                    (dist == bestDist &&
                     (cand.y < best.y || (cand.y == best.y && cand.x < best.x)))) {
                    best = cand;
                    bestDist = dist;
                    found = true;
                }
            }
        }
        return found ? best : target;
    }

    void fillMoveCandidates(const UnitSnapshot& unit,
                            const Map& map,
                            const Coord& goal,
                            const std::vector<float>& heat,
                            const std::unordered_set<std::uint64_t>& occ,
                            MoveIntent& intent) {
        intent.candidateCount = 0;
        auto pushCandidate = [&](const Coord& c) {
            for (int i = 0; i < intent.candidateCount; ++i) {
                if (intent.candidates[static_cast<std::size_t>(i)] == c) return;
            }
            if (intent.candidateCount < static_cast<int>(intent.candidates.size())) {
                intent.candidates[static_cast<std::size_t>(intent.candidateCount++)] = c;
            }
        };

        if (intent.hasMove) {
            pushCandidate(intent.to);
        } else {
            pushCandidate(unit.pos);
            return;
        }

        struct Candidate {
            Coord pos;
            float cost;
            std::uint32_t tie;
        };
        std::vector<Candidate> options;
        std::vector<Coord> nbrs;
        map.getNeighbors(unit.pos, nbrs);
        Coord clampedGoal = clampToMap(map, goal);
        const int width = map.getWidth();

        for (const auto& n : nbrs) {
            if (!map.inBounds(n)) continue;
            if (!map.getTile(n).isPassable()) continue;
            if (n != unit.pos && occ.find(packCoord(n)) != occ.end()) continue;
            if (intent.hasMove && n == intent.to) continue;
            float cost = map.getTile(n).getMoveCost();
            cost += static_cast<float>(n.mhtDistanceTo(clampedGoal));
            cost += kHeatAlpha * heatAt(heat, width, n);
            options.push_back({n, cost, stableHash(unit.id, n)});
        }

        std::sort(options.begin(), options.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (std::abs(a.cost - b.cost) > 1e-4f) return a.cost < b.cost;
                      return a.tie < b.tie;
                  });

        if (!options.empty()) {
            pushCandidate(options.front().pos);
        }

        pushCandidate(unit.pos);
    }

    bool selectAttackPosition(const UnitSnapshot& self,
                              const AttackableSnapshot& target,
                              const Map& map,
                              const std::vector<AttackableSnapshot>& enemies,
                              const ReachableGrid& reachable,
                              const std::unordered_set<std::uint64_t>& occ,
                              int minDist,
                              int maxDist,
                              Coord& out) {
        if (minDist > maxDist) return false;
        bool found = false;
        Coord bestPos{};
        int bestMove = 0;
        float bestThreat = 0.f;
        int bestTargetDist = 0;

        for (int dy = -maxDist; dy <= maxDist; ++dy) {
            for (int dx = -maxDist; dx <= maxDist; ++dx) {
                int manhattan = std::abs(dx) + std::abs(dy);
                if (manhattan < minDist || manhattan > maxDist) continue;
                Coord cand{target.pos.x + dx, target.pos.y + dy};
                if (!map.inBounds(cand)) continue;
                if (!map.getTile(cand).isPassable()) continue;
                if (!reachable.isReachable(cand)) continue;
                if (cand != self.pos && occ.find(packCoord(cand)) != occ.end()) continue;
                if (!inAttackRangeFrom(self, map, target, cand)) continue;

                int moveCost = self.pos.mhtDistanceTo(cand);
                float threat = computeThreatAtCoord(cand, enemies, kThreatRadius);
                int distToTarget = cand.mhtDistanceTo(target.pos);

                bool better = false;
                if (!found) {
                    better = true;
                } else if (moveCost != bestMove) {
                    better = moveCost < bestMove;
                } else if (std::abs(threat - bestThreat) > 1e-4f) {
                    better = threat < bestThreat;
                } else if (distToTarget != bestTargetDist) {
                    better = distToTarget < bestTargetDist;
                } else if (cand.y != bestPos.y) {
                    better = cand.y < bestPos.y;
                } else if (cand.x != bestPos.x) {
                    better = cand.x < bestPos.x;
                }

                if (better) {
                    bestPos = cand;
                    bestMove = moveCost;
                    bestThreat = threat;
                    bestTargetDist = distToTarget;
                    found = true;
                }
            }
        }

        if (!found) return false;
        out = bestPos;
        return true;
    }

    float computeThreatAtCoord(const Coord& pos,
                               const std::vector<AttackableSnapshot>& enemies,
                               float radius) {
        float threat = 0.f;
        for (const auto& e : enemies) {
            if (e.isBase) continue;
            if (pos.mhtDistanceTo(e.pos) <= radius) {
                threat += threatScore(e);
            }
        }
        return threat;
    }

    Coord chooseSafeSpot(const UnitSnapshot& self,
                         const Map& map,
                         const std::vector<AttackableSnapshot>& enemies,
                         int sampleRadius) {
        static const std::array<Coord, 12> offsets = {
            Coord{0, -2}, Coord{2, 0}, Coord{0, 2}, Coord{-2, 0},
            Coord{2, 2}, Coord{2, -2}, Coord{-2, 2}, Coord{-2, -2},
            Coord{0, -4}, Coord{4, 0}, Coord{0, 4}, Coord{-4, 0}
        };

        Coord best = self.pos;
        float bestThreat = 1e9f;
        bool found = false;

        for (const auto& off : offsets) {
            Coord cand{self.pos.x + off.x, self.pos.y + off.y};
            if (!map.inBounds(cand)) continue;
            if (std::abs(off.x) > sampleRadius || std::abs(off.y) > sampleRadius) continue;
            if (!map.getTile(cand).isPassable()) continue;
            float threat = computeThreatAtCoord(cand, enemies, kThreatRadius);
            if (!found || threat < bestThreat ||
                (std::abs(threat - bestThreat) <= 1e-4f &&
                 (cand.y < best.y || (cand.y == best.y && cand.x < best.x)))) {
                best = cand;
                bestThreat = threat;
                found = true;
            }
        }

        if (!found) return self.pos;
        return best;
    }

    float computeLocalThreatWorld(const Unit& self,
                                  const std::vector<std::shared_ptr<Unit>>& enemies,
                                  float radius) {
        float threat = 0.f;
        for (const auto& e : enemies) {
            if (!e || !e->isAlive()) continue;
            if (self.getPos().mhtDistanceTo(e->getPos()) <= radius) {
                threat += e->baseStats.attack * (0.6f + 0.12f * e->baseStats.attackRange);
            }
        }
        return threat;
    }

    void rebuildPathState(const UnitSnapshot& unit,
                          const Map& map,
                          const Coord& target,
                          IMovementBehavior::MovementState& state) {
        state.path.clear();
        map.findPathAStar(unit.pos, target, state.path);
        state.idx = 0;
        state.accumulator = 0.f;
        state.lastTarget = target;
        state.hasLast = true;
    }

    MoveIntent planMoveIntent(const UnitSnapshot& unit,
                              float dt,
                              const Map& map,
                              const std::vector<AttackableSnapshot>& enemies,
                              const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                              const std::vector<AttackableKey>& forcedKeys,
                              const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                              const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                              const std::vector<UnitSnapshot>& allies,
                              bool baseAlive,
                              const Coord& basePos,
                              const std::unordered_set<std::uint64_t>& occ) {
        MoveIntent intent;
        intent.unitId = unit.id;
        intent.from = unit.pos;
        intent.to = unit.pos;
        intent.commandMove = unit.commandMoveActive;

        std::vector<float> heat = buildHeatMap(map, allies, occ, unit.id);

        IMovementBehavior::MovementState nextState = unit.movementState;
        Coord desiredTarget = unit.pos;
        bool hasDesiredTarget = false;
        MoveReason reason = unit.moveReason;
        bool retreating = unit.retreating;
        float retreatTimer = unit.retreatTimer;
        bool hasAnchor = unit.hasRetreatAnchor;
        Coord anchor = unit.retreatAnchor;

        if (unit.commandMoveActive) {
            reason = MoveReason::Command;
            Coord cmdTarget{};
            bool hasCmdTarget = false;
            if (!unit.movementState.path.empty()) {
                cmdTarget = unit.movementState.path.back();
                hasCmdTarget = true;
            } else if (unit.movementState.hasLast) {
                cmdTarget = unit.movementState.lastTarget;
                hasCmdTarget = true;
            }
            if (hasCmdTarget) {
                desiredTarget = applyAnchorSlot(map, occ, cmdTarget, unit.id, unit.pos);
                hasDesiredTarget = true;
            }
        } else {
            std::vector<const AttackableSnapshot*> visible =
                collectVisibleEnemies(unit, map, enemies, enemyIndex, forcedKeys);
            ReachableGrid reachable = buildReachableGrid(map, unit.pos);
            TargetChoice choice = chooseTarget(unit, visible, allies,
                                               incomingDamage, lockedCounts,
                                               kTargetParams, map, reachable,
                                               occ);
            float localThreat = computeLocalThreat(unit, visible, kThreatRadius);
            const AttackableSnapshot* target = choice.target;
            if (unit.commandAttackActive && unit.commandTarget.id >= 0) {
                auto it = enemyIndex.find(unit.commandTarget);
                if (it != enemyIndex.end()) {
                    target = &enemies[it->second];
                    choice.baseOpportunity = target->isBase
                        ? baseOpportunityScore(unit, *target, allies, visible,
                                               kTargetParams, localThreat)
                        : 0.f;
                }
            }
            float dist = target ? unit.pos.mhtDistanceTo(target->pos) : 0.f;
            float cdAfter = std::max(0.0f, unit.cooldown - dt);
            bool inRange = target ? inAttackRange(unit, map, *target) : false;
            CombatAction action = decideCombatAction(unit, target, dist, inRange, cdAfter,
                                                     localThreat, choice.baseOpportunity,
                                                     kActionParams);
            float hpRatio = (unit.maxHp > 0.f) ? (unit.hp / unit.maxHp) : 1.0f;
            bool enterRetreat = (hpRatio <= kRetreatParams.hpEnter);
            bool exitRetreat = (hpRatio >= kRetreatParams.hpExit &&
                                retreatTimer <= 0.f);

            if (retreating) {
                if (exitRetreat) {
                    retreating = false;
                    retreatTimer = 0.f;
                    hasAnchor = false;
                }
            } else if (enterRetreat) {
                retreating = true;
                retreatTimer = kRetreatParams.retreatMinTime;
                hasAnchor = false;
            }

            if (retreating) {
                if (!hasAnchor) {
                    if (baseAlive) {
                        anchor = basePos;
                        hasAnchor = true;
                    } else {
                        anchor = chooseSafeSpot(unit, map, enemies, kRetreatParams.sampleRadius);
                        hasAnchor = true;
                    }
                }
                anchor = nearestPassable(map, anchor, 2);
                int distToAnchor = unit.pos.mhtDistanceTo(anchor);
                if (distToAnchor > 0) {
                    desiredTarget = applyAnchorSlot(map, occ, anchor, unit.id, unit.pos);
                    hasDesiredTarget = true;
                    reason = MoveReason::Retreat;
                } else {
                    nextState.path.clear();
                    nextState.idx = 0;
                    nextState.accumulator = 0.f;
                    nextState.hasLast = false;
                    reason = MoveReason::None;
                }
                if (distToAnchor > kRetreatParams.maxRetreatDist) {
                    desiredTarget = applyAnchorSlot(map, occ, anchor, unit.id, unit.pos);
                    hasDesiredTarget = true;
                    reason = MoveReason::Retreat;
                }
            }

            if (!retreating && action == CombatAction::Retreat && target) {
                int steps = static_cast<int>(std::ceil(kActionParams.kiteExtraDist));
                Coord awayGoal{unit.pos.x + (unit.pos.x - target->pos.x),
                               unit.pos.y + (unit.pos.y - target->pos.y)};
                desiredTarget = stepAlongManhattan(unit.pos, awayGoal, steps);
                hasDesiredTarget = true;
                reason = MoveReason::Retreat;
            } else if (!retreating && action == CombatAction::Kite && target) {
                int desiredDist = std::max(1, static_cast<int>(std::floor(
                    unit.stats.attackRange - kActionParams.kiteMargin)));
                Coord kitePos{};
                if (selectAttackPosition(unit, *target, map, enemies, reachable, occ,
                                         desiredDist, desiredDist + 1, kitePos)) {
                    desiredTarget = kitePos;
                } else {
                    int steps = static_cast<int>(std::ceil(kActionParams.kiteExtraDist));
                    Coord awayGoal{unit.pos.x + (unit.pos.x - target->pos.x),
                                   unit.pos.y + (unit.pos.y - target->pos.y)};
                    desiredTarget = stepAlongManhattan(unit.pos, awayGoal, steps);
                }
                hasDesiredTarget = true;
                reason = MoveReason::Kite;
            } else if (!retreating &&
                       (action == CombatAction::Chase || action == CombatAction::Siege) && target) {
                bool ranged = unit.stats.attackRange > 2.5f;
                bool needMove = !inRange;
                if (ranged) {
                    int desiredDist = std::max(1, static_cast<int>(std::floor(
                        unit.stats.attackRange - kActionParams.kiteMargin)));
                    if (dist > desiredDist) {
                        needMove = true;
                    }
                    if (needMove) {
                        Coord firePos{};
                        int minDist = std::max(1, desiredDist - 1);
                        int maxDist = std::max(minDist, desiredDist + 1);
                        if (selectAttackPosition(unit, *target, map, enemies, reachable, occ,
                                                 minDist, maxDist, firePos)) {
                            desiredTarget = firePos;
                            hasDesiredTarget = true;
                        } else {
                            Coord anchorSlot = applyAnchorSlot(map, occ, target->pos, unit.id, unit.pos);
                            desiredTarget = nearestReachableToTarget(map, reachable, anchorSlot);
                            hasDesiredTarget = (desiredTarget != unit.pos);
                        }
                    }
                } else {
                    int meleeRange = std::max(1, static_cast<int>(std::floor(
                        unit.stats.attackRange + 0.001f)));
                    if (dist > meleeRange) {
                        needMove = true;
                    }
                    if (needMove) {
                        Coord meleePos{};
                        if (selectAttackPosition(unit, *target, map, enemies, reachable, occ,
                                                 1, meleeRange, meleePos)) {
                            desiredTarget = meleePos;
                            hasDesiredTarget = true;
                        } else {
                            Coord anchorSlot = applyAnchorSlot(map, occ, target->pos, unit.id, unit.pos);
                            desiredTarget = nearestReachableToTarget(map, reachable, anchorSlot);
                            hasDesiredTarget = (desiredTarget != unit.pos);
                        }
                    }
                }
                if (hasDesiredTarget) {
                    reason = (action == CombatAction::Siege) ? MoveReason::Siege : MoveReason::Chase;
                } else {
                    nextState.path.clear();
                    nextState.idx = 0;
                    nextState.accumulator = 0.f;
                    nextState.hasLast = false;
                    reason = MoveReason::None;
                }
            }
        }

        if (hasDesiredTarget) {
            desiredTarget = clampToMap(map, desiredTarget);
            if (!nextState.hasLast || nextState.lastTarget != desiredTarget) {
                rebuildPathStateHeat(unit, map, desiredTarget, heat, nextState);
            }
        }

        if (nextState.path.empty() || nextState.idx >= nextState.path.size()) {
            intent.nextState = std::move(nextState);
            intent.setIdle = true;
            intent.reason = MoveReason::None;
            intent.retreating = retreating;
            intent.retreatTimer = retreatTimer;
            intent.retreatAnchor = anchor;
            intent.hasRetreatAnchor = retreating && hasAnchor;
            fillMoveCandidates(unit, map, desiredTarget, heat, occ, intent);
            return intent;
        }

        Coord nextPos = unit.pos;
        float sp = movementSpend(unit, map.getTile(unit.pos));
        nextState.accumulator += sp * dt;

        while (nextState.accumulator >= 1.0f && nextState.idx + 1 < nextState.path.size()) {
            nextState.accumulator -= 1.f;
            nextPos = nextState.path[++nextState.idx];
        }

        if (nextState.idx + 1 >= nextState.path.size()) {
            nextState.path.clear();
        }

        intent.to = nextPos;
        intent.hasMove = (nextPos != intent.from);
        intent.setIdle = nextState.path.empty();
        intent.reason = intent.setIdle ? MoveReason::None : reason;
        intent.retreating = retreating;
        intent.retreatTimer = retreatTimer;
        intent.retreatAnchor = anchor;
        intent.hasRetreatAnchor = retreating && hasAnchor;
        fillMoveCandidates(unit, map, desiredTarget, heat, occ, intent);
        intent.nextState = std::move(nextState);
        return intent;
    }
} 

WorldState::WorldState(int width, int height)
    : map(width, height) {}

WorldRuntime::WorldRuntime()
    : taskPool(4) {}

void ControlState::resetTargeting() {
    pendingTarget.reset();
    controlMode = ControlMode::Idle;
}

void ControlState::enterTargeting() {
    pendingTarget.reset();
    controlMode = ControlMode::Targeting;
}

void ControlState::cancelTargeting() {
    resetTargeting();
}

void ControlState::commitTargeting() {
    resetTargeting();
}

WorldDataContext::WorldDataContext(WorldState& state)
    : map(state.map),
      baseA(state.baseA),
      baseB(state.baseB),
      unitsA(state.unitsA),
      unitsB(state.unitsB),
      enemiesA(state.enemiesA),
      enemiesB(state.enemiesB),
      forcedVisibleForA(state.forcedVisibleForA),
      forcedVisibleForB(state.forcedVisibleForB),
      lastVisionIntents(state.lastVisionIntents),
      lastTargetHints(state.lastTargetHints),
      lastIncomingDamageA(state.lastIncomingDamageA),
      lastIncomingDamageB(state.lastIncomingDamageB),
      lastLockedTargetsA(state.lastLockedTargetsA),
      lastLockedTargetsB(state.lastLockedTargetsB),
      nextUnitId(state.nextUnitId),
      nextBaseId(state.nextBaseId) {}

WorldRuntimeContext::WorldRuntimeContext(WorldRuntime& runtime)
    : taskPool(runtime.taskPool),
      renderThread(runtime.renderThread),
      renderRunning(runtime.renderRunning),
      paused(runtime.paused),
      gameEnded(runtime.gameEnded),
      gameEndTimestampMs(runtime.gameEndTimestampMs),
      quitRequested(runtime.quitRequested),
      worldMutex(runtime.worldMutex),
      uiEventMutex(runtime.uiEventMutex),
      uiEvents(runtime.uiEvents) {}

WorldControlContext::WorldControlContext(ControlState& control)
    : commandQueue(control.commandQueue),
      lastCommandInput(control.lastCommandInput),
      lastCommandFeedback(control.lastCommandFeedback),
      selectedUnitIds(control.selectedUnitIds),
      controlMode(control.controlMode),
      pendingTarget(control.pendingTarget),
      awaitingProductionChoice(control.awaitingProductionChoice),
      productionChoiceBase(control.productionChoiceBase),
      productionInputBuffer(control.productionInputBuffer) {}

const AiScoring::TargetScoreParams AiScoring::kTargetParams{};
const AiScoring::ActionParams AiScoring::kActionParams{};
const AiScoring::RetreatParams AiScoring::kRetreatParams{};
const AiScoring::OocParams AiScoring::kOocParams{};

float AiScoring::unitPreference(UnitType self, UnitType target) {
    return ::unitPreference(self, target);
}

float AiScoring::basePreference(UnitType self) {
    return ::basePreference(self);
}

float AiScoring::threatScore(const AttackableSnapshot& target) {
    return ::threatScore(target);
}

float AiScoring::distanceScore(float dist) {
    return ::distanceScore(dist);
}

float AiScoring::ttkScore(const UnitSnapshot& self, const AttackableSnapshot& target) {
    return ::ttkScore(self, target);
}

bool AiScoring::containsKey(const std::vector<const AttackableSnapshot*>& visible,
                            const AttackableKey& key) {
    return ::containsKey(visible, key);
}

std::vector<const AiScoring::AttackableSnapshot*> AiScoring::collectVisibleEnemies(
    const UnitSnapshot& unit,
    const Map& map,
    const std::vector<AttackableSnapshot>& enemies,
    const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
    const std::vector<AttackableKey>& forcedKeys) {
    return ::collectVisibleEnemies(unit, map, enemies, enemyIndex, forcedKeys);
}

float AiScoring::computeLocalThreat(const UnitSnapshot& self,
                                    const std::vector<const AttackableSnapshot*>& enemies,
                                    float radius) {
    return ::computeLocalThreat(self, enemies, radius);
}

int AiScoring::countNearbyAllies(const std::vector<UnitSnapshot>& allies,
                                 const Coord& pos,
                                 int radius) {
    return ::countNearbyAllies(allies, pos, radius);
}

int AiScoring::countNearbyEnemies(const std::vector<const AttackableSnapshot*>& enemies,
                                  const Coord& pos,
                                  int radius) {
    return ::countNearbyEnemies(enemies, pos, radius);
}

float AiScoring::baseOpportunityScore(const UnitSnapshot& self,
                                      const AttackableSnapshot& base,
                                      const std::vector<UnitSnapshot>& allies,
                                      const std::vector<const AttackableSnapshot*>& enemies,
                                      const TargetScoreParams& params,
                                      float localThreat) {
    return ::baseOpportunityScore(self, base, allies, enemies, params, localThreat);
}

AiScoring::ReachableGrid AiScoring::buildReachableGrid(const Map& map, const Coord& start) {
    return ::buildReachableGrid(map, start);
}

float AiScoring::firingPositionAvailability(const UnitSnapshot& self,
                                            const AttackableSnapshot& target,
                                            const Map& map,
                                            const ReachableGrid& reachable,
                                            const std::unordered_set<std::uint64_t>& occ,
                                            int cap) {
    return ::firingPositionAvailability(self, target, map, reachable, occ, cap);
}

AiScoring::TargetChoice AiScoring::chooseTarget(
    const UnitSnapshot& self,
    const std::vector<const AttackableSnapshot*>& visible,
    const std::vector<UnitSnapshot>& allies,
    const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
    const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
    const TargetScoreParams& params,
    const Map& map,
    const ReachableGrid& reachable,
    const std::unordered_set<std::uint64_t>& occ) {
    return ::chooseTarget(self, visible, allies, incomingDamage, lockedCounts,
                          params, map, reachable, occ);
}

bool AiScoring::inAttackRange(const UnitSnapshot& self,
                              const Map& map,
                              const AttackableSnapshot& target) {
    return ::inAttackRange(self, map, target);
}

float AiScoring::computeAttackDamage(const UnitSnapshot& self, const Map& map) {
    return ::computeAttackDamage(self, map);
}

float AiScoring::computeAttackCooldown(const UnitSnapshot& self, const Map& map) {
    return ::computeAttackCooldown(self, map);
}

CombatAction AiScoring::decideCombatAction(const UnitSnapshot& self,
                                           const AttackableSnapshot* target,
                                           float dist,
                                           bool inRange,
                                           float cdAfter,
                                           float localThreat,
                                           float baseOpportunity,
                                           const ActionParams& params) {
    return ::decideCombatAction(self, target, dist, inRange, cdAfter,
                                localThreat, baseOpportunity, params);
}

AttackIntent AiScoring::planAttackIntent(
    const UnitSnapshot& unit,
    float dt,
    const Map& map,
    const std::vector<AttackableSnapshot>& enemies,
    const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
    const std::vector<AttackableKey>& forcedKeys,
    const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
    const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
    const std::vector<UnitSnapshot>& allies,
    const std::unordered_set<std::uint64_t>& occ) {
    return ::planAttackIntent(unit, dt, map, enemies, enemyIndex, forcedKeys,
                              incomingDamage, lockedCounts, allies, occ);
}

float AiScoring::movementSpend(const UnitSnapshot& unit, const Tile& tile) {
    return ::movementSpend(unit, tile);
}

Coord AiScoring::chooseSafeSpot(const UnitSnapshot& self,
                                const Map& map,
                                const std::vector<AttackableSnapshot>& enemies,
                                int sampleRadius) {
    return ::chooseSafeSpot(self, map, enemies, sampleRadius);
}

void AiScoring::rebuildPathState(const UnitSnapshot& unit,
                                 const Map& map,
                                 const Coord& desiredTarget,
                                 IMovementBehavior::MovementState& nextState) {
    ::rebuildPathState(unit, map, desiredTarget, nextState);
}

MoveIntent AiScoring::planMoveIntent(
    const UnitSnapshot& unit,
    float dt,
    const Map& map,
    const std::vector<AttackableSnapshot>& enemies,
    const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
    const std::vector<AttackableKey>& forcedKeys,
    const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
    const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
    const std::vector<UnitSnapshot>& allies,
    bool baseAlive,
    const Coord& basePos,
    const std::unordered_set<std::uint64_t>& occ) {
    return ::planMoveIntent(unit, dt, map, enemies, enemyIndex, forcedKeys,
                            incomingDamage, lockedCounts, allies,
                            baseAlive, basePos, occ);
}

void WorldRuntimeContext::start(WorldDataContext& data,
                                WorldControlContext& control,
                                SystemsBundle& systems) {
    if (renderRunning.load()) return;

    renderRunning.store(true);
    if (!systems.render) systems.render = std::make_unique<RenderSystem>();
    systems.render->clock.restart();

    renderThread = std::thread([this, &data, &control, &systems]() {
        RenderSystem& renderSystem = *systems.render;

        const unsigned W = static_cast<unsigned>(data.map.getWidth());
        const unsigned H = static_cast<unsigned>(data.map.getHeight());

        const float hud  = RenderConfig::HUD_WIDTH;
        const float pad  = 40.f;

        const sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
        const float maxW = std::floor(static_cast<float>(desktop.size.x) * 0.9f);
        const float maxH = std::floor(static_cast<float>(desktop.size.y) * 0.9f);

        const float mapMaxW = std::max(0.f, maxW - (hud + pad));
        const float mapMaxH = std::max(0.f, maxH - pad);

        int tileSize = RenderConfig::TILE_SIZE;
        if (static_cast<float>(tileSize) * W > mapMaxW ||
            static_cast<float>(tileSize) * H > mapMaxH) {
            float fit = std::floor(std::min(mapMaxW / static_cast<float>(W),
                                            mapMaxH / static_cast<float>(H)));
            int tileFit = std::max(static_cast<int>(fit), 24);
            if (tileFit >= RenderConfig::NATIVE_TILE_SIZE) tileSize = RenderConfig::NATIVE_TILE_SIZE;
            else if (tileFit >= 48) tileSize = 48;
            else if (tileFit >= 32) tileSize = 32;
            else tileSize = tileFit;
        }
        RenderConfig::TILE_SIZE = tileSize;

        const float mapPixelW = static_cast<float>(tileSize) * W;
        const float mapPixelH = static_cast<float>(tileSize) * H;
        const unsigned winW = static_cast<unsigned>(
            std::floor(std::min(mapPixelW + hud + pad, maxW)));
        const unsigned winH = static_cast<unsigned>(
            std::floor(std::min(mapPixelH + pad, maxH)));

        sf::RenderWindow window(
            sf::VideoMode({winW, winH}),
            "Battlefield Simulator",
            sf::Style::Titlebar | sf::Style::Close
        );
        window.setPosition(sf::Vector2i{60, 60});
        window.setFramerateLimit(60);

        auto commitTargeting = [&]() {
            control.commitTargeting();
            resume();
        };
        auto cancelTargeting = [&]() {
            control.cancelTargeting();
            resume();
        };
        auto enterTargeting = [&]() {
            control.enterTargeting();
            pause();
        };
        auto postUiInput = [&](const std::string& input) {
            enqueueUiEvent(std::optional<std::string>(input), std::nullopt);
        };
        auto postUi = [&](const std::string& input, const std::string& feedback) {
            enqueueUiEvent(std::optional<std::string>(input),
                           std::optional<std::string>(feedback));
        };
        auto selectUnit = [&](const std::shared_ptr<Unit>& unit) {
            control.selectedUnitIds = {unit->id};
            postUi("click select", "Selected #" + std::to_string(unit->id));
        };
        auto selectedFaction = [&]() -> std::optional<Faction> {
            if (control.selectedUnitIds.empty()) return std::nullopt;
            auto u = data.findUnit(control.selectedUnitIds.front());
            if (!u) return std::nullopt;
            return u->getFaction();
        };
        auto findUnitAt = [&](const Coord& coord) -> std::shared_ptr<Unit> {
            for (auto& u : data.unitsA) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            for (auto& u : data.unitsB) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            return nullptr;
        };
        auto resolveEnemyAt = [&](Faction enemyFaction, const Coord& coord)
            -> std::shared_ptr<IAttackable> {
            if (enemyFaction == Faction::A) {
                if (data.baseA && !data.baseA->isDestroyed() && data.baseA->getPos() == coord) return data.baseA;
                for (auto& u : data.unitsA) {
                    if (u && u->isAlive() && u->getPos() == coord) return u;
                }
                return nullptr;
            }
            if (data.baseB && !data.baseB->isDestroyed() && data.baseB->getPos() == coord) return data.baseB;
            for (auto& u : data.unitsB) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            return nullptr;
        };
        auto issueMoveTo = [&](const Coord& coord) {
            for (int id : control.selectedUnitIds) {
                auto u = data.findUnit(id);
                if (u) u->issueMove(coord);
            }
            postUi("click move",
                   "Move to " + std::to_string(coord.x) + "," + std::to_string(coord.y));
        };
        auto issueAttack = [&](const std::shared_ptr<IAttackable>& target) {
            for (int id : control.selectedUnitIds) {
                auto u = data.findUnit(id);
                if (u) u->issueAttackTarget(target);
            }
            postUi("click attack", "Attack target set");
        };
        auto shutdownStart = std::chrono::steady_clock::time_point{};
        bool shutdownArmed = false;
        while (renderRunning.load()) {
            if (!window.isOpen()) {
                renderRunning.store(false);
                requestQuit();
                break;
            }

            while (const std::optional event = window.pollEvent()) {
                if (event->is<sf::Event::Closed>()) {
                    window.close();
                    renderRunning.store(false);
                    requestQuit();
                }

                if (auto key = event->getIf<sf::Event::KeyPressed>()) {
                    using sf::Keyboard::Key;

                    if (control.awaitingProductionChoice) {
                        if (key->code == Key::Backspace) {
                            control.handleProductionBackspace(*this);
                            continue;
                        }
                        if (key->code == Key::P) {
                            control.cancelProductionChoice(*this);
                            resume();
                            postUiInput("production cancel (P)");
                            continue;
                        }
                        if (key->code == Key::Enter) {
                            control.commitProductionChoice(*this);
                            continue;
                        }
                        if (key->code == Key::Escape) {
                            control.cancelProductionChoice(*this);
                            resume();
                            continue;
                        }
                    }

                    if (control.controlMode == ControlState::ControlMode::Targeting) {
                        if (key->code == Key::Enter) {
                            std::unique_lock<std::shared_mutex> lock(worldMutex);
                            if (!control.pendingTarget || control.selectedUnitIds.empty()) {
                                continue;
                            }
                            auto selFactionOpt = selectedFaction();
                            if (!selFactionOpt.has_value()) {
                                cancelTargeting();
                                continue;
                            }
                            Faction selFaction = *selFactionOpt;
                            Faction enemyFaction =
                                (selFaction == Faction::A) ? Faction::B : Faction::A;

                            if (control.pendingTarget->kind == ControlState::PendingTarget::Kind::Unit) {
                                auto targetUnit = data.findUnit(control.pendingTarget->unitId);
                                if (targetUnit && targetUnit->isAlive()) {
                                    if (targetUnit->getFaction() == enemyFaction) {
                                        issueAttack(targetUnit);
                                    } else {
                                        issueMoveTo(targetUnit->getPos());
                                    }
                                } else {
                                    issueMoveTo(control.pendingTarget->tile);
                                }
                            } else {
                                auto target = resolveEnemyAt(enemyFaction, control.pendingTarget->tile);
                                if (target) {
                                    issueAttack(target);
                                } else {
                                    issueMoveTo(control.pendingTarget->tile);
                                }
                            }
                            commitTargeting();
                            continue;
                        }
                        if (key->code == Key::Escape) {
                            std::unique_lock<std::shared_mutex> lock(worldMutex);
                            cancelTargeting();
                            continue;
                        }
                    }

                    if (key->code == Key::Enter) {
                        if (renderSystem.inputActive) {
                            if (!renderSystem.inputBuffer.empty()) {
                                control.enqueueCommand(renderSystem.inputBuffer);
                                postUiInput(renderSystem.inputBuffer);
                            }
                            renderSystem.inputBuffer.clear();
                            renderSystem.inputActive = false;
                        } else {
                            renderSystem.inputBuffer.clear();
                            renderSystem.inputActive = true;
                        }
                    } else if (key->code == Key::Backspace) {
                        if (renderSystem.inputActive && !renderSystem.inputBuffer.empty()) {
                            renderSystem.inputBuffer.pop_back();
                        }
                    } else if (key->code == Key::Escape) {
                        if (renderSystem.inputActive) {
                            renderSystem.inputBuffer.clear();
                            renderSystem.inputActive = false;
                        } else {
                            window.close();
                            renderRunning.store(false);
                            requestQuit();
                        }
                    } else if (key->code == Key::Q) {
                        window.close();
                        renderRunning.store(false);
                        requestQuit();
                    } else if (key->code == Key::P) {
                        togglePause();
                        postUi("toggle pause", paused.load() ? "Paused" : "Resumed");
                    }
                }

                if (renderSystem.inputActive && !control.awaitingProductionChoice) {
                    if (const auto text = event->getIf<sf::Event::TextEntered>()) {
                        char32_t uni = text->unicode;
                        if (uni >= 32 && uni < 127) {
                            renderSystem.inputBuffer.push_back(static_cast<char>(uni));
                        }
                    }
                } else if (control.awaitingProductionChoice) {
                    if (const auto text = event->getIf<sf::Event::TextEntered>()) {
                        char32_t uni = text->unicode;
                        if (uni >= U'0' && uni <= U'9') {
                            control.handleProductionDigit(static_cast<char>(uni), *this);
                        }
                    }
                }

                if (const auto mouse = event->getIf<sf::Event::MouseButtonPressed>()) {
                    if (control.awaitingProductionChoice && mouse->button != sf::Mouse::Button::Middle) {
                        // 忽略其他鼠标操作，保持选择流程
                        continue;
                    }

                    auto coordOpt = renderSystem.pixelToTile(data, window, mouse->position);

                    if (mouse->button == sf::Mouse::Button::Middle) {
                        togglePause();
                        postUi("mouse pause", paused.load() ? "Paused" : "Resumed");
                        continue;
                    }

                    if (!coordOpt) continue;
                    Coord clicked = *coordOpt;

                    if (mouse->button == sf::Mouse::Button::Left) {
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        if (control.controlMode == ControlState::ControlMode::Targeting) {
                            auto hitUnit = findUnitAt(clicked);
                            auto selFactionOpt = selectedFaction();
                            if (hitUnit && selFactionOpt.has_value() &&
                                hitUnit->getFaction() == *selFactionOpt) {
                                selectUnit(hitUnit);
                                control.pendingTarget.reset();
                            } else if (hitUnit) {
                                control.pendingTarget = ControlState::PendingTarget{
                                    ControlState::PendingTarget::Kind::Unit,
                                    hitUnit->getPos(),
                                    hitUnit->id
                                };
                            } else {
                                control.pendingTarget = ControlState::PendingTarget{
                                    ControlState::PendingTarget::Kind::Tile,
                                    clicked,
                                    -1
                                };
                            }
                            continue;
                        }
                        std::shared_ptr<Base> baseTarget;
                        if (data.baseA && !data.baseA->isDestroyed() && data.baseA->getPos() == clicked) {
                            baseTarget = data.baseA;
                        } else if (data.baseB && !data.baseB->isDestroyed() && data.baseB->getPos() == clicked) {
                            baseTarget = data.baseB;
                        }
                        if (baseTarget) {
                            control.beginProductionChoice(baseTarget, *this);
                            continue;
                        }

                        std::shared_ptr<Unit> pick;
                        for (auto& u : data.unitsA) {
                            if (u && u->isAlive() && u->getPos() == clicked) {
                                pick = u;
                                break;
                            }
                        }
                        if (pick) {
                            selectUnit(pick);
                            enterTargeting();
                        }
                    } else if (mouse->button == sf::Mouse::Button::Right) {
                        if (control.controlMode == ControlState::ControlMode::Targeting) {
                            continue;
                        }
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        if (control.selectedUnitIds.empty()) continue;

                        std::shared_ptr<IAttackable> target;
                        if (data.baseB && !data.baseB->isDestroyed() && data.baseB->getPos() == clicked) {
                            target = data.baseB;
                        }
                        if (!target) {
                            for (auto& u : data.unitsB) {
                                if (u && u->isAlive() && u->getPos() == clicked) {
                                    target = u;
                                    break;
                                }
                            }
                        }

                        if (target) {
                            for (int id : control.selectedUnitIds) {
                                auto u = data.findUnit(id);
                                if (u) u->issueAttackTarget(target);
                            }
                            pause();
                            postUi("click attack", "Attack target set");
                        } else {
                            for (int id : control.selectedUnitIds) {
                                auto u = data.findUnit(id);
                                if (u) u->issueMove(clicked);
                            }
                            pause();
                            postUi("click move",
                                   "Move to " + std::to_string(clicked.x) + "," +
                                       std::to_string(clicked.y));
                        }
                    }
                }
            }

            if (!shutdownArmed && gameEnded.load()) {
                shutdownArmed = true;
                auto ms = gameEndTimestampMs.load();
                if (ms > 0) {
                    shutdownStart = std::chrono::steady_clock::time_point(std::chrono::milliseconds(ms));
                } else {
                    shutdownStart = std::chrono::steady_clock::now();
                }
            }

            if (shutdownArmed) {
                auto elapsed = std::chrono::steady_clock::now() - shutdownStart;
                if (elapsed >= std::chrono::seconds(5)) {
                    window.close();
                    renderRunning.store(false);
                    requestQuit();
                }
            }

            if (!renderRunning.load() || !window.isOpen()) {
                continue;
            }

            window.clear(sf::Color(18, 24, 32));
            {
                std::shared_lock<std::shared_mutex> lock(worldMutex);
                renderSystem.renderSfml(data, control, *this, window);
            }
            window.display();
        }
    });
}

void WorldRuntimeContext::stop() {
    renderRunning.store(false);
    if (renderThread.joinable()) {
        renderThread.join();
    }
}

void WorldRuntimeContext::join() {
    if (renderThread.joinable()) {
        renderThread.join();
    }
}

GameWorld::GameWorld()
    : state(MAP_WIDTH, MAP_HEIGHT)
{
    WorldDataContext data(state);
    MapGenerator gen(MAP_WIDTH, MAP_HEIGHT);

    bool ok = false;
    Coord baseAPos;
    Coord baseBPos;

    while (!ok) {
        Map candidate = gen.generate();

        if (pickBasePositions(candidate, baseAPos, baseBPos)) {
            state.map = std::move(candidate);
            ok = true;
        }
    }
    state.baseA = std::make_shared<Base>(baseAPos, Faction::A);
    state.baseB = std::make_shared<Base>(baseBPos, Faction::B);
    data.registerBase(state.baseA);
    data.registerBase(state.baseB);

    state.enemiesA.clear();
    state.enemiesB.clear();

    
    runtime.taskPool.init();
    systems.render = std::make_unique<RenderSystem>();
}


GameWorld::~GameWorld() {
    WorldRuntimeContext runtimeCtx(runtime);
    runtimeCtx.stop();
}

void GameWorld::update(float dt) {
    WorldDataContext data(state);
    WorldRuntimeContext runtimeCtx(runtime);
    WorldControlContext controlCtx(control);

    runtimeCtx.drainUiEvents(controlCtx);
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        controlCtx.processCommands(data, runtimeCtx);
        if (runtimeCtx.paused.load() || runtimeCtx.gameEnded.load()) {
            return;
        }

        data.decayForcedReveals(dt);
        systems.base.update(data, dt);
        data.rebuildEnemies();

        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(state.unitsA.size() + state.unitsB.size());
        for (auto& u : state.unitsA) allUnits.push_back(u);
        for (auto& u : state.unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });
        for (auto& u : allUnits) {
            if (u && u->behavior) {
                u->behavior->applyPendingCommand(*u, data.map);
            }
        }
    }

    const std::size_t localCount = std::max<std::size_t>(1, runtimeCtx.taskPool.workerCount());

    // Snapshot stage: read-only data for planning.
    std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageA;
    std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageB;
    std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsA;
    std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsB;
    Coord baseAPos{};
    Coord baseBPos{};
    bool baseAAlive = false;
    bool baseBAlive = false;

    std::vector<UnitSnapshot> unitsASnap;
    std::vector<UnitSnapshot> unitsBSnap;
    std::vector<AttackableSnapshot> enemiesASnap;
    std::vector<AttackableSnapshot> enemiesBSnap;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexA;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexB;
    std::vector<AttackableKey> forcedAKeys;
    std::vector<AttackableKey> forcedBKeys;
    std::unordered_set<std::uint64_t> occSnap;
    {
        std::shared_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        incomingDamageA = state.lastIncomingDamageA;
        incomingDamageB = state.lastIncomingDamageB;
        lockedTargetsA = state.lastLockedTargetsA;
        lockedTargetsB = state.lastLockedTargetsB;

        baseAAlive = state.baseA && !state.baseA->isDestroyed();
        baseBAlive = state.baseB && !state.baseB->isDestroyed();
        if (baseAAlive) baseAPos = state.baseA->pos;
        if (baseBAlive) baseBPos = state.baseB->pos;

        occSnap.reserve(state.unitsA.size() + state.unitsB.size() + 2);
        buildUnitSnapshots(state.unitsA, unitsASnap, &occSnap);
        buildUnitSnapshots(state.unitsB, unitsBSnap, &occSnap);
        if (baseAAlive) occSnap.insert(packCoord(baseAPos));
        if (baseBAlive) occSnap.insert(packCoord(baseBPos));

        buildEnemySnapshots(state.unitsB, state.baseB, enemiesASnap);
        buildEnemySnapshots(state.unitsA, state.baseA, enemiesBSnap);
        buildForcedKeys(state.forcedVisibleForA, forcedAKeys);
        buildForcedKeys(state.forcedVisibleForB, forcedBKeys);
    }

    auto byUnitId = [](const UnitSnapshot& a, const UnitSnapshot& b) {
        return a.id < b.id;
    };
    std::sort(unitsASnap.begin(), unitsASnap.end(), byUnitId);
    std::sort(unitsBSnap.begin(), unitsBSnap.end(), byUnitId);

    auto byAttackableKey = [](const AttackableSnapshot& a, const AttackableSnapshot& b) {
        if (a.key.type != b.key.type) return a.key.type < b.key.type;
        return a.key.id < b.key.id;
    };
    std::sort(enemiesASnap.begin(), enemiesASnap.end(), byAttackableKey);
    std::sort(enemiesBSnap.begin(), enemiesBSnap.end(), byAttackableKey);
    rebuildEnemyIndex(enemiesASnap, enemyIndexA);
    rebuildEnemyIndex(enemiesBSnap, enemyIndexB);

    auto keyLess = [](const AttackableKey& a, const AttackableKey& b) {
        if (a.type != b.type) return a.type < b.type;
        return a.id < b.id;
    };
    std::sort(forcedAKeys.begin(), forcedAKeys.end(), keyLess);
    forcedAKeys.erase(std::unique(forcedAKeys.begin(), forcedAKeys.end()), forcedAKeys.end());
    std::sort(forcedBKeys.begin(), forcedBKeys.end(), keyLess);
    forcedBKeys.erase(std::unique(forcedBKeys.begin(), forcedBKeys.end()), forcedBKeys.end());

    // Plan stage (Vision): tasks read snapshots only; no world writes here.
    auto visionGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> visionLocals(localCount);

    auto scheduleVision = [&](const UnitSnapshot& unit,
                              const std::vector<AttackableSnapshot>& enemies,
                              const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                              const std::vector<AttackableKey>& forcedKeys,
                              const std::vector<UnitSnapshot>& allies,
                              const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                              const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                              const std::unordered_set<std::uint64_t>& occ) {
        if (unit.hp <= 0.f) return;
        runtimeCtx.taskPool.submit([&, unit]() {
            std::vector<const AttackableSnapshot*> visible =
                collectVisibleEnemies(unit, state.map, enemies, enemyIndex, forcedKeys);
            ReachableGrid reachable = buildReachableGrid(state.map, unit.pos);

            VisionIntent visionIntent;
            visionIntent.unitId = unit.id;
            visionIntent.visibleEnemyIds.reserve(visible.size());
            for (const auto* e : visible) {
                if (e && e->key.id >= 0) visionIntent.visibleEnemyIds.push_back(e->key.id);
            }

            TargetHint hint;
            hint.unitId = unit.id;
            TargetChoice choice = chooseTarget(unit, visible, allies,
                                               incomingDamage, lockedCounts,
                                               kTargetParams, state.map, reachable,
                                               occ);
            hint.targetId = choice.target ? choice.target->key.id : -1;

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= visionLocals.size()) {
                idx = 0;
            }
            IntentBuffer& buffer = visionLocals[idx];
            buffer.visionIntents.push_back(std::move(visionIntent));
            buffer.targetHints.push_back(hint);
        }, visionGroup);
    };

    for (const auto& u : unitsASnap) {
        scheduleVision(u, enemiesASnap, enemyIndexA, forcedAKeys, unitsASnap,
                       incomingDamageA, lockedTargetsA, occSnap);
    }
    for (const auto& u : unitsBSnap) {
        scheduleVision(u, enemiesBSnap, enemyIndexB, forcedBKeys, unitsBSnap,
                       incomingDamageB, lockedTargetsB, occSnap);
    }
    visionGroup->wait();

    // Apply stage (Vision): main thread writes only, deterministic order.
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        std::vector<VisionIntent> mergedVision;
        std::vector<TargetHint> mergedHints;
        for (auto& buffer : visionLocals) {
            mergedVision.insert(mergedVision.end(),
                                std::make_move_iterator(buffer.visionIntents.begin()),
                                std::make_move_iterator(buffer.visionIntents.end()));
            mergedHints.insert(mergedHints.end(),
                               std::make_move_iterator(buffer.targetHints.begin()),
                               std::make_move_iterator(buffer.targetHints.end()));
        }

        auto byUnitId = [](const auto& a, const auto& b) {
            return a.unitId < b.unitId;
        };
        std::sort(mergedVision.begin(), mergedVision.end(), byUnitId);
        std::sort(mergedHints.begin(), mergedHints.end(), byUnitId);
        state.lastVisionIntents = std::move(mergedVision);
        state.lastTargetHints = std::move(mergedHints);
    }

    // Plan stage (Movement): tasks read snapshots only; no world writes here.
    auto moveGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> moveLocals(localCount);

    auto scheduleMove = [&](const UnitSnapshot& unit,
                            const std::vector<AttackableSnapshot>& enemies,
                            const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                            const std::vector<AttackableKey>& forcedKeys,
                            const std::vector<UnitSnapshot>& allies,
                            const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                            const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                            bool baseAlive,
                            const Coord& basePos,
                            const std::unordered_set<std::uint64_t>& occ) {
        if (unit.hp <= 0.f) return;
        runtimeCtx.taskPool.submit([&, unit]() {
            MoveIntent intent = planMoveIntent(unit, dt, state.map, enemies, enemyIndex,
                                               forcedKeys, incomingDamage,
                                               lockedCounts, allies,
                                               baseAlive, basePos, occ);

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= moveLocals.size()) {
                idx = 0;
            }
            moveLocals[idx].moveIntents.push_back(std::move(intent));
        }, moveGroup);
    };

    for (const auto& u : unitsASnap) {
        scheduleMove(u, enemiesASnap, enemyIndexA, forcedAKeys, unitsASnap,
                     incomingDamageA, lockedTargetsA, baseAAlive, baseAPos,
                     occSnap);
    }
    for (const auto& u : unitsBSnap) {
        scheduleMove(u, enemiesBSnap, enemyIndexB, forcedBKeys, unitsBSnap,
                     incomingDamageB, lockedTargetsB, baseBAlive, baseBPos,
                     occSnap);
    }
    moveGroup->wait();

    // Apply stage (Movement): main thread writes only, deterministic order.
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        std::vector<MoveIntent> mergedMoves;
        for (auto& buffer : moveLocals) {
            mergedMoves.insert(mergedMoves.end(),
                               std::make_move_iterator(buffer.moveIntents.begin()),
                               std::make_move_iterator(buffer.moveIntents.end()));
        }

        std::sort(mergedMoves.begin(), mergedMoves.end(),
                  [](const MoveIntent& a, const MoveIntent& b) {
                      return a.unitId < b.unitId;
                  });

        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(state.unitsA.size() + state.unitsB.size());
        for (auto& u : state.unitsA) allUnits.push_back(u);
        for (auto& u : state.unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });

        std::unordered_map<int, std::shared_ptr<Unit>> unitById;
        unitById.reserve(allUnits.size());
        for (auto& u : allUnits) {
            if (u) unitById[u->id] = u;
        }

        for (auto& intent : mergedMoves) {
            auto it = unitById.find(intent.unitId);
            if (it == unitById.end()) continue;
            auto& unit = it->second;
            if (!unit || !unit->behavior) continue;
            IMovementBehavior* movement = unit->behavior->getMovementBehavior();
            if (!movement) continue;
            movement->applyState(std::move(intent.nextState));
            unit->behavior->locomotionState =
                intent.setIdle ? LocomotionState::Idle : LocomotionState::Pathing;
            unit->behavior->moveReason =
                intent.setIdle ? MoveReason::None : intent.reason;
            unit->behavior->commandMoveActive =
                intent.commandMove && !intent.setIdle;
            unit->behavior->retreating = intent.retreating;
            unit->behavior->retreatTimer = intent.retreatTimer;
            unit->behavior->retreatAnchor = intent.retreatAnchor;
            unit->behavior->hasRetreatAnchor = intent.hasRetreatAnchor;
        }

        struct CoordKey {
            int x;
            int y;
        };
        struct CoordLess {
            bool operator()(const CoordKey& a, const CoordKey& b) const {
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
            }
        };

        std::map<CoordKey, std::vector<MoveIntent*>, CoordLess> byTarget;
        for (auto& intent : mergedMoves) {
            if (!intent.hasMove) continue;
            if (!state.map.inBounds(intent.to)) continue;
            if (!state.map.getTile(intent.to).isPassable()) continue;
            CoordKey key{intent.to.x, intent.to.y};
            byTarget[key].push_back(&intent);
        }

        auto movePriority = [](const MoveIntent* a, const MoveIntent* b) {
            if (a->commandMove != b->commandMove) return a->commandMove > b->commandMove;
            return a->unitId < b->unitId;
        };

        std::vector<MoveIntent*> winners;
        winners.reserve(byTarget.size());
        for (auto& entry : byTarget) {
            auto& candidates = entry.second;
            std::sort(candidates.begin(), candidates.end(), movePriority);
            winners.push_back(candidates.front());
        }

        std::sort(winners.begin(), winners.end(),
                  [](const MoveIntent* a, const MoveIntent* b) {
                      return a->unitId < b->unitId;
                  });

        std::unordered_set<int> primaryWinners;
        primaryWinners.reserve(winners.size());
        for (const auto* intent : winners) {
            primaryWinners.insert(intent->unitId);
        }

        std::unordered_set<std::uint64_t> occ;
        occ.reserve(state.unitsA.size() + state.unitsB.size() + 4);
        auto occupyIf = [&](bool ok, const Coord& c) {
            if (ok) occ.insert(packCoord(c));
        };
        occupyIf(state.baseA && !state.baseA->isDestroyed(), state.baseA->getPos());
        occupyIf(state.baseB && !state.baseB->isDestroyed(), state.baseB->getPos());
        for (auto& u : state.unitsA) occupyIf(u && u->isAlive(), u->getPos());
        for (auto& u : state.unitsB) occupyIf(u && u->isAlive(), u->getPos());

        for (const auto& intent : mergedMoves) {
            auto it = unitById.find(intent.unitId);
            if (it == unitById.end()) continue;
            auto& unit = it->second;
            if (!unit || !unit->isAlive()) continue;
            Coord from = unit->getPos();
            if (intent.candidateCount <= 0) continue;

            int startIdx = primaryWinners.count(intent.unitId) ? 0 : 1;
            Coord chosen = from;
            bool hasChoice = false;
            for (int i = startIdx; i < intent.candidateCount; ++i) {
                Coord cand = intent.candidates[static_cast<std::size_t>(i)];
                if (!state.map.inBounds(cand)) continue;
                if (!state.map.getTile(cand).isPassable()) continue;
                if (cand != from && occ.find(packCoord(cand)) != occ.end()) continue;
                chosen = cand;
                hasChoice = true;
                break;
            }
            if (!hasChoice || chosen == from) continue;

            auto kPrev = packCoord(from);
            auto kNow = packCoord(chosen);
            occ.erase(kPrev);
            if (occ.find(kNow) != occ.end()) {
                occ.insert(kPrev);
                continue;
            }
            unit->pos = chosen;
            occ.insert(kNow);
        }

        for (auto& unit : allUnits) {
            if (!unit || !unit->behavior || !unit->isAlive()) continue;
            if (unit->behavior->retreating) {
                unit->behavior->retreatTimer =
                    std::max(0.0f, unit->behavior->retreatTimer - dt);
            } else {
                unit->behavior->retreatTimer = 0.f;
                unit->behavior->hasRetreatAnchor = false;
            }
        }
    }

    // Snapshot stage for Attack planning (post-move/commands).
    std::vector<UnitSnapshot> unitsASnapAtk;
    std::vector<UnitSnapshot> unitsBSnapAtk;
    std::vector<AttackableSnapshot> enemiesASnapAtk;
    std::vector<AttackableSnapshot> enemiesBSnapAtk;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexAAtk;
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash> enemyIndexBAtk;
    std::unordered_set<std::uint64_t> occAtk;
    {
        std::shared_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        occAtk.reserve(state.unitsA.size() + state.unitsB.size() + 2);
        buildUnitSnapshots(state.unitsA, unitsASnapAtk, &occAtk);
        buildUnitSnapshots(state.unitsB, unitsBSnapAtk, &occAtk);
        if (state.baseA && !state.baseA->isDestroyed()) occAtk.insert(packCoord(state.baseA->pos));
        if (state.baseB && !state.baseB->isDestroyed()) occAtk.insert(packCoord(state.baseB->pos));
        buildEnemySnapshots(state.unitsB, state.baseB, enemiesASnapAtk);
        buildEnemySnapshots(state.unitsA, state.baseA, enemiesBSnapAtk);
    }

    std::sort(unitsASnapAtk.begin(), unitsASnapAtk.end(), byUnitId);
    std::sort(unitsBSnapAtk.begin(), unitsBSnapAtk.end(), byUnitId);
    std::sort(enemiesASnapAtk.begin(), enemiesASnapAtk.end(), byAttackableKey);
    std::sort(enemiesBSnapAtk.begin(), enemiesBSnapAtk.end(), byAttackableKey);
    rebuildEnemyIndex(enemiesASnapAtk, enemyIndexAAtk);
    rebuildEnemyIndex(enemiesBSnapAtk, enemyIndexBAtk);

    // Plan stage (Attack): tasks read snapshots only; no world writes here.
    auto attackGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> attackLocals(localCount);

    auto scheduleAttack = [&](const UnitSnapshot& unit,
                              const std::vector<AttackableSnapshot>& enemies,
                              const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
                              const std::vector<AttackableKey>& forcedKeys,
                              const std::vector<UnitSnapshot>& allies,
                              const std::unordered_map<AttackableKey, float, AttackableKeyHash>& incomingDamage,
                              const std::unordered_map<AttackableKey, int, AttackableKeyHash>& lockedCounts,
                              const std::unordered_set<std::uint64_t>& occ) {
        if (unit.hp <= 0.f) return;
        runtimeCtx.taskPool.submit([&, unit]() {
            AttackIntent intent = planAttackIntent(unit, dt, state.map, enemies,
                                                   enemyIndex, forcedKeys,
                                                   incomingDamage, lockedCounts,
                                                   allies, occ);

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= attackLocals.size()) {
                idx = 0;
            }
            attackLocals[idx].attackIntents.push_back(std::move(intent));
        }, attackGroup);
    };

    for (const auto& u : unitsASnapAtk) {
        scheduleAttack(u, enemiesASnapAtk, enemyIndexAAtk, forcedAKeys, unitsASnapAtk,
                       incomingDamageA, lockedTargetsA, occAtk);
    }
    for (const auto& u : unitsBSnapAtk) {
        scheduleAttack(u, enemiesBSnapAtk, enemyIndexBAtk, forcedBKeys, unitsBSnapAtk,
                       incomingDamageB, lockedTargetsB, occAtk);
    }
    attackGroup->wait();

    // Apply stage (Attack): main thread writes only, deterministic order.
    {
        std::unique_lock<std::shared_mutex> lock(runtimeCtx.worldMutex);
        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(state.unitsA.size() + state.unitsB.size());
        for (auto& u : state.unitsA) allUnits.push_back(u);
        for (auto& u : state.unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });

        std::vector<std::weak_ptr<IAttackable>> forcedAAtk;
        std::vector<std::weak_ptr<IAttackable>> forcedBAtk;
        forcedAAtk.reserve(state.forcedVisibleForA.size());
        forcedBAtk.reserve(state.forcedVisibleForB.size());
        for (const auto& r : state.forcedVisibleForA) {
            if (!r.target.expired()) forcedAAtk.push_back(r.target);
        }
        for (const auto& r : state.forcedVisibleForB) {
            if (!r.target.expired()) forcedBAtk.push_back(r.target);
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior) continue;
            const auto& enemies = (u->getFaction() == Faction::A) ? state.enemiesA : state.enemiesB;
            const auto& forced = (u->getFaction() == Faction::A) ? forcedAAtk : forcedBAtk;
            u->behavior->updateVision(*u, state.map, enemies, forced);
        }

        std::vector<AttackIntent> mergedAttacks;
        for (auto& buffer : attackLocals) {
            mergedAttacks.insert(mergedAttacks.end(),
                                 std::make_move_iterator(buffer.attackIntents.begin()),
                                 std::make_move_iterator(buffer.attackIntents.end()));
        }

        std::sort(mergedAttacks.begin(), mergedAttacks.end(),
                  [](const AttackIntent& a, const AttackIntent& b) {
                      return a.attackerId < b.attackerId;
                  });

        auto resolveTarget = [&](int id, AttackableType type) -> std::shared_ptr<IAttackable> {
            if (id < 0) return nullptr;
            if (type == AttackableType::BASE) {
                auto base = data.findBase(id, Faction::A);
                if (!base) base = data.findBase(id, Faction::B);
                return base;
            }
            return data.findUnit(id);
        };

        std::map<std::pair<int, int>, float> damageByTarget;
        std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageAFrame;
        std::unordered_map<AttackableKey, float, AttackableKeyHash> incomingDamageBFrame;
        std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsAFrame;
        std::unordered_map<AttackableKey, int, AttackableKeyHash> lockedTargetsBFrame;
        std::vector<int> attackersToReveal;
        std::vector<int> attackersDealtDamage;
        std::vector<int> damagedUnits;
        attackersToReveal.reserve(mergedAttacks.size());
        attackersDealtDamage.reserve(mergedAttacks.size());

        for (const auto& intent : mergedAttacks) {
            auto attacker = data.findUnit(intent.attackerId);
            if (!attacker || !attacker->behavior) continue;

            IAttackBehavior* attackBehavior = attacker->behavior->getAttackBehavior();
            if (!attackBehavior) continue;

            attackBehavior->setCooldown(intent.nextCooldown);
            if (intent.nextTargetId >= 0) {
                auto target = resolveTarget(intent.nextTargetId, intent.nextTargetType);
                attackBehavior->setTarget(std::weak_ptr<IAttackable>(target));
            } else {
                attackBehavior->setTarget(std::weak_ptr<IAttackable>{});
            }

            if (intent.nextTargetId >= 0) {
                AttackableKey key;
                key.id = intent.nextTargetId;
                key.type = intent.nextTargetType;
                if (attacker->getFaction() == Faction::A) {
                    lockedTargetsAFrame[key] += 1;
                } else {
                    lockedTargetsBFrame[key] += 1;
                }
                attacker->behavior->combatState = CombatState::Engaging;
                attacker->behavior->combatAction = intent.action;
            } else {
                attacker->behavior->combatState = CombatState::None;
                attacker->behavior->combatAction = CombatAction::None;
            }
            attacker->behavior->commitTimer = intent.nextCommitTimer;
            attacker->behavior->commitTargetId = intent.nextCommitTargetId;
            attacker->behavior->commitTargetType = intent.nextCommitTargetType;

            if (intent.didAttack && intent.targetId >= 0) {
                auto key = std::make_pair(static_cast<int>(intent.targetType), intent.targetId);
                damageByTarget[key] += intent.damage;
                attackersToReveal.push_back(intent.attackerId);
                AttackableKey dmgKey;
                dmgKey.id = intent.targetId;
                dmgKey.type = intent.targetType;
                if (attacker->getFaction() == Faction::A) {
                    incomingDamageAFrame[dmgKey] += intent.damage;
                } else {
                    incomingDamageBFrame[dmgKey] += intent.damage;
                }
                auto damageTarget = resolveTarget(intent.targetId, intent.targetType);
                if (damageTarget) {
                    attackersDealtDamage.push_back(intent.attackerId);
                    if (intent.targetType == AttackableType::UNIT) {
                        damagedUnits.push_back(intent.targetId);
                    }
                }
            }
        }

        for (const auto& entry : damageByTarget) {
            AttackableType type = static_cast<AttackableType>(entry.first.first);
            int targetId = entry.first.second;
            auto target = resolveTarget(targetId, type);
            if (target) {
                target->takeDamage(entry.second);
            }
        }

        std::sort(attackersToReveal.begin(), attackersToReveal.end());
        attackersToReveal.erase(
            std::unique(attackersToReveal.begin(), attackersToReveal.end()),
            attackersToReveal.end()
        );
        for (int attackerId : attackersToReveal) {
            auto attacker = data.findUnit(attackerId);
            if (attacker) {
                data.revealAttacker(*attacker);
            }
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior) continue;
            const auto& enemies = (u->getFaction() == Faction::A) ? state.enemiesA : state.enemiesB;
            u->behavior->postAttackStateUpdate(*u, state.map, enemies);
        }

        std::sort(attackersDealtDamage.begin(), attackersDealtDamage.end());
        attackersDealtDamage.erase(
            std::unique(attackersDealtDamage.begin(), attackersDealtDamage.end()),
            attackersDealtDamage.end()
        );
        std::sort(damagedUnits.begin(), damagedUnits.end());
        damagedUnits.erase(
            std::unique(damagedUnits.begin(), damagedUnits.end()),
            damagedUnits.end()
        );

        for (auto& u : allUnits) {
            if (!u || !u->behavior || !u->isAlive()) continue;
            u->behavior->timeSinceDamaged += dt;
            u->behavior->timeSinceDealtDamage += dt;

            if (std::binary_search(damagedUnits.begin(), damagedUnits.end(), u->id)) {
                u->behavior->timeSinceDamaged = 0.f;
            }
            if (std::binary_search(attackersDealtDamage.begin(), attackersDealtDamage.end(), u->id)) {
                u->behavior->timeSinceDealtDamage = 0.f;
            }

            if (u->behavior->timeSinceDamaged > kOocParams.delay &&
                u->behavior->timeSinceDealtDamage > kOocParams.delay) {
                const auto& enemyUnits = (u->getFaction() == Faction::A) ? state.unitsB : state.unitsA;
                float threat = computeLocalThreatWorld(*u, enemyUnits, kThreatRadius);
                if (threat < kOocParams.threatThreshold) {
                    float regen = u->baseStats.maxHP * kOocParams.regenRate * dt;
                    u->hp = std::min(u->baseStats.maxHP, u->hp + regen);
                }
            }
        }

        state.lastIncomingDamageA = std::move(incomingDamageAFrame);
        state.lastIncomingDamageB = std::move(incomingDamageBFrame);
        state.lastLockedTargetsA = std::move(lockedTargetsAFrame);
        state.lastLockedTargetsB = std::move(lockedTargetsBFrame);

        systems.cleanup.update(data);

        // 清理选中列表中已不存在的单位
        std::unordered_set<int> alive;
        for (auto& u : state.unitsA) if (u && u->isAlive()) alive.insert(u->id);
        for (auto& u : state.unitsB) if (u && u->isAlive()) alive.insert(u->id);
        controlCtx.selectedUnitIds.erase(
            std::remove_if(controlCtx.selectedUnitIds.begin(), controlCtx.selectedUnitIds.end(),
                [&](int id){ return alive.find(id) == alive.end(); }),
            controlCtx.selectedUnitIds.end()
        );
    }
}

void GameWorld::appendUnitSnapshot(
    const std::shared_ptr<Unit>& u,
    std::vector<AiScoring::UnitSnapshot>& out,
    std::unordered_set<std::uint64_t>* occ) {
    if (!u || !u->isAlive()) return;

    AiScoring::UnitSnapshot snap;
    snap.id = u->id;
    snap.type = u->type;
    snap.faction = u->owner;
    snap.pos = u->pos;
    snap.hp = u->hp;
    snap.maxHp = u->baseStats.maxHP;
    snap.stats = u->baseStats;

    if (u->behavior) {
        if (auto* attack = u->behavior->getAttackBehavior()) {
            snap.cooldown = attack->getCooldown();
            snap.currentTarget = attackableKey(attack->getTarget().lock());
        }

        snap.commitTimer = u->behavior->commitTimer;
        snap.commitTarget.id = u->behavior->commitTargetId;
        snap.commitTarget.type = u->behavior->commitTargetType;

        snap.commandAttackActive = u->behavior->commandAttackActive;
        snap.commandTarget.id = u->behavior->commandAttackTargetId;
        snap.commandTarget.type = u->behavior->commandAttackTargetType;

        snap.retreating = u->behavior->retreating;
        snap.retreatTimer = u->behavior->retreatTimer;
        snap.retreatAnchor = u->behavior->retreatAnchor;
        snap.hasRetreatAnchor = u->behavior->hasRetreatAnchor;

        snap.timeSinceDamaged = u->behavior->timeSinceDamaged;
        snap.timeSinceDealtDamage = u->behavior->timeSinceDealtDamage;

        snap.commandMoveActive = u->behavior->commandMoveActive;
        snap.locomotionState = u->behavior->locomotionState;
        snap.combatState = u->behavior->combatState;
        snap.moveReason = u->behavior->moveReason;

        if (auto* movement = u->behavior->getMovementBehavior()) {
            snap.movementState = movement->snapshot();
        }
    }

    out.push_back(std::move(snap));
    if (occ) occ->insert(packCoord(u->pos));
}

void GameWorld::buildUnitSnapshots(
    const std::vector<std::shared_ptr<Unit>>& units,
    std::vector<AiScoring::UnitSnapshot>& out,
    std::unordered_set<std::uint64_t>* occ) {
    for (const auto& u : units) {
        appendUnitSnapshot(u, out, occ);
    }
}

void GameWorld::buildEnemySnapshots(
    const std::vector<std::shared_ptr<Unit>>& units,
    const std::shared_ptr<Base>& base,
    std::vector<AiScoring::AttackableSnapshot>& out) {
    if (base && !base->isDestroyed()) {
        AiScoring::AttackableSnapshot snap;
        snap.key.id = base->id;
        snap.key.type = AttackableType::BASE;
        snap.type = AttackableType::BASE;
        snap.faction = base->faction;
        snap.unitType = UnitType::Infantry;
        snap.pos = base->pos;
        snap.hp = base->hp;
        snap.maxHp = base->maxHp;
        snap.attack = 0.f;
        snap.attackRange = 0.f;
        snap.armor = 0.f;
        snap.isBase = true;
        out.push_back(std::move(snap));
    }

    for (const auto& u : units) {
        if (!u || !u->isAlive()) continue;
        AiScoring::AttackableSnapshot snap;
        snap.key.id = u->id;
        snap.key.type = AttackableType::UNIT;
        snap.type = AttackableType::UNIT;
        snap.faction = u->owner;
        snap.unitType = u->type;
        snap.pos = u->pos;
        snap.hp = u->hp;
        snap.maxHp = u->baseStats.maxHP;
        snap.attack = u->baseStats.attack;
        snap.attackRange = u->baseStats.attackRange;
        snap.armor = u->baseStats.armor;
        snap.isBase = false;
        out.push_back(std::move(snap));
    }
}

void GameWorld::rebuildEnemyIndex(
    const std::vector<AiScoring::AttackableSnapshot>& snaps,
    std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& index) {
    index.clear();
    index.reserve(snaps.size());
    for (std::size_t i = 0; i < snaps.size(); ++i) {
        index[snaps[i].key] = i;
    }
}

void GameWorld::buildForcedKeys(
    const std::vector<ForcedReveal>& forced,
    std::vector<AttackableKey>& out) {
    for (const auto& r : forced) {
        auto target = r.target.lock();
        if (!target) continue;
        out.push_back(attackableKey(target));
    }
}


bool WorldDataContext::isTileFree(const Coord& c) const {
    for (auto& u : unitsA) {
        if (u->isAlive() && u->getPos() == c) return false;
    }

    for (auto& u : unitsB) {
        if (u->isAlive() && u->getPos() == c) return false;
    }

    if (baseA && !baseA->isDestroyed() && baseA->getPos() == c)
        return false;
    if (baseB && !baseB->isDestroyed() && baseB->getPos() == c)
        return false;

    return true;
}

void WorldDataContext::rebuildEnemies() {
    enemiesA.clear();
    enemiesB.clear();

    if (baseB) {
        enemiesA.emplace_back(baseB);
    }
    for (auto& u : unitsB) {
        enemiesA.emplace_back(u);
    }

    if (baseA) {
        enemiesB.emplace_back(baseA);
    }
    for (auto& u : unitsA) {
        enemiesB.emplace_back(u);
    }
}

std::shared_ptr<Unit> WorldDataContext::findUnit(int id) const {
    if (id < 0) return nullptr;
    for (auto& u : unitsA) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    for (auto& u : unitsB) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    return nullptr;
}

std::shared_ptr<Base> WorldDataContext::findBase(int id, Faction fac) const {
    if (baseA && !baseA->isDestroyed() &&
        ((id >= 0 && baseA->getId() == id) || (id < 0 && fac == Faction::A))) {
        return baseA;
    }
    if (baseB && !baseB->isDestroyed() &&
        ((id >= 0 && baseB->getId() == id) || (id < 0 && fac == Faction::B))) {
        return baseB;
    }
    return nullptr;
}

std::shared_ptr<IAttackable> WorldDataContext::findAttackable(int id) const {
    if (id < 0) return nullptr;
    auto u = findUnit(id);
    if (u) return u;

    if (baseA && baseA->getId() == id && !baseA->isDestroyed()) return baseA;
    if (baseB && baseB->getId() == id && !baseB->isDestroyed()) return baseB;
    return nullptr;
}

int WorldDataContext::registerUnit(const std::shared_ptr<Unit>& u) {
    if (!u) return -1;
    u->id = nextUnitId++;
    return u->id;
}

int WorldDataContext::registerBase(const std::shared_ptr<Base>& b) {
    if (!b) return -1;
    b->setId(nextBaseId++);
    return b->getId();
}

void WorldDataContext::addForcedReveal(Faction viewer,
                                       const std::shared_ptr<IAttackable>& target,
                                       float durationSeconds) {
    if (!target) return;
    auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    bucket.push_back({target, durationSeconds});
}

void WorldDataContext::decayForcedReveals(float dt) {
    auto decay = [&](std::vector<ForcedReveal>& arr) {
        for (auto& r : arr) r.timeLeft -= dt;
        arr.erase(std::remove_if(arr.begin(), arr.end(),
                                 [](const ForcedReveal& r) {
                                     return r.timeLeft <= 0.f || r.target.expired();
                                 }),
                  arr.end());
    };
    decay(forcedVisibleForA);
    decay(forcedVisibleForB);
}

void WorldDataContext::appendForcedReveals(Faction viewer,
                                           std::vector<std::weak_ptr<IAttackable>>& out) const {
    const auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    for (const auto& r : bucket) {
        if (!r.target.expired()) {
            out.push_back(r.target);
        }
    }
}

void WorldDataContext::revealAttacker(const Unit& u) {
    auto attacker = findUnit(u.id);
    if (!attacker) return;
    Faction viewer = (u.owner == Faction::A) ? Faction::B : Faction::A;
    addForcedReveal(viewer, attacker, 2.0f);
}

bool WorldRuntimeContext::shouldQuit() const {
    return quitRequested.load();
}

void WorldRuntimeContext::requestQuit() {
    quitRequested.store(true);
}

bool WorldRuntimeContext::isRenderRunning() const {
    return renderRunning.load();
}

bool WorldRuntimeContext::isPaused() const {
    return paused.load();
}

void WorldRuntimeContext::pause() {
    paused.store(true);
}

void WorldRuntimeContext::resume() {
    paused.store(false);
}

void WorldRuntimeContext::togglePause() {
    if (paused.load()) {
        resume();
    } else {
        pause();
    }
}

void WorldRuntimeContext::markGameOver() {
    gameEnded.store(true);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    gameEndTimestampMs.store(nowMs);
    pause();
}

bool WorldRuntimeContext::hasGameEnded() const {
    return gameEnded.load();
}

long long WorldRuntimeContext::gameEndMs() const {
    return gameEndTimestampMs.load();
}

void WorldRuntimeContext::enqueueUiEvent(std::optional<std::string> input,
                                         std::optional<std::string> feedback) {
    WorldRuntime::UiEvent evt;
    evt.input = std::move(input);
    evt.feedback = std::move(feedback);
    {
        std::lock_guard<std::mutex> lock(uiEventMutex);
        uiEvents.push(std::move(evt));
    }
}

void WorldRuntimeContext::drainUiEvents(WorldControlContext& control) {
    std::queue<WorldRuntime::UiEvent> local;
    {
        std::lock_guard<std::mutex> lock(uiEventMutex);
        std::swap(local, uiEvents);
    }
    while (!local.empty()) {
        const WorldRuntime::UiEvent& evt = local.front();
        if (evt.input.has_value()) {
            control.lastCommandInput = *evt.input;
        }
        if (evt.feedback.has_value()) {
            control.lastCommandFeedback = *evt.feedback;
        }
        local.pop();
    }
}

void WorldControlContext::enqueueCommand(const std::string& line) {
    commandQueue.push(line);
}

void WorldControlContext::processCommands(WorldDataContext& data, WorldRuntimeContext& runtime) {
    std::string line;
    while (commandQueue.tryPop(line)) {
        lastCommandInput = line;
        Command parsed;
        std::string err;
        if (!parseCommand(line, parsed, err)) {
            lastCommandFeedback = "ERR: " + err;
            continue;
        }
        CommandResult r = executeCommand(parsed, data, *this, runtime);
        if (r.ok) {
            lastCommandFeedback = r.normalized + " -> " + r.message;
        } else {
            lastCommandFeedback = "ERR: " + r.message;
        }
    }
}

void WorldControlContext::setSelection(const std::vector<int>& ids) {
    selectedUnitIds = ids;
}

void WorldControlContext::clearSelection() {
    selectedUnitIds.clear();
}

std::optional<UnitType> WorldControlContext::unitTypeFromCode(int code) const {
    if (code < 0) return std::nullopt;
    // Extend this list in order (1,2,3,...) to add more unit codes.
    static const std::vector<UnitType> mapping = {
        UnitType::Infantry,
        UnitType::Archer,
        UnitType::Knight
    };

    if (code >= 1 && code <= static_cast<int>(mapping.size())) {
        return mapping[static_cast<std::size_t>(code - 1)];
    }
    return std::nullopt;
}

void WorldControlContext::beginProductionChoice(const std::shared_ptr<Base>& base,
                                                WorldRuntimeContext& runtime) {
    if (!base) return;
    awaitingProductionChoice = true;
    productionChoiceBase = base;
    productionInputBuffer.clear();
    runtime.pause();
    runtime.enqueueUiEvent("production choice",
                           "Enter unit code then press Enter; Esc to cancel");
}

bool WorldControlContext::handleProductionDigit(char digit, WorldRuntimeContext& runtime) {
    if (!std::isdigit(static_cast<unsigned char>(digit))) return false;
    productionInputBuffer.push_back(digit);
    runtime.enqueueUiEvent(std::nullopt,
                           "Production code: " + productionInputBuffer + " \n\t(Enter to confirm)");
    return true;
}

bool WorldControlContext::handleProductionBackspace(WorldRuntimeContext& runtime) {
    if (productionInputBuffer.empty()) return false;
    productionInputBuffer.pop_back();
    runtime.enqueueUiEvent(std::nullopt,
                           productionInputBuffer.empty()
                               ? "Production code cleared"
                               : "Production code: " + productionInputBuffer);
    return true;
}

bool WorldControlContext::commitProductionChoice(WorldRuntimeContext& runtime) {
    auto basePtr = productionChoiceBase.lock();
    if (!basePtr) {
        cancelProductionChoice(runtime);
        return false;
    }

    if (productionInputBuffer.empty()) {
        runtime.enqueueUiEvent(std::nullopt, "Please enter a numeric code");
        return false;
    }

    auto ignoreInvalid = [&](const std::string& msg) {
        awaitingProductionChoice = false;
        productionChoiceBase.reset();
        productionInputBuffer.clear();
        runtime.resume();
        runtime.enqueueUiEvent(std::nullopt, msg);
    };

    int code = -1;
    try {
        code = std::stoi(productionInputBuffer);
    } catch (...) {
        ignoreInvalid("Invalid code, ignored");
        return false;
    }

    auto type = unitTypeFromCode(code);
    if (!type.has_value()) {
        ignoreInvalid("Unsupported code ignored: " + productionInputBuffer);
        return false;
    }

    basePtr->issueProduce(*type);
    runtime.enqueueUiEvent(std::nullopt,
                           "Queued " +
                               std::string(*type == UnitType::Infantry ? "Infantry" :
                                           *type == UnitType::Archer   ? "Archer"   : "Knight"));
    awaitingProductionChoice = false;
    productionChoiceBase.reset();
    productionInputBuffer.clear();
    runtime.resume();
    return true;
}

void WorldControlContext::cancelProductionChoice(WorldRuntimeContext& runtime) {
    awaitingProductionChoice = false;
    productionChoiceBase.reset();
    productionInputBuffer.clear();
    runtime.enqueueUiEvent(std::nullopt, "Production canceled");
    runtime.resume();
}

void WorldControlContext::resetTargeting() {
    pendingTarget.reset();
    controlMode = ControlState::ControlMode::Idle;
}

void WorldControlContext::enterTargeting() {
    pendingTarget.reset();
    controlMode = ControlState::ControlMode::Targeting;
}

void WorldControlContext::cancelTargeting() {
    resetTargeting();
}

void WorldControlContext::commitTargeting() {
    resetTargeting();
}
