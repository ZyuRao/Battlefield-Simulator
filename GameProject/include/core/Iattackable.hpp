# pragma once
#include <vector>
#include <string>
#include "utils/vec2.hpp"
#include "map.hpp"
#include "behavior.hpp"

class GameWorld;

enum class UnitType {
    Infantry, //步兵：中等移动、中等血量、近战
    Archer,   // 弓箭手：远程、血量低
    Knight    // 骑士：高血、高护甲、移动快
};


enum class Faction {
    A,
    B
};

enum AttackableType {
    UNIT,
    BASE
};


struct UnitStats {
    float maxHP;
    float attack;
    float armor;
    float attackRange;
    float visionRange;
    float moveSpeed;

    static UnitStats getStats(UnitType t) {
        switch(t) {
        case UnitType::Infantry:
            return {120.f, 12.f, 2.f, 2.0f, 4.5f, 1.0f};
        case UnitType::Archer:
            return { 80.f, 20.f, 1.f, 6.0f, 7.0f, 1.0f};
        case UnitType::Knight:
            return {120.f, 18.f, 4.f, 2.0f, 5.0f, 1.6f};
        }
        return {100,10,2,1,4,1};
    }
};

class IAttackable {
public:
    virtual void takeDamage(float dmg) = 0;
    virtual bool isDestroyed() const = 0;
    virtual Coord getPos() const = 0;
    virtual AttackableType getAttackType() const = 0;
    virtual ~IAttackable() = default;
    virtual Faction getFaction() const = 0;
};


class Base : public IAttackable {
private:
    Coord pos;
    Faction faction;
    float maxHp = 500;
    float hp;

    float baseProductTime = 3.f;

    std::unique_ptr<BaseBehavior> behavior;

public:
    Base(const Coord& p, Faction f) : pos(p), faction(f) {
        behavior = std::make_unique<BaseBehavior>();
    }

    void update(float dt, GameWorld& world){
        behavior->update(*this, dt, world);
    }


    Coord getPos() const override { return pos; }
    AttackableType getAttackableType() const override {
        return AttackableType::Base;
    }
    Faction getFaction() const { returm faction}

    std::string getSymbol() const {
        return faction == Faction::A ? "A" : "B";
    }

    void takeDamage(float dmg) override {
        hp -= dmg;
        if(hp <= 0 && !behavior->isDead()){
            hp = 0;
            behavior->onKilled(*this);
        }
    }
    bool isDestroyed() const override {
        return behavior->isDead();
    };
};

class Unit : public IAttackable {
public: 
    UnitType type;
    Coord pos;
    float hp;

    UnitStats baseStats;
    Faction owner;

    std::unique_ptr<UnitBehavior> behavior;
    //行为策略组合

public:
   Unit(UnitType t, const Coord& start, Faction faction)
        : pos(start), type(t), owner(faction)
    {
        baseStats = UnitStats::getStats(t);
        hp = baseStats.maxHP;

        behavior = std::make_unique<UnitBehavior>();
    }

    Coord getPos() const override { return pos; }

    AttackableType getAttackType() const override {
        return AttackableType::UNIT;
    }

    Faction getFaction() const { return owner; }
    void takeDamage(float dmg) override {
        float actual = std::max(0.f, dmg - baseStats.armor);
        hp -= actual;
        if (hp <= 0.f && !behavior->isDead()) {
            hp = 0.f;
            behavior->onKilled(*this);
        }
    }

    bool isDestroyed() const override {
        return behavior->isDead();
    }

    bool isAlive() const { return !behavior->isDead(); }

    std::string getSymbol() const {
        switch (type) {
            case UnitType::Infantry: return "I";
            case UnitType::Archer:   return "A";
            case UnitType::Knight:   return "K";
        }
        return "?";
    }
    
    void update(float dt, const Map& map, const std::vector<IAttackable*>& enemies) {
        behavior->update(*this, dt, map, enemies);
    }
};