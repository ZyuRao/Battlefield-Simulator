#pragma once
#include <vector>
#include "map.hpp"
#include "Iattackable.hpp"

class GameWorld {
private:
    Map map;
    Base baseA;
    Base baseB;

    std::vector<Unit*> unitsA;
    std::vector<Unit*> unitsB;

public:
    GameWorld(int w, int h, const Coord& baseAPos, const Coord& baseBPos);

    void update(float dt);
    void render() const;
    void addUnit() const;

    bool isFree(const Coord& c) const;


    const std::vector<Unit*>& getEnemies(Faction f) const;

    const Map& getMap() const {
        return map;
    }


};