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
    while(accumulator >=  1.0f && idx + 1 < path.size()) {
        accumulator -= 1.f;
        u.pos = path[++idx];
    }

    if (idx >= path.size()) {
        // 路径走完后留给上层（UnitBehavior）去把状态改成 Idle
        path.clear();
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

void DefaultAttackBehavior::update(Unit& u, float dt, const Map& map,
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
    IAttackable* bestBase = nullptr;
    float bestBaseDist = 1e9f;

    IAttackable* bestUnit = nullptr;
    float bestUnitDist = 1e9f;

    for(IAttackable* e : visibleEnemies) {
        if(!e || e->isDestroyed()) continue;
        float d = self.pos.mhtDistanceTo(e->getPos());
        if (e->getAttackableType() == AttackableType::Base) {
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
    IAttackable* t
) const {
    if(!t) return false;
    const Tile& tile = map.getTile(self.pos);
    float effectiveRange = self.baseStats.attackRange + tile.getVisionBonus();
    float dist = self.pos.mhtDistanceTo(t->getPos());
    return dist <= effectiveRange;
}

UnitBehavior::UnitBehavior() {
    movement = std::make_unique<DefaultMovementBehavior>();
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


void UnitBehavior::tickVision(
    Unit& u, const Map& map, 
    const std::vector<IAttackable*>& enemies
) {
    if(isDead()) return;
    vision->updateVisible(u, map, enemies);
}

void UnitBehavior::tickMovement(Unit& u, float dt, const Map& map) {
    if(isDead()) return;

    UnitState st = stateMachine->get();

    if(st == UnitState::Moving || st == UnitState::Chasing) {
        movement->update(u, dt, map);

        if(path.empty()) {
            stateMachine->set(UnitState::Idle);
        }
    }
}

void UnitBehavior::tickAttack(
    Unit& u, float dt, const Map& map,
    const std::vector<IAttackable*>& eneimes
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
            movement->setMoveTarget(nullptr, map, u);
            stateMachine->set(UnitState::Idle);
            break;
        default:
            break;
    }

    command->clear();

    vision->updateVisible(u, map, enemies);
    const auto& visible = vision->getVisible();

    UnitState st = stateMachine->get();

    switch(st) {
        case UnitState::Idle:
            attack->update(u, dt, map, visible);
            break;
        case UnitState::Attacking:
            attack->update(u, dt, map, visible);
            IAttackable* t = attack->getTarget();
            if(!t || t->isDestroyed()) {
                stateMachine->set(UnitState::Idle);
                break;
            }

            if(attack->inAttackRange(u, map, t)) {
                stateMachine->set(UnitState::Chasing);
                movement->setMoveTarget(t->getPos(), map, u);
            }
            break;
        case UnitState::Chasing:
            IAttackable* t = attack->getTarget();

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
        default:
            break;
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
}

bool PeriodicProductionBehavior::triggered(float dt) {
    timer += dt;
    if(timer >= period) {
        timer -= period;
        return true;
    }
    return false;
}



void BaseSpawnBehavior::begin(UnitType t, Base& self) {
    currentType = t;
    cd = self.baseProductTime;  // 从 Base 读取生产时间
}

bool BaseSpawnBehavior::update(float dt, Base& self, GameWorld& world) {
    cd -= dt;
    return cd <= 0.f;
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

void BaseBehavior::reqSpawn(Base& self, GameWorld world, UnitType t){
    world.baseSystem.spawnUnit(t, self, world);
}

void BaseBehavior::onKilled(Base& self) {
    stateMachine->set(BaseState::Dead);
}

void BaseBehavior::update(Base& self, float dt, GameWorld& world) {
    if(isDead()) return;

    BaseState st = stateMachine->get();

    switch(st) {
        case BaseState::Idle: {
            // 检查是否有生产命令
            if (command->hasPending()) {
                UnitType t = command->nextPending();
                command->pop();

                spawn->begin(t, self);
                stateMachine->set(BaseState::Producing);
                break;
            }
            
            if(periodic->triggered(dt)) {
                UnitType defaultUnit = UnitType::Infantry;
                spawn->begin(defaultUnit, self);
                stateMachine->set(BaseState::Producing);
            }
            break;
        }

        case BaseState::Producing: {
            if(spawn->update(dt, self)) {
                UnitType t = spawn->type();
                reqSpawn(self, world, t);
                stateMachine->set(BaseState::Idle)
            }
            break;
        }

        default:
            break;
    }
}