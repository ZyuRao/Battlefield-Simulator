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

    struct AttackableKey {
        int id = -1;
        AttackableType type = AttackableType::UNIT;
    };

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

    std::vector<std::shared_ptr<IAttackable>> collectVisibleEnemies(
        const Unit& unit,
        const Map& map,
        const std::vector<std::weak_ptr<IAttackable>>& enemies,
        const std::vector<std::weak_ptr<IAttackable>>& forcedVisible)
    {
        std::vector<std::shared_ptr<IAttackable>> visible;
        if (!map.inBounds(unit.getPos())) return visible;

        const Tile& tile = map.getTile(unit.getPos());
        float effVision = unit.baseStats.visionRange + tile.getVisionBonus();

        for (const auto& w : enemies) {
            auto e = w.lock();
            if (!e || e->isDestroyed()) continue;
            Coord p = e->getPos();
            if (!map.inBounds(p)) continue;
            float d = unit.getPos().mhtDistanceTo(p);
            if (d <= effVision) {
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

        return visible;
    }

    int pickTargetId(const Unit& unit,
                     const std::vector<std::shared_ptr<IAttackable>>& visible) {
        std::shared_ptr<IAttackable> bestBase;
        float bestBaseDist = 1e9f;
        std::shared_ptr<IAttackable> bestUnit;
        float bestUnitDist = 1e9f;

        for (const auto& e : visible) {
            if (!e || e->isDestroyed()) continue;
            float d = unit.getPos().mhtDistanceTo(e->getPos());
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

        if (bestBase) return attackableId(bestBase);
        return attackableId(bestUnit);
    }

    bool isVisibleTarget(const std::shared_ptr<IAttackable>& target,
                         const std::vector<std::shared_ptr<IAttackable>>& visible) {
        if (!target) return false;
        for (const auto& v : visible) {
            if (v == target) return true;
        }
        return false;
    }

    AttackIntent planAttackIntent(const Unit& unit,
                                  const IAttackBehavior& attackBehavior,
                                  UnitState state,
                                  float dt,
                                  const Map& map,
                                  const std::vector<std::weak_ptr<IAttackable>>& enemies,
                                  const std::vector<std::weak_ptr<IAttackable>>& forcedVisible) {
        AttackIntent intent;
        intent.attackerId = unit.id;
        const float cd = attackBehavior.getCooldown();
        intent.nextCooldown = cd;

        auto currentTarget = attackBehavior.getTarget().lock();
        AttackableKey currentKey = attackableKey(currentTarget);
        intent.nextTargetId = currentKey.id;
        intent.nextTargetType = currentKey.type;

        if (state != UnitState::Idle &&
            state != UnitState::Attacking &&
            state != UnitState::Wandering) {
            return intent;
        }

        float cdAfter = cd;
        if (cdAfter > 0.f) {
            cdAfter -= dt;
        }

        std::vector<std::shared_ptr<IAttackable>> visible =
            collectVisibleEnemies(unit, map, enemies, forcedVisible);

        bool needNewTarget = false;
        if (!currentTarget) {
            needNewTarget = true;
        } else if (currentTarget->isDestroyed()) {
            needNewTarget = true;
        } else if (!isVisibleTarget(currentTarget, visible)) {
            needNewTarget = true;
        }

        if (needNewTarget) {
            currentTarget = attackBehavior.findNearest(unit, map, visible);
        }

        if (!currentTarget) {
            intent.nextCooldown = cdAfter;
            intent.nextTargetId = -1;
            return intent;
        }

        currentKey = attackableKey(currentTarget);
        intent.nextTargetId = currentKey.id;
        intent.nextTargetType = currentKey.type;

        if (attackBehavior.inAttackRange(unit, map, currentTarget) && cdAfter <= 0.f) {
            const Tile& tile = map.getTile(unit.getPos());
            float dmg = unit.baseStats.attack + tile.getAttackBonus();
            float cdBase = 0.8f;

            if (tile.getType() == TileType::HILL) {
                dmg *= 0.85f;
                cdBase *= 1.25f;
            }

            intent.didAttack = true;
            intent.targetId = intent.nextTargetId;
            intent.targetType = intent.nextTargetType;
            intent.damage = dmg;
            intent.nextCooldown = cdBase;
            return intent;
        }

        intent.nextCooldown = cdAfter;
        return intent;
    }

    std::uint64_t packCoord(const Coord& c) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y)));
    }

    float movementSpend(const Unit& unit, const Tile& tile) {
        float s = unit.baseStats.moveSpeed / tile.getMoveCost();
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

    MoveIntent planMoveIntent(const Unit& unit,
                              const IMovementBehavior& movement,
                              UnitState state,
                              float dt,
                              const Map& map,
                              bool commandMove) {
        MoveIntent intent;
        intent.unitId = unit.id;
        intent.from = unit.getPos();
        intent.to = unit.getPos();
        intent.commandMove = commandMove;

        if (state != UnitState::Moving &&
            state != UnitState::Chasing &&
            state != UnitState::Wandering) {
            return intent;
        }

        IMovementBehavior::MovementState nextState = movement.snapshot();
        if (nextState.path.empty() || nextState.idx >= nextState.path.size()) {
            intent.nextState = std::move(nextState);
            intent.setIdle = true;
            return intent;
        }

        Coord nextPos = unit.getPos();
        float sp = movementSpend(unit, map.getTile(unit.getPos()));
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
    std::vector<std::shared_ptr<Unit>> unitsASnap;
    std::vector<std::shared_ptr<Unit>> unitsBSnap;
    std::vector<std::weak_ptr<IAttackable>> enemiesASnap;
    std::vector<std::weak_ptr<IAttackable>> enemiesBSnap;
    std::vector<ForcedReveal> forcedASnap;
    std::vector<ForcedReveal> forcedBSnap;
    {
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        unitsASnap = unitsA;
        unitsBSnap = unitsB;
        enemiesASnap = enemiesA;
        enemiesBSnap = enemiesB;
        forcedASnap = forcedVisibleForA;
        forcedBSnap = forcedVisibleForB;
    }

    std::vector<std::weak_ptr<IAttackable>> forcedA;
    std::vector<std::weak_ptr<IAttackable>> forcedB;
    forcedA.reserve(forcedASnap.size());
    forcedB.reserve(forcedBSnap.size());
    for (const auto& r : forcedASnap) {
        if (!r.target.expired()) forcedA.push_back(r.target);
    }
    for (const auto& r : forcedBSnap) {
        if (!r.target.expired()) forcedB.push_back(r.target);
    }

    // Plan stage (Vision): tasks read snapshots only; no world writes here.
    auto visionGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> visionLocals(localCount);

    auto scheduleVision = [&](const std::shared_ptr<Unit>& unit,
                              const std::vector<std::weak_ptr<IAttackable>>& enemies,
                              const std::vector<std::weak_ptr<IAttackable>>& forced) {
        if (!unit || !unit->isAlive()) return;
        taskPool.submit([&, unit]() {
            const int unitId = unit->id;
            std::vector<std::shared_ptr<IAttackable>> visible =
                collectVisibleEnemies(*unit, map, enemies, forced);

            VisionIntent visionIntent;
            visionIntent.unitId = unitId;
            visionIntent.visibleEnemyIds.reserve(visible.size());
            for (const auto& e : visible) {
                int id = attackableId(e);
                if (id >= 0) visionIntent.visibleEnemyIds.push_back(id);
            }

            TargetHint hint;
            hint.unitId = unitId;
            hint.targetId = pickTargetId(*unit, visible);

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
        scheduleVision(u, enemiesASnap, forcedA);
    }
    for (const auto& u : unitsBSnap) {
        scheduleVision(u, enemiesBSnap, forcedB);
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

    // Snapshot stage for Movement planning (post-command).
    std::vector<std::shared_ptr<Unit>> unitsASnapMove;
    std::vector<std::shared_ptr<Unit>> unitsBSnapMove;
    {
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        unitsASnapMove = unitsA;
        unitsBSnapMove = unitsB;
    }

    // Plan stage (Movement): tasks read snapshots only; no world writes here.
    auto moveGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> moveLocals(localCount);

    auto scheduleMove = [&](const std::shared_ptr<Unit>& unit) {
        if (!unit || !unit->isAlive() || !unit->behavior) return;
        taskPool.submit([&, unit]() {
            const IMovementBehavior* movement = unit->behavior->getMovementBehavior();
            if (!movement) return;
            UnitState state = unit->behavior->getState();
            if (state != UnitState::Moving &&
                state != UnitState::Chasing &&
                state != UnitState::Wandering) {
                return;
            }
            bool commandMove = unit->behavior->isCommandMoveActive();
            MoveIntent intent = planMoveIntent(*unit, *movement, state, dt, map, commandMove);

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= moveLocals.size()) {
                idx = 0;
            }
            moveLocals[idx].moveIntents.push_back(std::move(intent));
        }, moveGroup);
    };

    for (const auto& u : unitsASnapMove) {
        scheduleMove(u);
    }
    for (const auto& u : unitsBSnapMove) {
        scheduleMove(u);
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
            if (intent.setIdle) {
                unit->behavior->setState(UnitState::Idle);
                unit->behavior->setCommandMoveActive(false);
            }
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
    }

    // Snapshot stage for Attack planning (post-move/commands).
    std::vector<std::shared_ptr<Unit>> unitsASnapAtk;
    std::vector<std::shared_ptr<Unit>> unitsBSnapAtk;
    std::vector<std::weak_ptr<IAttackable>> enemiesASnapAtk;
    std::vector<std::weak_ptr<IAttackable>> enemiesBSnapAtk;
    std::vector<ForcedReveal> forcedASnapAtk;
    std::vector<ForcedReveal> forcedBSnapAtk;
    {
        std::shared_lock<std::shared_mutex> lock(worldMutex);
        unitsASnapAtk = unitsA;
        unitsBSnapAtk = unitsB;
        enemiesASnapAtk = enemiesA;
        enemiesBSnapAtk = enemiesB;
        forcedASnapAtk = forcedVisibleForA;
        forcedBSnapAtk = forcedVisibleForB;
    }

    std::vector<std::weak_ptr<IAttackable>> forcedAAtk;
    std::vector<std::weak_ptr<IAttackable>> forcedBAtk;
    forcedAAtk.reserve(forcedASnapAtk.size());
    forcedBAtk.reserve(forcedBSnapAtk.size());
    for (const auto& r : forcedASnapAtk) {
        if (!r.target.expired()) forcedAAtk.push_back(r.target);
    }
    for (const auto& r : forcedBSnapAtk) {
        if (!r.target.expired()) forcedBAtk.push_back(r.target);
    }

    // Plan stage (Attack): tasks read snapshots only; no world writes here.
    auto attackGroup = std::make_shared<TaskGroup>();
    std::vector<IntentBuffer> attackLocals(localCount);

    auto scheduleAttack = [&](const std::shared_ptr<Unit>& unit,
                              const std::vector<std::weak_ptr<IAttackable>>& enemies,
                              const std::vector<std::weak_ptr<IAttackable>>& forced) {
        if (!unit || !unit->isAlive() || !unit->behavior) return;
        taskPool.submit([&, unit]() {
            const IAttackBehavior* attackBehavior = unit->behavior->getAttackBehavior();
            if (!attackBehavior) return;
            UnitState state = unit->behavior->getState();
            AttackIntent intent = planAttackIntent(*unit, *attackBehavior, state, dt, map,
                                                   enemies, forced);

            std::size_t idx = TaskPool::workerIndex();
            if (idx == TaskPool::kInvalidWorkerIndex || idx >= attackLocals.size()) {
                idx = 0;
            }
            attackLocals[idx].attackIntents.push_back(std::move(intent));
        }, attackGroup);
    };

    for (const auto& u : unitsASnapAtk) {
        scheduleAttack(u, enemiesASnapAtk, forcedAAtk);
    }
    for (const auto& u : unitsBSnapAtk) {
        scheduleAttack(u, enemiesBSnapAtk, forcedBAtk);
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
                return findBase(id, Faction::A);
            }
            return findUnit(id);
        };

        std::map<std::pair<int, int>, float> damageByTarget;
        std::vector<int> attackersToReveal;
        attackersToReveal.reserve(mergedAttacks.size());

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

            if (intent.didAttack && intent.targetId >= 0) {
                auto key = std::make_pair(static_cast<int>(intent.targetType), intent.targetId);
                damageByTarget[key] += intent.damage;
                attackersToReveal.push_back(intent.attackerId);
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
