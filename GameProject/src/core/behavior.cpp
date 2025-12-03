#include "core/behavior.hpp"
#include "core/gameworld.hpp"
#include "core/Iattackable.hpp"
#include "core/map.hpp"
#include "utils/vec2.hpp"
#include <memory>


class GameWorld;

namespace {
    Coord pickRandomWanderTarget(const Unit& u, const Map& map, std::mt19937& rng){
        std::uniform_int_distribution<int> dx(-5, 5);
        std::uniform_int_distribution<int> dy(-5, 5);

        Coord start = u.getPos();

        for(int tries = 0; tries < 20; tries++) {
            Coord cand{start.x + dx(rng), start.y + dy(rng)};
            if(!map.inBounds(cand)) continue;

            if(!map.getTile(cand).isPassable()) continue;

            if(map.isReachable(start, cand)) return cand;
        }

        return start;
    }
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

void DefaultVisionBehavior::updateVisible(Unit& self, const Map& map,
                                          const std::vector<std::weak_ptr<IAttackable>>& allEnemies)
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
}

const std::vector<std::shared_ptr<IAttackable>>& DefaultVisionBehavior::getVisible() const {
    return visible;
}

void DefaultAttackBehavior::update(Unit& u, float dt, const Map& map,
        const std::vector<std::shared_ptr<IAttackable>>& visibleEnemies) {
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
            float dmg = u.baseStats.attack + map.getTile(u.pos).getAttackBonus();
            cur->takeDamage(dmg);
            cd = 0.8f;
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
    return dist <= effectiveRange;
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
    return stateMachine->get() == UnitState::Dead;
}

void UnitBehavior::onKilled(Unit& u) {
    stateMachine->set(UnitState::Dead);
}


void UnitBehavior::tickVision(
    Unit& u, const Map& map, 
    const std::vector<std::weak_ptr<IAttackable>>& enemies
) {
    if(isDead()) return;
    vision->updateVisible(u, map, enemies);
}

void UnitBehavior::tickMovement(Unit& u, float dt, const Map& map) {
    if(isDead()) return;

    UnitState st = stateMachine->get();

    if(st == UnitState::Moving || st == UnitState::Chasing || st == UnitState::Wandering) {
        movement->update(u, dt, map);

        if(movement->usePath().empty()) {
            stateMachine->set(UnitState::Idle);
        }
    }
}

void UnitBehavior::tickAttack(
    Unit& u, float dt, const Map& map,
    const std::vector<std::weak_ptr<IAttackable>>& enemies
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

    vision->updateVisible(u, map, enemies);
    const auto& visible = vision->getVisible();

    UnitState st = stateMachine->get();

    switch(st) {
        case UnitState::Idle: {
            attack->update(u, dt, map, visible);
            auto tWeak = attack->getTarget();
            if(!tWeak.expired()) {
                stateMachine->set(UnitState::Attacking);
                break;
            } else {
                Coord dst = pickRandomWanderTarget(u, map, rng);
                movement->setMoveTarget(dst, map, u);
                stateMachine->set(UnitState::Wandering);
            }      
            break;
        }     
        case UnitState::Attacking: {
            attack->update(u, dt, map, visible);
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
            attack->update(u, dt, map, visible);
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

bool BaseSpawnBehavior::update(Base& self, float dt, GameWorld& world) {
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

void BaseBehavior::reqSpawn(Base& self, GameWorld& world, UnitType t){
    world.getBaseSystem().spawnUnit(t, self, world);
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
                UnitType t = periodic->nextType();
                spawn->begin(t, self);
                stateMachine->set(BaseState::Producing);
            }
            break;
        }

        case BaseState::Producing: {
            if(spawn->update(self, dt, world)) {
                UnitType t = spawn->type();
                reqSpawn(self, world, t);
                stateMachine->set(BaseState::Idle);
            }
            break;
        }

        default:
            break;
    }
};


Base::Base(const Coord& p, Faction f) : pos(p), faction(f) {
        behavior = std::make_unique<BaseBehavior>();
    }

void Base::update(float dt, GameWorld& world){
    behavior->update(*this, dt, world);
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