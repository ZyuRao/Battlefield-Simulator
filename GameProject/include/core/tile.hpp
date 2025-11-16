# pragma once
#include <string>

enum class TileType{
    PLAIN,
    RIVER,
    MOUNTAIN
};


class Tile {
private:
    TileType type;

public:
    Tile() : type(TileType::PLAIN) {}
    Tile(TileType t) : type(t) {}

    TileType getType() const {
        return type;
    }
    void setType(TileType t) {type = t;}

    bool isPassable() const {
        switch(type) {
            case TileType::PLAIN:
                return true;
            case TileType::MOUNTAIN:
            case TileType::RIVER:
                return false;
        }
        return false;
    }
    
    std::string getSymbol() const {
        switch(type) {
            case TileType::PLAIN : return ".";
            case TileType::MOUNTAIN : return "^";
            case TileType::RIVER : return "~";
        }
        return "?";
    }

    std::string getName() const {
        switch (type) {
            case TileType::PLAIN:     return "plain";
            case TileType::MOUNTAIN:  return "mountain";
            case TileType::RIVER:     return "river";
        }
        return "unknown";
    }
};

// ================== Design Notes ==================
// 模块功能：
// Tile 类用于描述游戏地图中的单个格子，包含地形类型、可通行性判断、字符渲染符号等。
// 该模块是地图系统(Map)的最基本构成单位。

// 设计模式：无特定设计模式。
// 但地形类型使用 enum class 优雅地区分类型，并方便未来扩展其他地形。

// 扩展接口：
// 1. 可轻松添加更多地形，例如 Forest、Road、Swamp 等。
// 2. 可为图形渲染加入纹理ID或颜色信息。
// 3. isPassable() 可扩展为“不同单位可通行不同地形”，例如空军可越过河流。
// 4. getSymbol() 可被图形渲染器替代为图像ID，不修改核心逻辑。