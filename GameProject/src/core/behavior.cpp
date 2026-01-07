#include "core/behavior.hpp"
#include "core/gameworld.hpp"
#include "core/Iattackable.hpp"
#include "core/map.hpp"
#include "utils/vec2.hpp"
#include <memory>
#include <cmath>


class GameWorld;

struct BehaviorUtil {
    static Coord pickRandomWanderTarget(const Unit& u,
                                        const Map& map,
                                        const std::vector<std::weak_ptr<IAttackable>>& enemies,
                                        std::mt19937& rng);
    static void fillCommitTarget(const std::shared_ptr<IAttackable>& target,
                                 int& outId,
                                 AttackableType& outType);
    static UnitState mapLegacyState(LifeState life,
                                    LocomotionState locomotion,
                                    CombatState combat,
                                    MoveReason moveReason,
                                    CombatAction combatAction);
};

Coord BehaviorUtil::pickRandomWanderTarget(const Unit& u, const Map& map,
                                           const std::vector<std::weak_ptr<IAttackable>>& enemies,
                                           std::mt19937& rng) {
    std::uniform_int_distribution<int> dx(-5, 5);
    std::uniform_int_distribution<int> dy(-5, 5);

    Coord start = u.getPos();

    bool hasBase = false;
    Coord basePos = start;
    for (const auto& w : enemies) {
        auto e = w.lock();
        if (!e || e->isDestroyed()) continue;
        if (e->getAttackType() == AttackableType::BASE) {
            basePos = e->getPos();
            hasBase = true;
            break;
        }
    }
    auto sgn = [](int v) { return (v > 0) - (v < 0); };
    const int dirX = sgn(basePos.x - start.x);
    const int dirY = sgn(basePos.y - start.y);

    std::uniform_int_distribution<int> stepDist(4, 9);
    std::uniform_int_distribution<int> noise(-3, 3);
    std::uniform_real_distribution<float> jitter(0.f, 1.f);

    Coord best = start;
    float bestScore = -1e30f;

    for (int tries = 0; tries < 30; ++tries) {
        int step = stepDist(rng);
        Coord cand{
            start.x + dirX * step + noise(rng),
            start.y + dirY * step + noise(rng)
        };

        if (!map.inBounds(cand)) continue;
        const Tile& tile = map.getTile(cand);
        if (!tile.isPassable()) continue;
        if (!map.isReachable(start, cand)) continue;

        float dBase = static_cast<float>(cand.mhtDistanceTo(basePos));
        float cost  = tile.getMoveCost();
        float dSelf = static_cast<float>(cand.mhtDistanceTo(start));

        // 权重可后续再调：目前偏“更快相遇/更快推进”
        float score = (-1.2f * dBase) + (-2.0f * cost) + (-0.15f * dSelf) + 0.01f * jitter(rng);

        if (score > bestScore) {
            bestScore = score;
            best = cand;
        }
    }

    return best;
}

void BehaviorUtil::fillCommitTarget(const std::shared_ptr<IAttackable>& target,
                                    int& outId,
                                    AttackableType& outType) {
    outId = -1;
    outType = AttackableType::UNIT;
    if (!target) return;
    outType = target->getAttackType();
    if (outType == AttackableType::BASE) {
        auto basePtr = std::dynamic_pointer_cast<Base>(target);
        outId = basePtr ? basePtr->getId() : -1;
    } else {
        auto unitPtr = std::dynamic_pointer_cast<Unit>(target);
        outId = unitPtr ? unitPtr->getId() : -1;
    }
}

UnitState BehaviorUtil::mapLegacyState(LifeState life,
                                       LocomotionState locomotion,
                                       CombatState combat,
                                       MoveReason moveReason,
                                       CombatAction combatAction) {
    if (life == LifeState::Dead) return UnitState::Dead;
    if (combat == CombatState::Engaging) {
        if (locomotion == LocomotionState::Pathing) {
            return UnitState::Chasing;
        }
        if (combatAction == CombatAction::Attack) {
            return UnitState::Attacking;
        }
        return UnitState::Attacking;
    }
    if (locomotion == LocomotionState::Pathing) {
        if (moveReason == MoveReason::Wander) return UnitState::Wandering;
        return UnitState::Moving;
    }
    return UnitState::Idle;
}

