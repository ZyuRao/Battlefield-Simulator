#include "behavior.hpp"
#include <memory>

static float terrainSpend(Unit& u, const Tile& tile) {
    float s = u.baseStats.moveSpeed / tile.getMoveCost();
    if(t == UnitType::Knight) {
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

void DefaultCommandBehavior::issueAttack(IAttackable* t) {
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

IAttackable* DefaultCommandBehavior::pendingAttackTarget() const {
    return attackTarget;
}

void DefaultCommandBehavior::clear() {
    pending = UnitCommandType::None;
    attackTarget = nullptr;
}

void DefaultMovementBehavior::setMoveTarget(const Coord& dst, const Map& map, Unit& u){
    path.clear();
    map.findPathAStar(u.pos, dst, path);
    idx = 0;
    accumulator = 0.0f;
}

void DefaultMovementBehavior::update(Unit& u, float dt, const Map& map) {
    if(idx > path.size()) return;

    float sp - terrainSpend(u, map.getTile(u.pos));

    accumulator += sp * dt;
    while(accumulator >=  1.0f) {
        accumulator -= 1.f;
        u.pos = path[++idx];
    }
}

void DefaultVisionBehavior::updateVisible(Unit& self, const Map& map,
                                          const std::vector<IAttackable*>& allEnemies)
{
    visible.clear();

    const Tile& t = map.getTile(self.pos);
    float effVision = self.baseStats.visionRange + t.getVisionBonus();

    for(IAttackable* e : allEnemies) {
        if(!e || e-> isDestroyed()) continue;
        float d = self.pos.mhtDistanceTo(e->getPos());
        if(d <= effVision) {
            visible.push_back(e);
        }
    }
}

const std::vector<IAttackable*>& DefaultVisionBehavior::getVisible() const {
    return visible;
}

void DefaultAttackBehavior::update(Unit& u, float dt, const Map& map
        const std::vector<IAttackable*>& visibleEnemies) {
    if(cd > 0) cd -= dt;

    bool needNewTarget = false;

    if (!target || target->isDestroyed()) {
        needNewTarget = true;
    } else {
        // 检查 target 是否依然在可见列表中（离开视野也算丢失）
        bool stillVisible = false;
        for (IAttackable* e : visibleEnemies) {
            if (e == target) { 
                stillVisible = true; 
                break; 
            }
        }
        if (!stillVisible) {
            needNewTarget = true;
        }
    }

    if(needNewTarget) {
        taregt = findNearest(u, map, visibleEnemies);
        if(!target) return;
    }

    if(inAttackRange(u, map, target)) {
        if(cd <= 0.f) {
            float dmg = u.baseStats.attack + map.getTile(u.pos).getAttackBonus();
            target->takeDamage(dmg);
            cd = 0.8f;
        }
    }
}

void DefaultAttackBehavior::setTarget(IAttackable* t) {
    target = t;
}

IAttackable* DefaultAttackBehavior::findNearest(
    const Unit& u, const Map& map,
    const std::vector<IAttackable*>& visibleEnemies
) const {
    IAttackable* best = nullptr;
    float bestDist = 1e9f;

    for(IAttackable* e : visibleEnemies) {
        if(!e || e->isDestroyed()) continue;
        float d = self.pos.mhtDistanceTo(e->getPos());
        if(d < bestDist) {
            bestDist = d;
            best = e;
        }
    }
    return best;
} 

bool DefaultAttackBehavior::inAttackRange(
    const Unit& self, const Map& map,
    IAttackable* t
) const {
    if(!t) return false;
    const Tile& tile = map.getTile(self.pos);
    float effectiveRange = self.baseStats.attackRange + tile.getVisionBonus();
    float dist = self.pos.mhtDistanceTo(t->getPos());
    return dist <= effectiveRange;
}

UnitBehavior::UnitBehavior() {
    movement = std::make_unique<DefaultMovementBehavior>;
    attack = std:::make_unique<DefaultAttackBehavior>();
    command      = std::make_unique<DefaultCommandBehavior>();
    vision       = std::make_unique<DefaultVisionBehavior>();
    stateMachine = std::make_unique<DefaultStateMachine>();
}

void UnitBehavior::issueMove(const Coord& dst) { command->issueMove(dst); }
void UnitBehavior::issueAttack(IAttackable* t) { command->issueAttack(t); }
void UnitBehavior::issueStop() { command->issueStop(); }

bool UnitBehavior::isDead() const {
    return stateMachine->get() == UnitState::Dead;
}

void UnitBehavior::onKilled(Unit& u) {
    stateMachine->set(UnitState::Dead);
}


void UnitBehavior::update(Unit& u, float dt, const Map& map
                         std::vector<IAttackable*> enemies)
{
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
            stateMachine->set(UnitState::Idle);
            break;

        default:
            break;
    }

    command->clear();
    vision->updateVisible(u, map, allEnemies);
    const auto& visible = vision->getVisible();
    
    switch (stateMachine->get()) {
        case UnitState::Idle:
            attack->update(u, dt, map);
            break;
        case UnitState::Moving:
            movement->update(u, dt, map);
            break;
        case UnitState::Attacking:
            attack->update(u, dt, map);
            break;
        case UnitState::Chasing:
            movement->update(u, dt, map);
            break;
        default:
            break;
    }

}