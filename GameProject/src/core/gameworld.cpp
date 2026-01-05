#include "core/Iattackable.hpp"
#include "core/gameworld.hpp"
#include "core/render_config.hpp"
#include <chrono>
#include <algorithm>
#include <iostream>
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

    int attackableId(const std::shared_ptr<IAttackable>& target) {
        if (!target) return -1;
        if (target->getAttackType() == AttackableType::BASE) {
            auto basePtr = std::dynamic_pointer_cast<Base>(target);
            return basePtr ? basePtr->getId() : -1;
        }
        auto unitPtr = std::dynamic_pointer_cast<Unit>(target);
        return unitPtr ? unitPtr->getId() : -1;
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

    struct UnitSnapshot {
        int id = -1;
        UnitType type = UnitType::Infantry;
        Faction faction = Faction::A;
        Coord pos{};
        float hp = 0.f;
        float maxHp = 0.f;
        UnitStats stats{};
        float cooldown = 0.f;
        AttackableKey currentTarget{};
        float commitTimer = 0.f;
        AttackableKey commitTarget{};
        bool commandAttackActive = false;
        AttackableKey commandTarget{};
        bool retreating = false;
        float retreatTimer = 0.f;
        Coord retreatAnchor{};
        bool hasRetreatAnchor = false;
        float timeSinceDamaged = 0.f;
        float timeSinceDealtDamage = 0.f;
        bool commandMoveActive = false;
        LocomotionState locomotionState = LocomotionState::Idle;
        CombatState combatState = CombatState::None;
        MoveReason moveReason = MoveReason::None;
        IMovementBehavior::MovementState movementState;
    };

    struct AttackableSnapshot {
        AttackableKey key{};
        AttackableType type = AttackableType::UNIT;
        Faction faction = Faction::A;
        UnitType unitType = UnitType::Infantry;
        Coord pos{};
        float hp = 0.f;
        float maxHp = 0.f;
        float attack = 0.f;
        float attackRange = 0.f;
        float armor = 0.f;
        bool isBase = false;
    };

    struct TargetScoreParams {
        float distanceWeight = 1.2f;
        float threatWeight = 1.0f;
        float ttkWeight = 1.4f;
        float executeWeight = 0.9f;
        float preferenceWeight = 1.2f;
        float overkillWeight = 1.1f;
        float lockWeight = 0.6f;
        float baseWeight = 0.8f;
        float baseThreatPenalty = 1.2f;
        float baseFinishBonus = 0.7f;
        float commitBonus = 0.8f;
        float commitDuration = 0.6f;
        float retreatHpFrac = 0.35f;
        float threatRetreat = 32.f;
        float kiteRangeMargin = 0.8f;
        float kiteExtraDist = 1.5f;
        float siegeOpportunity = 1.25f;
        float siegeHoldTime = 0.8f;
        float losPenalty = 1.2f;
        float unreachablePenalty = 1.8f;
        float availabilityWeight = 0.9f;
        int availabilityCap = 8;
    };

    struct ActionParams {
        float retreatHpFrac = 0.35f;
        float threatRetreat = 32.f;
        float kiteMargin = 0.8f;
        float kiteExtraDist = 1.5f;
        float siegeOpportunity = 1.25f;
    };

    struct RetreatParams {
        float hpEnter = 0.35f;
        float hpExit = 0.55f;
        float threatEnter = 32.f;
        float threatExit = 22.f;
        float retreatMinTime = 0.8f;
        int maxRetreatDist = 12;
        int sampleRadius = 6;
        float retreatFireThreat = 24.f;
    };

    struct OocParams {
        float delay = 2.0f;
        float regenRate = 0.08f;
        float threatThreshold = 12.f;
    };

    const TargetScoreParams kTargetParams{};
    const ActionParams kActionParams{};
    const RetreatParams kRetreatParams{};
    const OocParams kOocParams{};
    constexpr float kThreatRadius = 6.0f;

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

    struct ReachableGrid {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> reachable;

        bool isReachable(const Coord& c) const {
            if (c.x < 0 || c.x >= width || c.y < 0 || c.y >= height) return false;
            std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
            return reachable[idx] != 0;
        }
    };

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

    struct TargetChoice {
        const AttackableSnapshot* target = nullptr;
        float score = -1e30f;
        float baseOpportunity = 0.f;
    };

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
            float dist = self.pos.mhtDistanceTo(v->pos);
            float score = 0.f;
            score += params.distanceWeight * distanceScore(dist);
            score += params.threatWeight * threatScore(*v);
            score += params.ttkWeight * ttkScore(self, *v);
            float hpFrac = (v->maxHp > 0.f) ? (v->hp / v->maxHp) : 1.0f;
            score += params.executeWeight * (1.0f - hpFrac);

            if (map.hasMountainBetween(self.pos, v->pos)) {
                score -= params.losPenalty;
            }
            float availability = firingPositionAvailability(self, *v, map, reachable,
                                                             occ, params.availabilityCap);
            score += params.availabilityWeight * availability;
            if (availability <= 0.f) {
                score -= params.unreachablePenalty;
            }

            if (v->isBase) {
                float opp = baseOpportunityScore(self, *v, allies, visible, params, localThreat);
                score += params.baseWeight * basePreference(self.type) * opp;
                score -= params.baseThreatPenalty * (localThreat / params.threatRetreat);
            } else {
                score += params.preferenceWeight * unitPreference(self.type, v->unitType);
            }

            auto incIt = incomingDamage.find(v->key);
            if (incIt != incomingDamage.end() && v->maxHp > 0.f) {
                score -= params.overkillWeight * (incIt->second / v->maxHp);
            }
            auto lockIt = lockedCounts.find(v->key);
            if (lockIt != lockedCounts.end()) {
                score -= params.lockWeight * static_cast<float>(lockIt->second);
            }

            if (v->key == self.commitTarget) {
                score += params.commitBonus;
            }
            if (v->key == self.currentTarget) {
                score += params.commitBonus * 0.5f;
            }

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
                best.baseOpportunity = v->isBase
                    ? baseOpportunityScore(self, *v, allies, visible, params, localThreat)
                    : 0.f;
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
        if (unit.commandAttackActive && unit.commandTarget.id >= 0) {
            auto it = enemyIndex.find(unit.commandTarget);
            if (it != enemyIndex.end()) {
                target = &enemies[it->second];
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
                    desiredTarget = anchor;
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
                    desiredTarget = anchor;
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
                int steps = static_cast<int>(std::ceil(kActionParams.kiteExtraDist));
                Coord awayGoal{unit.pos.x + (unit.pos.x - target->pos.x),
                               unit.pos.y + (unit.pos.y - target->pos.y)};
                desiredTarget = stepAlongManhattan(unit.pos, awayGoal, steps);
                hasDesiredTarget = true;
                reason = MoveReason::Kite;
            } else if (!retreating &&
                       (action == CombatAction::Chase || action == CombatAction::Siege) && target) {
                int desiredDist = 0;
                if (unit.stats.attackRange > 2.5f) {
                    desiredDist = std::max(1, static_cast<int>(std::floor(
                        unit.stats.attackRange - kActionParams.kiteMargin)));
                }
                if (dist > desiredDist) {
                    desiredTarget = stepAlongManhattan(target->pos, unit.pos, desiredDist);
                    hasDesiredTarget = true;
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
                rebuildPathState(unit, map, desiredTarget, nextState);
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
        intent.nextState = std::move(nextState);
        return intent;
    }
} 

GameWorld::GameWorld()
    : map(MAP_WIDTH, MAP_HEIGHT), baseSystem()
    , renderRunning(false), taskPool(4)
{
    MapGenerator gen(MAP_WIDTH, MAP_HEIGHT);

    bool ok = false;
    Coord baseAPos;
    Coord baseBPos;

    while (!ok) {
        Map candidate = gen.generate();

        if (pickBasePositions(candidate, baseAPos, baseBPos)) {
            map = std::move(candidate);
            ok = true;
        }
    }
    baseA = std::make_shared<Base>(baseAPos, Faction::A);
    baseB = std::make_shared<Base>(baseBPos, Faction::B);
    registerBase(baseA);
    registerBase(baseB);

    enemiesA.clear();
    enemiesB.clear();

    
    taskPool.init();
    renderSystem = std::make_unique<RenderSystem>();
}


GameWorld::~GameWorld() {
    stopRenderThread();
}

void GameWorld::update(float dt) {
    static int tick = 0;
    ++tick;
    // std::cout << "\n[World] Tick " << tick << "\n";

    // std::cout << "  dt=" << dt << "\n";

    drainUiEvents();
    {
        std::unique_lock<std::shared_mutex> lock(worldMutex);
        processCommands();
        if (paused.load() || gameEnded.load()) {
            return;
        }

        decayForcedReveals(dt);
        // std::cout << "  BaseSystem...\n";
        baseSystem.update(*this, dt);
        rebuildEnemies();

        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(unitsA.size() + unitsB.size());
        for (auto& u : unitsA) allUnits.push_back(u);
        for (auto& u : unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });
        for (auto& u : allUnits) {
            if (u && u->behavior) {
                u->behavior->applyPendingCommand(*u, map);
            }
        }
    }

    const std::size_t localCount = std::max<std::size_t>(1, taskPool.workerCount());

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
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        incomingDamageA = lastIncomingDamageA;
        incomingDamageB = lastIncomingDamageB;
        lockedTargetsA = lastLockedTargetsA;
        lockedTargetsB = lastLockedTargetsB;

        baseAAlive = baseA && !baseA->isDestroyed();
        baseBAlive = baseB && !baseB->isDestroyed();
        if (baseAAlive) baseAPos = baseA->pos;
        if (baseBAlive) baseBPos = baseB->pos;

        occSnap.reserve(unitsA.size() + unitsB.size() + 2);
        unitsASnap.reserve(unitsA.size());
        unitsBSnap.reserve(unitsB.size());
        for (const auto& u : unitsA) {
            if (!u || !u->isAlive()) continue;
            occSnap.insert(packCoord(u->pos));
            UnitSnapshot snap;
            snap.id = u->id;
            snap.type = u->type;
            snap.faction = u->owner;
            snap.pos = u->pos;
            snap.hp = u->hp;
            snap.maxHp = u->baseStats.maxHP;
            snap.stats = u->baseStats;
            if (u->behavior) {
                if (auto* attackBehavior = u->behavior->getAttackBehavior()) {
                    snap.cooldown = attackBehavior->getCooldown();
                    snap.currentTarget = attackableKey(attackBehavior->getTarget().lock());
                }
                if (auto* moveBehavior = u->behavior->getMovementBehavior()) {
                    snap.movementState = moveBehavior->snapshot();
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
            }
            unitsASnap.push_back(std::move(snap));
        }
        for (const auto& u : unitsB) {
            if (!u || !u->isAlive()) continue;
            occSnap.insert(packCoord(u->pos));
            UnitSnapshot snap;
            snap.id = u->id;
            snap.type = u->type;
            snap.faction = u->owner;
            snap.pos = u->pos;
            snap.hp = u->hp;
            snap.maxHp = u->baseStats.maxHP;
            snap.stats = u->baseStats;
            if (u->behavior) {
                if (auto* attackBehavior = u->behavior->getAttackBehavior()) {
                    snap.cooldown = attackBehavior->getCooldown();
                    snap.currentTarget = attackableKey(attackBehavior->getTarget().lock());
                }
                if (auto* moveBehavior = u->behavior->getMovementBehavior()) {
                    snap.movementState = moveBehavior->snapshot();
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
            }
            unitsBSnap.push_back(std::move(snap));
        }
        if (baseAAlive) occSnap.insert(packCoord(baseAPos));
        if (baseBAlive) occSnap.insert(packCoord(baseBPos));

        enemiesASnap.reserve(unitsB.size() + 1);
        for (const auto& u : unitsB) {
            if (!u || !u->isAlive()) continue;
            AttackableSnapshot snap;
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
            enemyIndexA[snap.key] = enemiesASnap.size();
            enemiesASnap.push_back(std::move(snap));
        }
        if (baseB && !baseB->isDestroyed()) {
            AttackableSnapshot snap;
            snap.key.id = baseB->id;
            snap.key.type = AttackableType::BASE;
            snap.type = AttackableType::BASE;
            snap.faction = baseB->faction;
            snap.unitType = UnitType::Infantry;
            snap.pos = baseB->pos;
            snap.hp = baseB->hp;
            snap.maxHp = baseB->maxHp;
            snap.isBase = true;
            enemyIndexA[snap.key] = enemiesASnap.size();
            enemiesASnap.push_back(std::move(snap));
        }

        enemiesBSnap.reserve(unitsA.size() + 1);
        for (const auto& u : unitsA) {
            if (!u || !u->isAlive()) continue;
            AttackableSnapshot snap;
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
            enemyIndexB[snap.key] = enemiesBSnap.size();
            enemiesBSnap.push_back(std::move(snap));
        }
        if (baseA && !baseA->isDestroyed()) {
            AttackableSnapshot snap;
            snap.key.id = baseA->id;
            snap.key.type = AttackableType::BASE;
            snap.type = AttackableType::BASE;
            snap.faction = baseA->faction;
            snap.unitType = UnitType::Infantry;
            snap.pos = baseA->pos;
            snap.hp = baseA->hp;
            snap.maxHp = baseA->maxHp;
            snap.isBase = true;
            enemyIndexB[snap.key] = enemiesBSnap.size();
            enemiesBSnap.push_back(std::move(snap));
        }

        forcedAKeys.reserve(forcedVisibleForA.size());
        for (const auto& r : forcedVisibleForA) {
            auto target = r.target.lock();
            AttackableKey key = attackableKey(target);
            if (key.id >= 0) forcedAKeys.push_back(key);
        }
        forcedBKeys.reserve(forcedVisibleForB.size());
        for (const auto& r : forcedVisibleForB) {
            auto target = r.target.lock();
            AttackableKey key = attackableKey(target);
            if (key.id >= 0) forcedBKeys.push_back(key);
        }
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
    enemyIndexA.clear();
    for (std::size_t i = 0; i < enemiesASnap.size(); ++i) {
        enemyIndexA[enemiesASnap[i].key] = i;
    }
    enemyIndexB.clear();
    for (std::size_t i = 0; i < enemiesBSnap.size(); ++i) {
        enemyIndexB[enemiesBSnap[i].key] = i;
    }

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
        taskPool.submit([&, unit]() {
            std::vector<const AttackableSnapshot*> visible =
                collectVisibleEnemies(unit, map, enemies, enemyIndex, forcedKeys);
            ReachableGrid reachable = buildReachableGrid(map, unit.pos);

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
                                               kTargetParams, map, reachable,
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
        std::unique_lock<std::shared_mutex> lock(worldMutex);
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
        lastVisionIntents = std::move(mergedVision);
        lastTargetHints = std::move(mergedHints);
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
        taskPool.submit([&, unit]() {
            MoveIntent intent = planMoveIntent(unit, dt, map, enemies, enemyIndex,
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
        std::unique_lock<std::shared_mutex> lock(worldMutex);
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
        allUnits.reserve(unitsA.size() + unitsB.size());
        for (auto& u : unitsA) allUnits.push_back(u);
        for (auto& u : unitsB) allUnits.push_back(u);
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
            if (!map.inBounds(intent.to)) continue;
            if (!map.getTile(intent.to).isPassable()) continue;
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

        std::unordered_set<std::uint64_t> occ;
        occ.reserve(unitsA.size() + unitsB.size() + 4);
        auto occupyIf = [&](bool ok, const Coord& c) {
            if (ok) occ.insert(packCoord(c));
        };
        occupyIf(baseA && !baseA->isDestroyed(), baseA->getPos());
        occupyIf(baseB && !baseB->isDestroyed(), baseB->getPos());
        for (auto& u : unitsA) occupyIf(u && u->isAlive(), u->getPos());
        for (auto& u : unitsB) occupyIf(u && u->isAlive(), u->getPos());

        for (const auto* intent : winners) {
            auto it = unitById.find(intent->unitId);
            if (it == unitById.end()) continue;
            auto& unit = it->second;
            if (!unit || !unit->isAlive()) continue;
            Coord from = unit->getPos();
            Coord to = intent->to;
            if (from == to) continue;
            auto kPrev = packCoord(from);
            auto kNow = packCoord(to);
            occ.erase(kPrev);
            if (occ.find(kNow) != occ.end()) {
                occ.insert(kPrev);
                continue;
            }
            unit->pos = to;
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
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        unitsASnapAtk.reserve(unitsA.size());
        unitsBSnapAtk.reserve(unitsB.size());
        for (const auto& u : unitsA) {
            if (!u || !u->isAlive()) continue;
            occAtk.insert(packCoord(u->pos));
            UnitSnapshot snap;
            snap.id = u->id;
            snap.type = u->type;
            snap.faction = u->owner;
            snap.pos = u->pos;
            snap.hp = u->hp;
            snap.maxHp = u->baseStats.maxHP;
            snap.stats = u->baseStats;
            if (u->behavior) {
                if (auto* attackBehavior = u->behavior->getAttackBehavior()) {
                    snap.cooldown = attackBehavior->getCooldown();
                    snap.currentTarget = attackableKey(attackBehavior->getTarget().lock());
                }
                if (auto* moveBehavior = u->behavior->getMovementBehavior()) {
                    snap.movementState = moveBehavior->snapshot();
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
            }
            unitsASnapAtk.push_back(std::move(snap));
        }
        for (const auto& u : unitsB) {
            if (!u || !u->isAlive()) continue;
            occAtk.insert(packCoord(u->pos));
            UnitSnapshot snap;
            snap.id = u->id;
            snap.type = u->type;
            snap.faction = u->owner;
            snap.pos = u->pos;
            snap.hp = u->hp;
            snap.maxHp = u->baseStats.maxHP;
            snap.stats = u->baseStats;
            if (u->behavior) {
                if (auto* attackBehavior = u->behavior->getAttackBehavior()) {
                    snap.cooldown = attackBehavior->getCooldown();
                    snap.currentTarget = attackableKey(attackBehavior->getTarget().lock());
                }
                if (auto* moveBehavior = u->behavior->getMovementBehavior()) {
                    snap.movementState = moveBehavior->snapshot();
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
            }
            unitsBSnapAtk.push_back(std::move(snap));
        }
        if (baseA && !baseA->isDestroyed()) occAtk.insert(packCoord(baseA->pos));
        if (baseB && !baseB->isDestroyed()) occAtk.insert(packCoord(baseB->pos));

        enemiesASnapAtk.reserve(unitsB.size() + 1);
        for (const auto& u : unitsB) {
            if (!u || !u->isAlive()) continue;
            AttackableSnapshot snap;
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
            enemyIndexAAtk[snap.key] = enemiesASnapAtk.size();
            enemiesASnapAtk.push_back(std::move(snap));
        }
        if (baseB && !baseB->isDestroyed()) {
            AttackableSnapshot snap;
            snap.key.id = baseB->id;
            snap.key.type = AttackableType::BASE;
            snap.type = AttackableType::BASE;
            snap.faction = baseB->faction;
            snap.unitType = UnitType::Infantry;
            snap.pos = baseB->pos;
            snap.hp = baseB->hp;
            snap.maxHp = baseB->maxHp;
            snap.isBase = true;
            enemyIndexAAtk[snap.key] = enemiesASnapAtk.size();
            enemiesASnapAtk.push_back(std::move(snap));
        }

        enemiesBSnapAtk.reserve(unitsA.size() + 1);
        for (const auto& u : unitsA) {
            if (!u || !u->isAlive()) continue;
            AttackableSnapshot snap;
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
            enemyIndexBAtk[snap.key] = enemiesBSnapAtk.size();
            enemiesBSnapAtk.push_back(std::move(snap));
        }
        if (baseA && !baseA->isDestroyed()) {
            AttackableSnapshot snap;
            snap.key.id = baseA->id;
            snap.key.type = AttackableType::BASE;
            snap.type = AttackableType::BASE;
            snap.faction = baseA->faction;
            snap.unitType = UnitType::Infantry;
            snap.pos = baseA->pos;
            snap.hp = baseA->hp;
            snap.maxHp = baseA->maxHp;
            snap.isBase = true;
            enemyIndexBAtk[snap.key] = enemiesBSnapAtk.size();
            enemiesBSnapAtk.push_back(std::move(snap));
        }
    }

    std::sort(unitsASnapAtk.begin(), unitsASnapAtk.end(), byUnitId);
    std::sort(unitsBSnapAtk.begin(), unitsBSnapAtk.end(), byUnitId);
    std::sort(enemiesASnapAtk.begin(), enemiesASnapAtk.end(), byAttackableKey);
    std::sort(enemiesBSnapAtk.begin(), enemiesBSnapAtk.end(), byAttackableKey);
    enemyIndexAAtk.clear();
    for (std::size_t i = 0; i < enemiesASnapAtk.size(); ++i) {
        enemyIndexAAtk[enemiesASnapAtk[i].key] = i;
    }
    enemyIndexBAtk.clear();
    for (std::size_t i = 0; i < enemiesBSnapAtk.size(); ++i) {
        enemyIndexBAtk[enemiesBSnapAtk[i].key] = i;
    }

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
        taskPool.submit([&, unit]() {
            AttackIntent intent = planAttackIntent(unit, dt, map, enemies,
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
        std::unique_lock<std::shared_mutex> lock(worldMutex);
        std::vector<std::shared_ptr<Unit>> allUnits;
        allUnits.reserve(unitsA.size() + unitsB.size());
        for (auto& u : unitsA) allUnits.push_back(u);
        for (auto& u : unitsB) allUnits.push_back(u);
        std::sort(allUnits.begin(), allUnits.end(),
                  [](const std::shared_ptr<Unit>& a, const std::shared_ptr<Unit>& b) {
                      return a->id < b->id;
                  });

        std::vector<std::weak_ptr<IAttackable>> forcedAAtk;
        std::vector<std::weak_ptr<IAttackable>> forcedBAtk;
        forcedAAtk.reserve(forcedVisibleForA.size());
        forcedBAtk.reserve(forcedVisibleForB.size());
        for (const auto& r : forcedVisibleForA) {
            if (!r.target.expired()) forcedAAtk.push_back(r.target);
        }
        for (const auto& r : forcedVisibleForB) {
            if (!r.target.expired()) forcedBAtk.push_back(r.target);
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior) continue;
            const auto& enemies = (u->getFaction() == Faction::A) ? enemiesA : enemiesB;
            const auto& forced = (u->getFaction() == Faction::A) ? forcedAAtk : forcedBAtk;
            u->behavior->updateVision(*u, map, enemies, forced);
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
                auto base = findBase(id, Faction::A);
                if (!base) base = findBase(id, Faction::B);
                return base;
            }
            return findUnit(id);
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
            auto attacker = findUnit(intent.attackerId);
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
            auto attacker = findUnit(attackerId);
            if (attacker) {
                revealAttacker(*attacker);
            }
        }

        for (auto& u : allUnits) {
            if (!u || !u->behavior) continue;
            const auto& enemies = (u->getFaction() == Faction::A) ? enemiesA : enemiesB;
            u->behavior->postAttackStateUpdate(*u, map, enemies);
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
                const auto& enemyUnits = (u->getFaction() == Faction::A) ? unitsB : unitsA;
                float threat = computeLocalThreatWorld(*u, enemyUnits, kThreatRadius);
                if (threat < kOocParams.threatThreshold) {
                    float regen = u->baseStats.maxHP * kOocParams.regenRate * dt;
                    u->hp = std::min(u->baseStats.maxHP, u->hp + regen);
                }
            }
        }

        lastIncomingDamageA = std::move(incomingDamageAFrame);
        lastIncomingDamageB = std::move(incomingDamageBFrame);
        lastLockedTargetsA = std::move(lockedTargetsAFrame);
        lastLockedTargetsB = std::move(lockedTargetsBFrame);

        // std::cout << "  CleanupSystem...\n";
        cleanupSystem.update(*this);

        // 清理选中列表中已不存在的单位
        std::unordered_set<int> alive;
        for (auto& u : unitsA) if (u && u->isAlive()) alive.insert(u->id);
        for (auto& u : unitsB) if (u && u->isAlive()) alive.insert(u->id);
        selectedUnitIds.erase(
            std::remove_if(selectedUnitIds.begin(), selectedUnitIds.end(),
                [&](int id){ return alive.find(id) == alive.end(); }),
            selectedUnitIds.end()
        );
    }
}


bool GameWorld::isTileFree(const Coord& c) const {
    for(auto& u : unitsA) {
        if(u->isAlive() && u->getPos() == c) return false;

    }

    for(auto& u : unitsB) {
        if(u->isAlive() && u->getPos() == c) return false;
    }

    if (baseA && !baseA->isDestroyed() && baseA->getPos() == c)
        return false;
    if (baseB && !baseB->isDestroyed() && baseB->getPos() == c)
        return false;

    return true;
}

void GameWorld::startRenderThread() {
    if(renderRunning.load()) return;

    renderRunning.store(true);
    if(!renderSystem) renderSystem = std::make_unique<RenderSystem>();
    renderSystem->clock.restart();

    renderThread = std::thread([this]() {
        const unsigned W = static_cast<unsigned>(map.getWidth());
        const unsigned H = static_cast<unsigned>(map.getHeight());

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

        auto clearTargeting = [&]() {
            pendingTarget.reset();
            controlMode = ControlMode::Idle;
            resume();
        };
        auto enterTargeting = [&]() {
            pendingTarget.reset();
            controlMode = ControlMode::Targeting;
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
            selectedUnitIds = {unit->id};
            postUi("click select", "Selected #" + std::to_string(unit->id));
        };
        auto selectedFaction = [&]() -> std::optional<Faction> {
            if (selectedUnitIds.empty()) return std::nullopt;
            auto u = findUnit(selectedUnitIds.front());
            if (!u) return std::nullopt;
            return u->getFaction();
        };
        auto findUnitAt = [&](const Coord& coord) -> std::shared_ptr<Unit> {
            for (auto& u : unitsA) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            for (auto& u : unitsB) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            return nullptr;
        };
        auto resolveEnemyAt = [&](Faction enemyFaction, const Coord& coord)
            -> std::shared_ptr<IAttackable> {
            if (enemyFaction == Faction::A) {
                if (baseA && !baseA->isDestroyed() && baseA->getPos() == coord) return baseA;
                for (auto& u : unitsA) {
                    if (u && u->isAlive() && u->getPos() == coord) return u;
                }
                return nullptr;
            }
            if (baseB && !baseB->isDestroyed() && baseB->getPos() == coord) return baseB;
            for (auto& u : unitsB) {
                if (u && u->isAlive() && u->getPos() == coord) return u;
            }
            return nullptr;
        };
        auto issueMoveTo = [&](const Coord& coord) {
            for (int id : selectedUnitIds) {
                auto u = findUnit(id);
                if (u) u->issueMove(coord);
            }
            postUi("click move",
                   "Move to " + std::to_string(coord.x) + "," + std::to_string(coord.y));
        };
        auto issueAttack = [&](const std::shared_ptr<IAttackable>& target) {
            for (int id : selectedUnitIds) {
                auto u = findUnit(id);
                if (u) u->issueAttackTarget(target);
            }
            postUi("click attack", "Attack target set");
        };
        auto shutdownStart = std::chrono::steady_clock::time_point{};
        bool shutdownArmed = false;
        while(renderRunning.load()) {
            if (!window.isOpen()) {
                renderRunning.store(false);
                requestQuit();
                break;
            }

            while(const std::optional event = window.pollEvent()) {
                if(event->is<sf::Event::Closed>()) {
                    window.close();
                    renderRunning.store(false);
                    requestQuit();
                }

                if (auto key = event->getIf<sf::Event::KeyPressed>()) {
                    using sf::Keyboard::Key;

                    if (awaitingProductionChoice) {
                        if (key->code == Key::Backspace) {
                            handleProductionBackspace();
                            continue;
                        }
                        if (key->code == Key::P) {
                            cancelProductionChoice();
                            resume();
                            postUiInput("production cancel (P)");
                            continue;
                        }
                        if (key->code == Key::Enter) {
                            commitProductionChoice();
                            continue;
                        }
                        if (key->code == Key::Escape) {
                            cancelProductionChoice();
                            resume();
                            continue;
                        }
                    }

                    if (controlMode == ControlMode::Targeting) {
                        if (key->code == Key::Enter) {
                            std::unique_lock<std::shared_mutex> lock(worldMutex);
                            if (!pendingTarget || selectedUnitIds.empty()) {
                                continue;
                            }
                            auto selFactionOpt = selectedFaction();
                            if (!selFactionOpt.has_value()) {
                                clearTargeting();
                                continue;
                            }
                            Faction selFaction = *selFactionOpt;
                            Faction enemyFaction =
                                (selFaction == Faction::A) ? Faction::B : Faction::A;

                            if (pendingTarget->kind == PendingTarget::Kind::Unit) {
                                auto targetUnit = findUnit(pendingTarget->unitId);
                                if (targetUnit && targetUnit->isAlive()) {
                                    if (targetUnit->getFaction() == enemyFaction) {
                                        issueAttack(targetUnit);
                                    } else {
                                        issueMoveTo(targetUnit->getPos());
                                    }
                                } else {
                                    issueMoveTo(pendingTarget->tile);
                                }
                            } else {
                                auto target = resolveEnemyAt(enemyFaction, pendingTarget->tile);
                                if (target) {
                                    issueAttack(target);
                                } else {
                                    issueMoveTo(pendingTarget->tile);
                                }
                            }
                            clearTargeting();
                            continue;
                        }
                        if (key->code == Key::Escape) {
                            std::unique_lock<std::shared_mutex> lock(worldMutex);
                            clearTargeting();
                            continue;
                        }
                    }

                    if (key->code == Key::Enter) {
                        if (renderSystem->inputActive) {
                            if (!renderSystem->inputBuffer.empty()) {
                                enqueueCommand(renderSystem->inputBuffer);
                                postUiInput(renderSystem->inputBuffer);
                            }
                            renderSystem->inputBuffer.clear();
                            renderSystem->inputActive = false;
                        } else {
                            renderSystem->inputBuffer.clear();
                            renderSystem->inputActive = true;
                        }
                    } else if (key->code == Key::Backspace) {
                        if (renderSystem->inputActive && !renderSystem->inputBuffer.empty()) {
                            renderSystem->inputBuffer.pop_back();
                        }
                    } else if (key->code == Key::Escape) {
                        if (renderSystem->inputActive) {
                            renderSystem->inputBuffer.clear();
                            renderSystem->inputActive = false;
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

                if (renderSystem->inputActive && !awaitingProductionChoice) {
                    if (const auto text = event->getIf<sf::Event::TextEntered>()) {
                        char32_t uni = text->unicode;
                        if (uni >= 32 && uni < 127) {
                            renderSystem->inputBuffer.push_back(static_cast<char>(uni));
                        }
                    }
                } else if (awaitingProductionChoice) {
                    if (const auto text = event->getIf<sf::Event::TextEntered>()) {
                        char32_t uni = text->unicode;
                        if (uni >= U'0' && uni <= U'9') {
                            handleProductionDigit(static_cast<char>(uni));
                        }
                    }
                }

                if (const auto mouse = event->getIf<sf::Event::MouseButtonPressed>()) {
                    if (awaitingProductionChoice && mouse->button != sf::Mouse::Button::Middle) {
                        // 忽略其他鼠标操作，保持选择流程
                        continue;
                    }

                    auto coordOpt = renderSystem->pixelToTile(*this, window, mouse->position);

                    if (mouse->button == sf::Mouse::Button::Middle) {
                        togglePause();
                        postUi("mouse pause", paused.load() ? "Paused" : "Resumed");
                        continue;
                    }

                    if (!coordOpt) continue;
                    Coord clicked = *coordOpt;

                    if (mouse->button == sf::Mouse::Button::Left) {
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        if (controlMode == ControlMode::Targeting) {
                            auto hitUnit = findUnitAt(clicked);
                            auto selFactionOpt = selectedFaction();
                            if (hitUnit && selFactionOpt.has_value() &&
                                hitUnit->getFaction() == *selFactionOpt) {
                                selectUnit(hitUnit);
                                pendingTarget.reset();
                            } else if (hitUnit) {
                                pendingTarget = PendingTarget{
                                    PendingTarget::Kind::Unit,
                                    hitUnit->getPos(),
                                    hitUnit->id
                                };
                            } else {
                                pendingTarget = PendingTarget{
                                    PendingTarget::Kind::Tile,
                                    clicked,
                                    -1
                                };
                            }
                            continue;
                        }
                        std::shared_ptr<Base> baseTarget;
                        if (baseA && !baseA->isDestroyed() && baseA->getPos() == clicked) {
                            baseTarget = baseA;
                        } else if (baseB && !baseB->isDestroyed() && baseB->getPos() == clicked) {
                            baseTarget = baseB;
                        }
                        if (baseTarget) {
                            beginProductionChoice(baseTarget);
                            continue;
                        }

                        std::shared_ptr<Unit> pick;
                        for (auto& u : unitsA) {
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
                        if (controlMode == ControlMode::Targeting) {
                            continue;
                        }
                        std::unique_lock<std::shared_mutex> lock(worldMutex);
                        if (selectedUnitIds.empty()) continue;

                        std::shared_ptr<IAttackable> target;
                        if (baseB && !baseB->isDestroyed() && baseB->getPos() == clicked) {
                            target = baseB;
                        }
                        if (!target) {
                            for (auto& u : unitsB) {
                                if (u && u->isAlive() && u->getPos() == clicked) {
                                    target = u;
                                    break;
                                }
                            }
                        }

                        if (target) {
                            for (int id : selectedUnitIds) {
                                auto u = findUnit(id);
                                if (u) u->issueAttackTarget(target);
                            }
                            pause();
                            postUi("click attack", "Attack target set");
                        } else {
                            for (int id : selectedUnitIds) {
                                auto u = findUnit(id);
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
                renderSystem->renderSfml(*this, window);
            }
            window.display();
        }
    });
}

void GameWorld::stopRenderThread() {
    renderRunning.store(false);
    if(renderThread.joinable()) {
        renderThread.join();
    }
}

std::optional<UnitType> GameWorld::unitTypeFromCode(int code) const {
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

void GameWorld::beginProductionChoice(const std::shared_ptr<Base>& base) {
    if (!base) return;
    awaitingProductionChoice = true;
    productionChoiceBase = base;
    productionInputBuffer.clear();
    pause();
    enqueueUiEvent("production choice",
                   "Enter unit code then press Enter; Esc to cancel");
}

bool GameWorld::handleProductionDigit(char digit) {
    if (!std::isdigit(static_cast<unsigned char>(digit))) return false;
    productionInputBuffer.push_back(digit);
    enqueueUiEvent(std::nullopt,
                   "Production code: " + productionInputBuffer + " \n\t(Enter to confirm)");
    return true;
}

bool GameWorld::handleProductionBackspace() {
    if (productionInputBuffer.empty()) return false;
    productionInputBuffer.pop_back();
    enqueueUiEvent(std::nullopt,
                   productionInputBuffer.empty()
                       ? "Production code cleared"
                       : "Production code: " + productionInputBuffer);
    return true;
}

bool GameWorld::commitProductionChoice() {
    auto basePtr = productionChoiceBase.lock();
    if (!basePtr) {
        cancelProductionChoice();
        return false;
    }

    if (productionInputBuffer.empty()) {
        enqueueUiEvent(std::nullopt, "Please enter a numeric code");
        return false;
    }

    auto ignoreInvalid = [&](const std::string& msg) {
        awaitingProductionChoice = false;
        productionChoiceBase.reset();
        productionInputBuffer.clear();
        resume();
        enqueueUiEvent(std::nullopt, msg);
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
    enqueueUiEvent(std::nullopt,
                   "Queued " +
                       std::string(*type == UnitType::Infantry ? "Infantry" :
                                   *type == UnitType::Archer   ? "Archer"   : "Knight"));
    awaitingProductionChoice = false;
    productionChoiceBase.reset();
    productionInputBuffer.clear();
    resume();
    return true;
}

void GameWorld::cancelProductionChoice() {
    awaitingProductionChoice = false;
    productionChoiceBase.reset();
    productionInputBuffer.clear();
    enqueueUiEvent(std::nullopt, "Production canceled");
    resume();
}

void GameWorld::rebuildEnemies() {
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

void GameWorld::enqueueCommand(const std::string& line) {
    commandQueue.push(line);
}

void GameWorld::processCommands() {
    std::string line;
    while (commandQueue.tryPop(line)) {
        lastCommandInput = line;
        Command parsed;
        std::string err;
        if (!parseCommand(line, parsed, err)) {
            lastCommandFeedback = "ERR: " + err;
            continue;
        }
        CommandResult r = executeCommand(parsed, *this);
        if (r.ok) {
            lastCommandFeedback = r.normalized + " -> " + r.message;
        } else {
            lastCommandFeedback = "ERR: " + r.message;
        }
    }
}

void GameWorld::drainUiEvents() {
    std::queue<UiEvent> local;
    {
        std::lock_guard<std::mutex> lock(uiEventMutex);
        std::swap(local, uiEvents);
    }
    while (!local.empty()) {
        const UiEvent& evt = local.front();
        if (evt.input.has_value()) {
            lastCommandInput = *evt.input;
        }
        if (evt.feedback.has_value()) {
            lastCommandFeedback = *evt.feedback;
        }
        local.pop();
    }
}

void GameWorld::enqueueUiEvent(std::optional<std::string> input,
                               std::optional<std::string> feedback) {
    UiEvent evt;
    evt.input = std::move(input);
    evt.feedback = std::move(feedback);
    {
        std::lock_guard<std::mutex> lock(uiEventMutex);
        uiEvents.push(std::move(evt));
    }
}

std::shared_ptr<Unit> GameWorld::findUnit(int id) const {
    if (id < 0) return nullptr;
    for (auto& u : unitsA) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    for (auto& u : unitsB) {
        if (u && u->id == id && u->isAlive()) return u;
    }
    return nullptr;
}

std::shared_ptr<Base> GameWorld::findBase(int id, Faction fac) const {
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

std::shared_ptr<IAttackable> GameWorld::findAttackable(int id) const {
    if (id < 0) return nullptr;
    auto u = findUnit(id);
    if (u) return u;

    if (baseA && baseA->getId() == id && !baseA->isDestroyed()) return baseA;
    if (baseB && baseB->getId() == id && !baseB->isDestroyed()) return baseB;
    return nullptr;
}

void GameWorld::setSelection(const std::vector<int>& ids) {
    selectedUnitIds = ids;
}

void GameWorld::clearSelection() {
    selectedUnitIds.clear();
}

int GameWorld::registerUnit(const std::shared_ptr<Unit>& u) {
    if (!u) return -1;
    u->id = nextUnitId++;
    return u->id;
}

int GameWorld::registerBase(const std::shared_ptr<Base>& b) {
    if (!b) return -1;
    b->setId(nextBaseId++);
    return b->getId();
}

void GameWorld::pause() {
    paused.store(true);
}

void GameWorld::resume() {
    paused.store(false);
}

void GameWorld::togglePause() {
    if (paused.load()) {
        resume();
    } else {
        pause();
    }
}

void GameWorld::markGameOver() {
    gameEnded.store(true);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    gameEndTimestampMs.store(nowMs);
    pause();
}

void GameWorld::addForcedReveal(Faction viewer, const std::shared_ptr<IAttackable>& target, float durationSeconds) {
    if (!target) return;
    auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    bucket.push_back({target, durationSeconds});
}

void GameWorld::decayForcedReveals(float dt) {
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

void GameWorld::appendForcedReveals(Faction viewer, std::vector<std::weak_ptr<IAttackable>>& out) const {
    const auto& bucket = (viewer == Faction::A) ? forcedVisibleForA : forcedVisibleForB;
    for (const auto& r : bucket) {
        if (!r.target.expired()) {
            out.push_back(r.target);
        }
    }
}

void GameWorld::revealAttacker(const Unit& u) {
    auto attacker = findUnit(u.id);
    if (!attacker) return;
    Faction viewer = (u.owner == Faction::A) ? Faction::B : Faction::A;
    addForcedReveal(viewer, attacker, 2.0f);
}

void GameWorld::waitRenderThread() {
    if (renderThread.joinable()) {
        renderThread.join();
    }
}