AttackableKey AttackableKey::from(const std::shared_ptr<IAttackable>& target) {
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

const AiScoring::TargetScoreParams AiScoring::kTargetParams{};
const AiScoring::ActionParams AiScoring::kActionParams{};
const AiScoring::RetreatParams AiScoring::kRetreatParams{};
const AiScoring::OocParams AiScoring::kOocParams{};

using UnitSnapshot = AiScoring::UnitSnapshot;
using AttackableSnapshot = AiScoring::AttackableSnapshot;
using TargetScoreParams = AiScoring::TargetScoreParams;
using ActionParams = AiScoring::ActionParams;
using RetreatParams = AiScoring::RetreatParams;
using OocParams = AiScoring::OocParams;
using ReachableGrid = MapQuery::ReachableGrid;
using TargetChoice = AiScoring::TargetChoice;

float AiTargetScoring::unitPreference(UnitType self, UnitType target) {
    static const float prefs[3][3] = {
        {1.5f, 1.0f, 1.2f},  // Infantry vs {Infantry, Archer, Knight}
        {1.0f, 1.5f, 1.2f},  // Archer
        {1.2f, 0.9f, 1.3f}   // Knight
    };
    return prefs[static_cast<int>(self)][static_cast<int>(target)];
}

float AiTargetScoring::basePreference(UnitType self) {
    switch (self) {
        case UnitType::Infantry: return 0.9f;
        case UnitType::Archer:   return 0.7f;
        case UnitType::Knight:   return 1.1f;
    }
    return 1.0f;
}

float AiTargetScoring::threatScore(const AttackableSnapshot& target) {
    if (target.isBase) return 0.f;
    return target.attack * (0.6f + 0.12f * target.attackRange);
}

float AiTargetScoring::distanceScore(float dist) {
    return 1.0f / (1.0f + dist);
}

float AiTargetScoring::ttkScore(const UnitSnapshot& self, const AttackableSnapshot& target) {
    float dps = std::max(1.0f, self.stats.attack);
    float ttk = target.hp / dps;
    return 1.0f / (1.0f + ttk);
}

bool AiTargetScoring::containsKey(const std::vector<const AttackableSnapshot*>& visible,
                                  const AttackableKey& key) {
    for (const auto* v : visible) {
        if (v && v->key == key) return true;
    }
    return false;
}

std::vector<const AttackableSnapshot*> AiTargetScoring::collectVisibleEnemies(
    const UnitSnapshot& unit,
    const Map& map,
    const std::vector<AttackableSnapshot>& enemies,
    const std::unordered_map<AttackableKey, std::size_t, AttackableKeyHash>& enemyIndex,
    const std::vector<AttackableKey>& forcedKeys) {
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

float AiTargetScoring::computeLocalThreat(const UnitSnapshot& self,
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

float AiTargetScoring::computeLocalThreatWorld(const Unit& self,
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

int AiTargetScoring::countNearbyAllies(const std::vector<UnitSnapshot>& allies,
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

int AiTargetScoring::countNearbyEnemies(const std::vector<const AttackableSnapshot*>& enemies,
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

float AiTargetScoring::baseOpportunityScore(const UnitSnapshot& self,
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

float AiTargetScoring::firingPositionAvailability(const UnitSnapshot& self,
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
            if (MapQuery::hasMountainBetween(map, cand, target.pos)) continue;
            auto key = Coord::packCoord(cand);
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

float AiTargetScoring::scoreTargetCandidate(
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

    if (MapQuery::hasMountainBetween(map, self.pos, target.pos)) {
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

TargetChoice AiTargetScoring::chooseTarget(
    const UnitSnapshot& self,
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

    float localThreat = computeLocalThreat(self, visible, AiScoring::kThreatRadius);
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

CombatAction CombatPlanner::decideCombatAction(const UnitSnapshot& self,
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

float CombatPlanner::computeAttackDamage(const UnitSnapshot& self, const Map& map) {
    const Tile& tile = map.getTile(self.pos);
    float dmg = self.stats.attack + tile.getAttackBonus();
    if (tile.getType() == TileType::HILL) {
        dmg *= 0.85f;
    }
    return dmg;
}

float CombatPlanner::computeAttackCooldown(const UnitSnapshot& self, const Map& map) {
    const Tile& tile = map.getTile(self.pos);
    float cdBase = 0.8f;
    if (tile.getType() == TileType::HILL) {
        cdBase *= 1.25f;
    }
    return cdBase;
}

AttackIntent CombatPlanner::planAttackIntent(
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
        AiTargetScoring::collectVisibleEnemies(unit, map, enemies, enemyIndex, forcedKeys);
    ReachableGrid reachable = MapQuery::buildReachableGrid(map, unit.pos);

    TargetChoice choice = AiTargetScoring::chooseTarget(unit, visible, allies,
                                                       incomingDamage, lockedCounts,
                                                       AiScoring::kTargetParams, map, reachable,
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
            if (curTarget && MapQuery::inAttackRange(map, unit.pos, unit.stats, curTarget->pos)) {
                float localThreat = AiTargetScoring::computeLocalThreat(unit, visible,
                                                                       AiScoring::kThreatRadius);
                float baseOpp = 0.f;
                float curScore = AiTargetScoring::scoreTargetCandidate(unit, *curTarget, allies,
                                                                       visible, incomingDamage,
                                                                       lockedCounts,
                                                                       AiScoring::kTargetParams,
                                                                       map, reachable, occ,
                                                                       localThreat, &baseOpp);
                if (!target || (choice.score - curScore) <= AiScoring::kTargetParams.commitBonus) {
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
    bool inRange = MapQuery::inAttackRange(map, unit.pos, unit.stats, target->pos);
    float localThreat = AiTargetScoring::computeLocalThreat(unit, visible, AiScoring::kThreatRadius);
    intent.action = decideCombatAction(unit, target, dist, inRange, cdAfter,
                                       localThreat, choice.baseOpportunity,
                                       AiScoring::kActionParams);
    float hpRatio = (unit.maxHp > 0.f) ? (unit.hp / unit.maxHp) : 1.0f;
    bool enterRetreat = (hpRatio <= AiScoring::kRetreatParams.hpEnter);
    bool exitRetreat = (hpRatio >= AiScoring::kRetreatParams.hpExit &&
                        unit.retreatTimer <= 0.f);
    bool retreatNow = unit.retreating ? !exitRetreat : enterRetreat;
    if (retreatNow) {
        intent.action = CombatAction::Retreat;
    }
    bool commitSame = (target->key == unit.commitTarget && unit.commitTimer > 0.f);
    float commitDuration = (intent.action == CombatAction::Siege)
        ? AiScoring::kTargetParams.siegeHoldTime
        : AiScoring::kTargetParams.commitDuration;
    intent.nextCommitTimer = commitSame ? std::max(0.0f, unit.commitTimer - dt)
                                        : commitDuration;

    bool allowRetreatFire = (intent.action != CombatAction::Retreat ||
                             localThreat < AiScoring::kRetreatParams.retreatFireThreat);
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

constexpr float kHeatAlpha = 0.35f;
constexpr float kHeatOcc = 1.0f;
constexpr float kHeatTarget = 0.6f;
constexpr float kHeatNeighborScale = 0.5f;

float MovementPlanner::movementSpend(const UnitSnapshot& unit, const Tile& tile) {
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

float MovementPlanner::computeThreatAtCoord(const Coord& pos,
                                            const std::vector<AttackableSnapshot>& enemies,
                                            float radius) {
    float threat = 0.f;
    for (const auto& e : enemies) {
        if (e.isBase) continue;
        if (pos.mhtDistanceTo(e.pos) <= radius) {
            threat += AiTargetScoring::threatScore(e);
        }
    }
    return threat;
}

float MovementPlanner::heatAt(const std::vector<float>& heat, int width, const Coord& c) {
    if (width <= 0 || heat.empty()) return 0.f;
    std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
    if (idx >= heat.size()) return 0.f;
    return heat[idx];
}

void MovementPlanner::addHeat(std::vector<float>& heat, int width, int height,
                              const Coord& c, float value) {
    if (c.x < 0 || c.y < 0 || c.x >= width || c.y >= height) return;
    std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
    heat[idx] += value;
}

std::vector<float> MovementPlanner::buildHeatMap(const Map& map,
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
        Coord c = Coord::unpackCoord(packed);
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

Coord MovementPlanner::applyAnchorSlot(const Map& map,
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
    const std::size_t start = static_cast<std::size_t>(Coord::stableHash(unitId, anchor) % slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& off = slots[(start + i) % slots.size()];
        Coord cand{anchor.x + off.x, anchor.y + off.y};
        if (!map.inBounds(cand)) continue;
        if (!map.getTile(cand).isPassable()) continue;
        if (cand != selfPos && occ.find(Coord::packCoord(cand)) != occ.end()) continue;
        return cand;
    }
    return anchor;
}

Coord MovementPlanner::stepAlongManhattan(const Coord& from, const Coord& to, int steps) {
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

void MovementPlanner::fillMoveCandidates(const UnitSnapshot& unit,
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
    Coord clampedGoal = MapQuery::clampToMap(map, goal);
    const int width = map.getWidth();

    for (const auto& n : nbrs) {
        if (!map.inBounds(n)) continue;
        if (!map.getTile(n).isPassable()) continue;
        if (n != unit.pos && occ.find(Coord::packCoord(n)) != occ.end()) continue;
        if (intent.hasMove && n == intent.to) continue;
        float cost = map.getTile(n).getMoveCost();
        cost += static_cast<float>(n.mhtDistanceTo(clampedGoal));
        cost += kHeatAlpha * heatAt(heat, width, n);
        options.push_back({n, cost, Coord::stableHash(unit.id, n)});
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

bool MovementPlanner::selectAttackPosition(const UnitSnapshot& self,
                                           const AttackableSnapshot& target,
                                           const Map& map,
                                           const std::vector<AttackableSnapshot>& enemies,
                                           const ReachableGrid& reachable,
                                           const std::unordered_set<std::uint64_t>& occ,
                                           int minDist,
                                           int maxDist,
                                           Coord& outPos) {
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
            if (cand != self.pos && occ.find(Coord::packCoord(cand)) != occ.end()) continue;
            if (!MapQuery::inAttackRangeFrom(map, cand, self.stats, target.pos)) continue;

            int moveCost = self.pos.mhtDistanceTo(cand);
            float threat = computeThreatAtCoord(cand, enemies, AiScoring::kThreatRadius);
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
    outPos = bestPos;
    return true;
}

Coord MovementPlanner::chooseSafeSpot(const UnitSnapshot& self,
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
        float threat = computeThreatAtCoord(cand, enemies, AiScoring::kThreatRadius);
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

void MovementPlanner::rebuildPathState(const UnitSnapshot& unit,
                                       const Map& map,
                                       const Coord& target,
                                       IMovementBehavior::MovementState& state) {
    PathPlanner::rebuildPathState(map, unit.pos, target,
                                  state.path, state.idx, state.accumulator,
                                  state.lastTarget, state.hasLast);
}

void MovementPlanner::rebuildPathStateHeat(const UnitSnapshot& unit,
                                           const Map& map,
                                           const Coord& target,
                                           const std::vector<float>& heat,
                                           IMovementBehavior::MovementState& state) {
    PathPlanner::rebuildPathStateHeat(map, unit.pos, target, heat, unit.id,
                                      state.path, state.idx, state.accumulator,
                                      state.lastTarget, state.hasLast);
}

MoveIntent MovementPlanner::planMoveIntent(
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
            AiTargetScoring::collectVisibleEnemies(unit, map, enemies, enemyIndex, forcedKeys);
        ReachableGrid reachable = MapQuery::buildReachableGrid(map, unit.pos);
        TargetChoice choice = AiTargetScoring::chooseTarget(unit, visible, allies,
                                                           incomingDamage, lockedCounts,
                                                           AiScoring::kTargetParams, map, reachable,
                                                           occ);
        float localThreat = AiTargetScoring::computeLocalThreat(unit, visible, AiScoring::kThreatRadius);
        const AttackableSnapshot* target = choice.target;
        if (unit.commandAttackActive && unit.commandTarget.id >= 0) {
            auto it = enemyIndex.find(unit.commandTarget);
            if (it != enemyIndex.end()) {
                target = &enemies[it->second];
                choice.baseOpportunity = target->isBase
                    ? AiTargetScoring::baseOpportunityScore(unit, *target, allies, visible,
                                                           AiScoring::kTargetParams, localThreat)
                    : 0.f;
            }
        }
        float dist = target ? unit.pos.mhtDistanceTo(target->pos) : 0.f;
        float cdAfter = std::max(0.0f, unit.cooldown - dt);
        bool inRange = target
            ? MapQuery::inAttackRange(map, unit.pos, unit.stats, target->pos)
            : false;
        CombatAction action = CombatPlanner::decideCombatAction(unit, target, dist, inRange, cdAfter,
                                                                localThreat, choice.baseOpportunity,
                                                                AiScoring::kActionParams);
        float hpRatio = (unit.maxHp > 0.f) ? (unit.hp / unit.maxHp) : 1.0f;
        bool enterRetreat = (hpRatio <= AiScoring::kRetreatParams.hpEnter);
        bool exitRetreat = (hpRatio >= AiScoring::kRetreatParams.hpExit &&
                            retreatTimer <= 0.f);

        if (retreating) {
            if (exitRetreat) {
                retreating = false;
                retreatTimer = 0.f;
                hasAnchor = false;
            }
        } else if (enterRetreat) {
            retreating = true;
            retreatTimer = AiScoring::kRetreatParams.retreatMinTime;
            hasAnchor = false;
        }

        if (retreating) {
            if (!hasAnchor) {
                if (baseAlive) {
                    anchor = basePos;
                    hasAnchor = true;
                } else {
                    anchor = chooseSafeSpot(unit, map, enemies, AiScoring::kRetreatParams.sampleRadius);
                    hasAnchor = true;
                }
            }
            anchor = MapQuery::nearestPassable(map, anchor, 2);
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
            if (distToAnchor > AiScoring::kRetreatParams.maxRetreatDist) {
                desiredTarget = applyAnchorSlot(map, occ, anchor, unit.id, unit.pos);
                hasDesiredTarget = true;
                reason = MoveReason::Retreat;
            }
        }

        if (!retreating && action == CombatAction::Retreat && target) {
            int steps = static_cast<int>(std::ceil(AiScoring::kActionParams.kiteExtraDist));
            Coord awayGoal{unit.pos.x + (unit.pos.x - target->pos.x),
                           unit.pos.y + (unit.pos.y - target->pos.y)};
            desiredTarget = stepAlongManhattan(unit.pos, awayGoal, steps);
            hasDesiredTarget = true;
            reason = MoveReason::Retreat;
        } else if (!retreating && action == CombatAction::Kite && target) {
            int desiredDist = std::max(1, static_cast<int>(std::floor(
                unit.stats.attackRange - AiScoring::kActionParams.kiteMargin)));
            Coord kitePos{};
            if (selectAttackPosition(unit, *target, map, enemies, reachable, occ,
                                     desiredDist, desiredDist + 1, kitePos)) {
                desiredTarget = kitePos;
            } else {
                int steps = static_cast<int>(std::ceil(AiScoring::kActionParams.kiteExtraDist));
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
                    unit.stats.attackRange - AiScoring::kActionParams.kiteMargin)));
                if (dist > desiredDist) {
                    needMove = true;
                }
                if (needMove) {
                    Coord firePos{};
                    if (selectAttackPosition(unit, *target, map, enemies, reachable, occ,
                                             desiredDist, desiredDist + 1, firePos)) {
                        desiredTarget = firePos;
                    } else {
                        Coord anchorSlot = applyAnchorSlot(map, occ, target->pos, unit.id, unit.pos);
                        desiredTarget = MapQuery::nearestReachableToTarget(map, reachable, anchorSlot);
                    }
                    hasDesiredTarget = true;
                    reason = (action == CombatAction::Siege) ? MoveReason::Siege : MoveReason::Chase;
                }
            } else {
                if (needMove) {
                    Coord firePos{};
                    if (selectAttackPosition(unit, *target, map, enemies, reachable, occ,
                                             1, 2, firePos)) {
                        desiredTarget = firePos;
                    } else {
                        Coord anchorSlot = applyAnchorSlot(map, occ, target->pos, unit.id, unit.pos);
                        desiredTarget = MapQuery::nearestReachableToTarget(map, reachable, anchorSlot);
                    }
                    hasDesiredTarget = true;
                    reason = (action == CombatAction::Siege) ? MoveReason::Siege : MoveReason::Chase;
                }
            }
        }
    }

    if (!hasDesiredTarget &&
        unit.locomotionState == LocomotionState::Pathing &&
        unit.moveReason == MoveReason::Wander) {
        if (!nextState.path.empty()) {
            desiredTarget = nextState.path.back();
            hasDesiredTarget = true;
            reason = MoveReason::Wander;
        } else if (nextState.hasLast) {
            desiredTarget = nextState.lastTarget;
            hasDesiredTarget = true;
            reason = MoveReason::Wander;
        }
    }

    if (!hasDesiredTarget) {
        intent.to = unit.pos;
        intent.hasMove = false;
        intent.setIdle = true;
        intent.reason = MoveReason::None;
        intent.retreating = retreating;
        intent.retreatTimer = retreatTimer;
        intent.retreatAnchor = anchor;
        intent.hasRetreatAnchor = retreating && hasAnchor;
        intent.nextState = std::move(nextState);
        fillMoveCandidates(unit, map, desiredTarget, heat, occ, intent);
        return intent;
    }

    Coord adjustedTarget = desiredTarget;
    if (!map.inBounds(adjustedTarget) || !map.getTile(adjustedTarget).isPassable()) {
        adjustedTarget = MapQuery::nearestPassable(map, adjustedTarget, 3);
    }

    if (unit.pos == adjustedTarget) {
        intent.to = unit.pos;
        intent.hasMove = false;
        intent.setIdle = true;
        intent.reason = MoveReason::None;
        intent.retreating = retreating;
        intent.retreatTimer = retreatTimer;
        intent.retreatAnchor = anchor;
        intent.hasRetreatAnchor = retreating && hasAnchor;
        intent.nextState = std::move(nextState);
        fillMoveCandidates(unit, map, adjustedTarget, heat, occ, intent);
        return intent;
    }

    bool needRepath = false;
    if (nextState.path.empty()) {
        needRepath = true;
    } else if (!nextState.hasLast || nextState.lastTarget != adjustedTarget) {
        needRepath = true;
    }

    if (needRepath) {
        if (unit.commandMoveActive) {
            rebuildPathStateHeat(unit, map, adjustedTarget, heat, nextState);
        } else {
            rebuildPathState(unit, map, adjustedTarget, nextState);
        }
    }

    if (nextState.path.empty()) {
        intent.to = unit.pos;
        intent.hasMove = false;
        intent.setIdle = true;
        intent.reason = MoveReason::None;
        intent.retreating = retreating;
        intent.retreatTimer = retreatTimer;
        intent.retreatAnchor = anchor;
        intent.hasRetreatAnchor = retreating && hasAnchor;
        intent.nextState = std::move(nextState);
        fillMoveCandidates(unit, map, adjustedTarget, heat, occ, intent);
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

static float terrainSpend(Unit& u, const Tile& tile) {
    float s = u.baseStats.moveSpeed / tile.getMoveCost();
    if(u.type == UnitType::Knight) {
        switch(tile.getType()) {
            case TileType::PLAIN:  return s * 1.1f;
            case TileType::FOREST: return s * 0.6f;
            case TileType::HILL:   return s * 0.7f;
            case TileType::SWAMP:  return s * 0.4f;
            default: return s;
        }
    }
    return s;
}

UnitState DefaultStateMachine::get() const { return state; }
void DefaultStateMachine::set(UnitState s) { state = s; }

void DefaultCommandBehavior::issueMove(const Coord& dst) {
    pending = UnitCommandType::MoveTo;
    moveTarget = dst;
}

void DefaultCommandBehavior::issueAttack(const std::shared_ptr<IAttackable>& t) {
    pending = UnitCommandType::AttackUnit;
    attackTarget = t;
}

void DefaultCommandBehavior::issueStop() {
    pending = UnitCommandType::Stop;
}

UnitCommandType DefaultCommandBehavior::pendingType() const {
    return pending;
}

Coord DefaultCommandBehavior::pendingMoveTarget() const {
    return moveTarget;
}

std::weak_ptr<IAttackable> DefaultCommandBehavior::pendingAttackTarget() const {
    return attackTarget;
}

void DefaultCommandBehavior::clear() {
    pending = UnitCommandType::None;
    attackTarget.reset();
}

void DefaultMovementBehavior::setMoveTarget(const Coord& dst, const Map& map, Unit& u){
    path.clear();
    map.findPathAStar(u.pos, dst, path);
    idx = 0;
    accumulator = 0.0f;
}

void DefaultMovementBehavior::update(Unit& u, float dt, const Map& map) {
    if(path.empty()) return;
    if(idx >= path.size()) return;

    float sp = terrainSpend(u, map.getTile(u.pos));

    accumulator += sp * dt;
    while(accumulator >=  1.0f && idx + 1 < path.size()) {
        accumulator -= 1.f;
        u.pos = path[++idx];
    }

    if (idx + 1 >= path.size()) {
        // 路径走完后留给上层（UnitBehavior）去把状态改成 Idle
        path.clear();
    }
}

IMovementBehavior::MovementState DefaultMovementBehavior::snapshot() const {
    IMovementBehavior::MovementState state;
    state.path = path;
    state.idx = idx;
    state.accumulator = accumulator;
    state.lastTarget = lastTarget;
    state.hasLast = hasLast;
    return state;
}

void DefaultMovementBehavior::applyState(IMovementBehavior::MovementState state) {
    path = std::move(state.path);
    idx = state.idx;
    accumulator = state.accumulator;
    lastTarget = state.lastTarget;
    hasLast = state.hasLast;
}

void DefaultVisionBehavior::updateVisible(Unit& self, const Map& map,
                                          const std::vector<std::weak_ptr<IAttackable>>& allEnemies,
                                          const std::vector<std::weak_ptr<IAttackable>>& forcedVisible)
{
    visible.clear();

    if(!map.inBounds(self.pos)) return;
    const Tile& t = map.getTile(self.pos);
    float effVision = self.baseStats.visionRange + t.getVisionBonus();

    for(const auto& w : allEnemies) {
        auto e = w.lock();
        if(!e || e-> isDestroyed()) continue;

        Coord p = e->getPos();
        if (!map.inBounds(p)) continue;

        float d = self.pos.mhtDistanceTo(e->getPos());
        if(d <= effVision) {
            visible.push_back(e);
        }
    }

    for (const auto& w : forcedVisible) {
        auto e = w.lock();
        if (!e || e->isDestroyed()) continue;
        if (std::find(visible.begin(), visible.end(), e) == visible.end()) {
            visible.push_back(e);
        }
    }
}

const std::vector<std::shared_ptr<IAttackable>>& DefaultVisionBehavior::getVisible() const {
    return visible;
}

void DefaultAttackBehavior::update(Unit& u, float dt, const Map& map,
                        const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies,
                        WorldDataContext& data) {
    if(cd > 0) cd -= dt;

    std::shared_ptr<IAttackable> cur = target.lock();
    bool needNewTarget = false;

    if (!cur) {
        needNewTarget = true;
    } else if (cur->isDestroyed()) {
        needNewTarget = true;
    } else {
        bool stillVisible = false;
        for (const auto& e : visibleEnemies) {
            if (e == cur) { stillVisible = true; break; }
        }
        if (!stillVisible) {
            needNewTarget = true;
        }
    }

    if(needNewTarget) {
        cur = findNearest(u, map, visibleEnemies);
        if(!cur) {
            target.reset();
            return;
        }
        target = cur;
    }

    if(inAttackRange(u, map, cur)) {
        if(cd <= 0.f) {
            const Tile& tile = map.getTile(u.pos);
            float dmg = u.baseStats.attack + tile.getAttackBonus();
            float cdBase = 0.8f;

            if (tile.getType() == TileType::HILL) {
                dmg *= 0.85f;
                cdBase *= 1.25f;
            }

            cur->takeDamage(dmg);
            data.revealAttacker(u);
            cd = cdBase;
        }
    }
}

void DefaultAttackBehavior::setTarget(const std::weak_ptr<IAttackable>& t) {
    target = t;
}

std::shared_ptr<IAttackable> DefaultAttackBehavior::findNearest(
    const Unit& u, const Map& map,
    const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies
) const {
    std::shared_ptr<IAttackable> bestBase;
    float bestBaseDist = 1e9f;

    std::shared_ptr<IAttackable> bestUnit;
    float bestUnitDist = 1e9f;

    for(const auto& e : visibleEnemies) {
        if(!e || e->isDestroyed()) continue;
        float d = u.pos.mhtDistanceTo(e->getPos());
        if (e->getAttackType() == AttackableType::BASE) {
            if (d < bestBaseDist) {
                bestBaseDist = d;
                bestBase = e;
            }
        } else {
            if (d < bestUnitDist) {
                bestUnitDist = d;
                bestUnit = e;
            }
        }
    }
    if(bestBase) return bestBase;
    return bestUnit;
} 

bool DefaultAttackBehavior::inAttackRange(
    const Unit& self, const Map& map,
    const std::shared_ptr<IAttackable>& t
) const {
    if(!t) return false;
    const Tile& tile = map.getTile(self.pos);
    float effectiveRange = self.baseStats.attackRange + tile.getVisionBonus();
    float dist = self.pos.mhtDistanceTo(t->getPos());
    if (dist > effectiveRange) return false;

    if (map.hasMountainBetween(self.pos, t->getPos())) return false;
    if (self.baseStats.attackRange <= 2.0f &&
        map.hasRiverBetween(self.pos, t->getPos())) {
        return false;
    }
    return true;
}

UnitBehavior::UnitBehavior() {
    std::random_device rd;
    rng.seed(rd());
    movement = std::make_unique<DefaultMovementBehavior>();
    attack = std::make_unique<DefaultAttackBehavior>();
    command      = std::make_unique<DefaultCommandBehavior>();
    vision       = std::make_unique<DefaultVisionBehavior>();
    stateMachine = std::make_unique<DefaultStateMachine>();
}

void UnitBehavior::issueMove(const Coord& dst) { command->issueMove(dst); }
void UnitBehavior::issueAttack(const std::shared_ptr<IAttackable>& t) { command->issueAttack(t); }
void UnitBehavior::issueStop() { command->issueStop(); }

bool UnitBehavior::isDead() const {
    return lifeState == LifeState::Dead;
}

UnitState UnitBehavior::getState() const {
    return stateMachine->get();
}

void UnitBehavior::setState(UnitState state) {
    stateMachine->set(state);
    switch (state) {
        case UnitState::Dead:
            lifeState = LifeState::Dead;
            locomotionState = LocomotionState::Idle;
            combatState = CombatState::None;
            moveReason = MoveReason::None;
            combatAction = CombatAction::None;
            break;
        case UnitState::Attacking:
            lifeState = LifeState::Alive;
            combatState = CombatState::Engaging;
            combatAction = CombatAction::Attack;
            locomotionState = LocomotionState::Idle;
            moveReason = MoveReason::None;
            break;
        case UnitState::Chasing:
            lifeState = LifeState::Alive;
            combatState = CombatState::Engaging;
            combatAction = CombatAction::Chase;
            locomotionState = LocomotionState::Pathing;
            moveReason = MoveReason::Chase;
            break;
        case UnitState::Moving:
            lifeState = LifeState::Alive;
            locomotionState = LocomotionState::Pathing;
            moveReason = MoveReason::Command;
            combatState = CombatState::None;
            combatAction = CombatAction::None;
            break;
        case UnitState::Wandering:
            lifeState = LifeState::Alive;
            locomotionState = LocomotionState::Pathing;
            moveReason = MoveReason::Wander;
            combatState = CombatState::None;
            combatAction = CombatAction::None;
            break;
        case UnitState::Idle:
        default:
            lifeState = LifeState::Alive;
            locomotionState = LocomotionState::Idle;
            combatState = CombatState::None;
            moveReason = MoveReason::None;
            combatAction = CombatAction::None;
            break;
    }
}

void UnitBehavior::onKilled(Unit& u) {
    stateMachine->set(UnitState::Dead);
    lifeState = LifeState::Dead;
    locomotionState = LocomotionState::Idle;
    combatState = CombatState::None;
    moveReason = MoveReason::None;
    combatAction = CombatAction::None;
    commitTimer = 0.f;
    commitTargetId = -1;
    commitTargetType = AttackableType::UNIT;
    commandAttackActive = false;
    commandAttackTargetId = -1;
    commandAttackTargetType = AttackableType::UNIT;
    retreating = false;
    retreatTimer = 0.f;
    hasRetreatAnchor = false;
}


void UnitBehavior::tickVision(
    Unit& u, const Map& map, 
    const std::vector<std::weak_ptr<IAttackable>>& enemies,
    const std::vector<std::weak_ptr<IAttackable>>& forcedVisible
) {
    if(isDead()) return;
    vision->updateVisible(u, map, enemies, forcedVisible);
}

void UnitBehavior::applyPendingCommand(Unit& u, const Map& map) {
    if (isDead()) return;

    switch (command->pendingType()) {
        case UnitCommandType::MoveTo:
            movement->setMoveTarget(command->pendingMoveTarget(), map, u);
            locomotionState = LocomotionState::Pathing;
            moveReason = MoveReason::Command;
            retreating = false;
            retreatTimer = 0.f;
            hasRetreatAnchor = false;
            stateMachine->set(UnitState::Moving);
            commandMoveActive = true;
            break;
        case UnitCommandType::AttackUnit:
            attack->setTarget(command->pendingAttackTarget());
            combatState = CombatState::Engaging;
            combatAction = CombatAction::Attack;
            locomotionState = LocomotionState::Idle;
            moveReason = MoveReason::None;
            {
                auto target = command->pendingAttackTarget().lock();
                BehaviorUtil::fillCommitTarget(target, commitTargetId, commitTargetType);
                commitTimer = 1.2f;
                commandAttackActive = true;
                BehaviorUtil::fillCommitTarget(target, commandAttackTargetId, commandAttackTargetType);
            }
            retreating = false;
            retreatTimer = 0.f;
            hasRetreatAnchor = false;
            stateMachine->set(UnitState::Attacking);
            commandMoveActive = false;
            break;
        case UnitCommandType::Stop:
            movement->usePath().clear();
            locomotionState = LocomotionState::Idle;
            combatState = CombatState::None;
            moveReason = MoveReason::None;
            combatAction = CombatAction::None;
            commitTimer = 0.f;
            commitTargetId = -1;
            commitTargetType = AttackableType::UNIT;
            commandAttackActive = false;
            commandAttackTargetId = -1;
            commandAttackTargetType = AttackableType::UNIT;
            retreating = false;
            retreatTimer = 0.f;
            hasRetreatAnchor = false;
            stateMachine->set(UnitState::Idle);
            commandMoveActive = false;
            break;
        default:
            break;
    }

    command->clear();
}

void UnitBehavior::updateVision(Unit& u, const Map& map,
                                const std::vector<std::weak_ptr<IAttackable>>& enemies,
                                const std::vector<std::weak_ptr<IAttackable>>& forcedVisible) {
    if (isDead()) return;
    vision->updateVisible(u, map, enemies, forcedVisible);
}

void UnitBehavior::tickMovement(Unit& u, float dt, const Map& map) {
    if(isDead()) return;

    if(locomotionState == LocomotionState::Pathing) {
        movement->update(u, dt, map);

        if(movement->usePath().empty()) {
            locomotionState = LocomotionState::Idle;
            moveReason = MoveReason::None;
            stateMachine->set(UnitState::Idle);
        }
    }
}

void UnitBehavior::tickAttack(
    Unit& u, float dt, const Map& map,
    const std::vector<std::weak_ptr<IAttackable>>& enemies,
    WorldDataContext& data
) {
    if(isDead()) return;

    switch(command->pendingType()) {
        case UnitCommandType::MoveTo:
            movement->setMoveTarget(command->pendingMoveTarget(), map, u);
            stateMachine->set(UnitState::Moving);
            break;
        case UnitCommandType::AttackUnit:
            attack->setTarget(command->pendingAttackTarget());
            stateMachine->set(UnitState::Attacking);
            break;
        case UnitCommandType::Stop:
            movement->usePath().clear();
            stateMachine->set(UnitState::Idle);
            break;
        default:
            break;
    }

    command->clear();

    std::vector<std::weak_ptr<IAttackable>> forcedVisible;
    data.appendForcedReveals(u.owner, forcedVisible);
    vision->updateVisible(u, map, enemies, forcedVisible);
    const auto& visible = vision->getVisible();

    UnitState st = stateMachine->get();

    switch(st) {
        case UnitState::Idle: {
            attack->update(u, dt, map, visible, data);
            auto tWeak = attack->getTarget();
            if(!tWeak.expired()) {
                stateMachine->set(UnitState::Attacking);
                break;
            } else {
                Coord dst = BehaviorUtil::pickRandomWanderTarget(u, map, enemies, rng);
                movement->setMoveTarget(dst, map, u);
                stateMachine->set(UnitState::Wandering);
            }      
            break;
        }     
        case UnitState::Attacking: {
            attack->update(u, dt, map, visible, data);
            auto t = attack->getTarget().lock();
            if(!t || t->isDestroyed()) {
                stateMachine->set(UnitState::Idle);
                break;
            }

            if(!attack->inAttackRange(u, map, t)) {
                stateMachine->set(UnitState::Chasing);
                movement->setMoveTarget(t->getPos(), map, u);
            }
            break;
        }
        case UnitState::Chasing: {
            auto t = attack->getTarget().lock();

            if (!t || t->isDestroyed()) {
                stateMachine->set(UnitState::Idle);
                break;
            }
            Coord curTargetPos = t->getPos();
            // 进入射程 → 切攻击
            if (attack->inAttackRange(u, map, t)) {
                stateMachine->set(UnitState::Attacking);
                break;
            }
            // 只有当目标移动过才重新规划路径
            if (!movement->hasLastTarget() ||
                movement->getLastTarget() != curTargetPos)
            {
                movement->setMoveTarget(curTargetPos, map, u);
                movement->setLastTarget(curTargetPos);
            }
            break;
        }  
        case UnitState::Wandering: {
            attack->update(u, dt, map, visible, data);
            auto tWeak = attack->getTarget();
            if(!tWeak.expired()) {
                stateMachine->set(UnitState::Attacking);
            }

            break;
        }     
        default:
            break;
    }
}

void UnitBehavior::postAttackStateUpdate(
    Unit& u, const Map& map,
    const std::vector<std::weak_ptr<IAttackable>>& enemies
) {
    if (isDead()) return;
    stateMachine->set(BehaviorUtil::mapLegacyState(lifeState, locomotionState, combatState,
                                     moveReason, combatAction));

    if (locomotionState == LocomotionState::Idle &&
        combatState == CombatState::None &&
        !commandMoveActive &&
        !retreating)
    {
        Coord dst = BehaviorUtil::pickRandomWanderTarget(u, map, enemies, rng);
        movement->setMoveTarget(dst, map, u);
        locomotionState = LocomotionState::Pathing;
        moveReason = MoveReason::Wander;
        stateMachine->set(UnitState::Wandering);
    }
}


BaseState DefaultBaseStateMachine::get() const { return state; }
void DefaultBaseStateMachine::set(BaseState s) { state = s; }

void BaseCommandBehavior::issueProduce(UnitType t) {
    pendingQueue.push(t);
}

bool BaseCommandBehavior::hasPending() const {
    return !pendingQueue.empty();
}

UnitType BaseCommandBehavior::nextPending() const {
    return pendingQueue.front();
}

void BaseCommandBehavior::pop() {
    pendingQueue.pop();
}

void BaseCommandBehavior::clear() {
    while (!pendingQueue.empty()) pendingQueue.pop();
}

void PeriodicProductionBehavior::reset(float p) {
    period = p;
    timer = 0.f;
    idx = 0;
}

bool PeriodicProductionBehavior::triggered(float dt) {
    timer += dt;
    if(timer >= period) {
        timer -= period;
        return true;
    }
    return false;
}

UnitType PeriodicProductionBehavior::nextType() {
    UnitType t = cycle[idx];
    idx = (idx + 1) % cycle.size();
    return t;
}

void BaseSpawnBehavior::begin(UnitType t, Base& self) {
    currentType = t;
    cd = self.baseProductTime;  // 从 Base 读取生产时间
}

bool BaseSpawnBehavior::update(Base& self, float dt, WorldDataContext& data) {
    cd -= dt;
    return cd <= 0.f;
}

UnitType BaseSpawnBehavior::type() const {
    return currentType;
}

BaseBehavior::BaseBehavior() {
    spawn        = std::make_unique<BaseSpawnBehavior>();
    command      = std::make_unique<BaseCommandBehavior>();
    stateMachine = std::make_unique<DefaultBaseStateMachine>();
    periodic     = std::make_unique<PeriodicProductionBehavior>();
}

void BaseBehavior::issueProduce(UnitType t) {
    command->issueProduce(t);
}

bool BaseBehavior::isDead() const {
    return stateMachine->get() == BaseState::Dead;
}

void BaseBehavior::reqSpawn(Base& self, WorldDataContext& data,
                            const BaseSystem& baseSystem,
                            UnitType t){
    baseSystem.spawnUnit(t, self, data);
}

void BaseBehavior::onKilled(Base& self) {
    stateMachine->set(BaseState::Dead);
}

void BaseBehavior::update(Base& self, float dt, WorldDataContext& data,
                          const BaseSystem& baseSystem) {
    if(isDead()) return;

    BaseState st = stateMachine->get();

    switch(st) {
        case BaseState::Idle: {
            // Shared cooldown for both queued commands and periodic rotation
            if (!periodic->triggered(dt)) break;

            UnitType t;
            if (command->hasPending()) {
                t = command->nextPending();
                command->pop();
            } else {
                t = periodic->nextType();
            }

            spawn->begin(t, self);
            stateMachine->set(BaseState::Producing);
            break;
        }

        case BaseState::Producing: {
            if(spawn->update(self, dt, data)) {
                UnitType t = spawn->type();
                reqSpawn(self, data, baseSystem, t);
                stateMachine->set(BaseState::Idle);
            }
            break;
        }

        default:
            break;
    }
};


Base::Base(const Coord& p, Faction f) : pos(p), faction(f), hp(maxHp){
        behavior = std::make_unique<BaseBehavior>();
    }

void Base::issueProduce(UnitType t) {
    if (behavior) behavior->issueProduce(t);
}

void Base::update(float dt, WorldDataContext& data, const BaseSystem& baseSystem){
    behavior->update(*this, dt, data, baseSystem);
}

void Base::takeDamage(float dmg) {
    hp -= dmg;
    if(hp <= 0 && !behavior->isDead()){
        hp = 0;
        behavior->onKilled(*this);
    }
}
bool Base::isDestroyed() const {
    return behavior->isDead();
};

Unit::Unit(UnitType t, const Coord& start, Faction faction)
        : pos(start), type(t), owner(faction)
{
    baseStats = UnitStats::getStats(t);
    hp = baseStats.maxHP;

    behavior = std::make_unique<UnitBehavior>();
}

void Unit::issueMove(const Coord& dst) {
    behavior->issueMove(dst);
}

void Unit::issueAttackTarget(const std::shared_ptr<IAttackable>& t) {
    behavior->issueAttack(t);
}

void Unit::issueStop() {
    behavior->issueStop();
}

void Unit::takeDamage(float dmg){
    float actual = std::max(0.f, dmg - baseStats.armor);
    hp -= actual;
    if (hp <= 0.f && !behavior->isDead()) {
        hp = 0.f;
        behavior->onKilled(*this);
    }
}

bool Unit::isDestroyed() const {
    return behavior->isDead();
}

bool Unit::isAlive() const { return !behavior->isDead(); }
