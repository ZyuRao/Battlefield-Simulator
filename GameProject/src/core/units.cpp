#include "core/Iattackable.hpp"
#include <algorithm>
#include <cmath>

static float computeTerrainSpeed(UnitType t, const Tile& tile, float baseSpeed)
{
    float speed = baseSpeed / tile.getMoveCost();

    TileType tt = tile.getType();

    if (t == UnitType::Knight) {
        switch (tt) {
        case TileType::PLAIN:    speed *= 1.10f; break;
        case TileType::FOREST:   speed *= 0.60f; break;
        case TileType::HILL:     speed *= 0.70f; break;
        case TileType::SWAMP:    speed *= 0.40f; break;
        default: break;
        }
    }

    // 步兵、弓箭手暂时不做额外修正，有需要可添加
    return speed;
}

Unit::Unit(UnitType t, const Coord& start)
    : type(t),
      pos(start),
      pathIdx(0),
      pendingCmd(UnitCommandType::None),
      target(nullptr),
      pendingMoveTarget(start),
      attackCd(0.f),
      moveAccumulator(0.f)
{
    baseStats = UnitStats::getStats(t);
    hp = baseStats.maxHP;
    state = UnitState::Idle;
}

void Unit::issueMoveCommand(const Coord& dst) {
    pendingCmd = UnitCommandType::MoveTo;
    pendingMoveTarget = dst;
    target = nullptr;
}

void Unit::issueAttackCommand(Unit* t) {
    pendingCmd = UnitCommandType::AttrackUnit;
    target = t;
}

void Unit::issueStop() {
    pendingCmd = UnitCommandType::Stop;
    path.clear();
    target = nullptr;
    state = UnitState::Idle;
}

void Unit::updateIdle(float, const Map& map, const std::vector<Unit*>& enemies) {
    Unit* e = findNearestEnemy(map, enemies);
    if(e && e->isAlive()) {
        target = e;
        if(inAttackRange(e, map)) state = UnitState::Attacking;
        else {
            state = UnitState::Chasing;
            computePath(e->getPos(), map);
        }
    }
}

void Unit::updateMoving(float dt, const Map& map) {
    if(path.empty()){
        state = UnitState::Idle;
        return;
    }

    moveStep(dt, map);
}

void Unit::updateAttacking(float dt, const Map& map) {
    if(!target || !target->isAlive()) {
        state = UnitState::Idle;
        return;
    }

    if(inAttackRange(target, map) ){
        if(attackCd <= 0){
            float dmg = baseStats.attack +
                        map.getTile(pos).getAttackBonus();
            target->takeDamage(dmg);
            attackCd = 0.8f;
        }       
    }else {
        state = UnitState::Chasing;
        computePath(target->getPos(), map);
    }
}

void Unit::updateChasing(float dt, const Map& map) {
    if (!target || !target->isAlive()) {
        state = UnitState::Idle;
        return;
    }
    if(inAttackRange(target, map)){
        state = UnitState::Attacking;
        return;
    }

    if (path.empty() || target->getPos() != path.back()) {
        computePath(target->getPos(), map);
    }

    moveStep(dt, map);
}

void Unit::computePath(const Coord& dst, const Map& map) {
    path.clear();
    map.findPathAStar(pos, dst, path);
    pathIdx = 0;
}

void Unit::moveStep(float dt, const Map& map) {
    if(pathIdx >= path.size()) {
        state = UnitState::Idle;
        return;
    }
    Coord next = path[pathIdx];
    const Tile& t = map.getTile(next);
    float speed = computeTerrainSpeed(type, t, baseStats.moveSpeed);
    moveAccumulator += dt * speed;
    while(moveAccumulator >= 1.0f && pathIdx < path.size()){
        moveAccumulator -= 1.0f;
        pos = path[pathIdx++];
    }

    if(pathIdx >= path.size()){
        state = UnitState::Idle;
    }    
}

void Unit::takeDamage(float dmg) {
    float actual = std::max(0.f, dmg - baseStats.armor);
    hp -= actual;
    if(hp <= 0) {
        hp = 0;
        state = UnitState::Dead;
    }
}

Unit* Unit::findNearestEnemy(const Map& map, const std::vector<Unit*>& enemies) const {
    const Tile& t = map.getTile(pos);
    float effectiveVision = baseStats.visionRange + t.getVisionBonus();

    Unit* best = nullptr;
    float bestD = 1e9f;
    for(Unit* e : enemies) {
        if(!e->isAlive()) continue;

        float d = pos.mhtDistanceTo(e->getPos());
        if(d <= effectiveVision && d < bestD) {
            best = e;
            bestD = d;
        }
    }

    return best;
}

bool Unit::inAttackRange(Unit* e, const Map& map) const {
    const Tile& t = map.getTile(pos);

    float effectiveRange = baseStats.attackRange
            + (float)t.getVisionBonus();
    float dist = pos.mhtDistanceTo(e->getPos());
    return dist <= effectiveRange;
}

void Unit::update(float dt, const Map& map, const std::vector<Unit*>& enemies) {
    if(!isAlive()) return;
    if(attackCd > 0) attackCd -= dt;

    if(pendingCmd != UnitCommandType::None) {
        if(pendingCmd == UnitCommandType::MoveTo){
            computePath(pendingMoveTarget, map);
            state = UnitState::Moving;
        }
        else if(pendingCmd == UnitCommandType::AttrackUnit) {
            if(target && target->isAlive()) {
                if (inAttackRange(target, map)) {
                    state = UnitState::Attacking;
                } else {
                    state = UnitState::Chasing;
                    computePath(target->getPos(), map);
                }
            }
        }
        else if(pendingCmd == UnitCommandType::Stop) {
            path.clear();
            state = UnitState::Idle;
        }

        pendingCmd == UnitCommandType::None;
    }

    switch (state)
    {
    case UnitState::Idle:      updateIdle(dt, map, enemies); break;
    case UnitState::Moving:    updateMoving(dt, map);        break;
    case UnitState::Attacking: updateAttacking(dt, map);     break;
    case UnitState::Chasing:   updateChasing(dt, map);       break;
    default: break;
    }
}
