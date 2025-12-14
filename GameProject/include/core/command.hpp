#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

#include "core/Iattackable.hpp"
#include "utils/vec2.hpp"

class GameWorld;

enum class CommandType {
    Produce,
    Move,
    Attack,
    Stop,
    Select,
    DeselectAll,
    Quit
};

struct Command {
    CommandType type = CommandType::Move;

    // common
    int unitId = -1;

    // produce
    Faction baseFaction = Faction::A;
    int baseId = -1;
    UnitType produceType = UnitType::Infantry;

    // move/attack
    Coord coord{};
    bool hasCoord = false;

    int targetId = -1;       // for attack target (unit/base)
    bool targetIsBase = false;
};

// 线程安全队列，用于从不同线程推送命令
class CommandQueue {
private:
    std::mutex              mtx;
    std::queue<std::string> lines;

public:
    void push(const std::string& line);
    bool tryPop(std::string& out);
};

// 命令解析与执行的返回信息
struct CommandResult {
    bool        ok = false;
    std::string message;
    std::string normalized;
};

bool parseCommand(const std::string& line, Command& out, std::string& err);
CommandResult executeCommand(const Command& cmd, GameWorld& world);
