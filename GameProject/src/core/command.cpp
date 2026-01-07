 #include "core/command.hpp"
 #include "core/gameworld.hpp"

 #include <algorithm>
 #include <cctype>
 #include <sstream>

struct CommandParser {
    static std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    static bool parseInt(const std::string& token, int& out) {
        try {
            size_t idx = 0;
            int v = std::stoi(token, &idx);
            if (idx != token.size()) return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    }

    static bool parseUnitTypeToken(const std::string& token, UnitType& out) {
        const std::string t = toLower(token);
        if (t == "infantry" || t == "i") {
            out = UnitType::Infantry;
            return true;
        }
        if (t == "archer" || t == "a") {
            out = UnitType::Archer;
            return true;
        }
        if (t == "knight" || t == "k") {
            out = UnitType::Knight;
            return true;
        }
        return false;
    }

    static bool parseFactionToken(const std::string& token, Faction& out) {
        const std::string t = toLower(token);
        if (t == "a" || t == "factiona" || t == "basea") {
            out = Faction::A;
            return true;
        }
        if (t == "b" || t == "factionb" || t == "baseb") {
            out = Faction::B;
            return true;
        }
        return false;
    }
};

void CommandQueue::push(const std::string& line) {
    std::lock_guard<std::mutex> lock(mtx);
    lines.push(line);
}

bool CommandQueue::tryPop(std::string& out) {
    std::lock_guard<std::mutex> lock(mtx);
    if (lines.empty()) return false;
    out = std::move(lines.front());
    lines.pop();
    return true;
}

bool parseCommand(const std::string& line, Command& out, std::string& err) {
    err.clear();
    std::istringstream iss(line);
    std::string cmdToken;
    if (!(iss >> cmdToken)) {
        err = "empty command";
        return false;
    }

cmdToken = CommandParser::toLower(cmdToken);

    if (cmdToken == "produce" || cmdToken == "p") {
        std::string who;
        std::string typeToken;
        if (!(iss >> who >> typeToken)) {
            err = "usage: produce <A|B|baseId> <infantry|archer|knight>";
            return false;
        }
        out = Command{};
        out.type = CommandType::Produce;
    if (!CommandParser::parseFactionToken(who, out.baseFaction)) {
        if (!CommandParser::parseInt(who, out.baseId)) {
            err = "unknown base target";
            return false;
        }
    }
    if (!CommandParser::parseUnitTypeToken(typeToken, out.produceType)) {
        err = "unknown unit type";
        return false;
    }
    return true;
    }

    if (cmdToken == "move" || cmdToken == "m") {
        int id = -1;
        int x = 0, y = 0;
        std::string idToken;
    if (!(iss >> idToken >> x >> y) || !CommandParser::parseInt(idToken, id)) {
        err = "usage: move <unitId> <x> <y>";
        return false;
    }
        out = Command{};
        out.type = CommandType::Move;
        out.unitId = id;
        out.coord = Coord{x, y};
        out.hasCoord = true;
        return true;
    }

    if (cmdToken == "attack" || cmdToken == "a") {
        int attackerId = -1;
        std::string targetToken;
        if (!(iss >> attackerId >> targetToken)) {
            err = "usage: attack <unitId> <targetId|baseA|baseB>";
            return false;
        }
        out = Command{};
        out.type = CommandType::Attack;
        out.unitId = attackerId;

        Faction fac;
    if (CommandParser::parseFactionToken(targetToken, fac)) {
        out.targetIsBase = true;
        out.baseFaction = fac;
        return true;
    }
    int tid = -1;
    if (!CommandParser::parseInt(targetToken, tid)) {
        err = "invalid target id";
        return false;
    }
    out.targetId = tid;
    return true;
    }

    if (cmdToken == "stop" || cmdToken == "s") {
        int id = -1;
        std::string idToken;
    if (!(iss >> idToken) || !CommandParser::parseInt(idToken, id)) {
        err = "usage: stop <unitId>";
        return false;
    }
        out = Command{};
        out.type = CommandType::Stop;
        out.unitId = id;
        return true;
    }

    if (cmdToken == "select") {
        int id = -1;
        std::string idToken;
    if (!(iss >> idToken) || !CommandParser::parseInt(idToken, id)) {
        err = "usage: select <unitId>";
        return false;
    }
        out = Command{};
        out.type = CommandType::Select;
        out.unitId = id;
        return true;
    }

    if (cmdToken == "clear" || cmdToken == "deselect") {
        out = Command{};
        out.type = CommandType::DeselectAll;
        return true;
    }

    if (cmdToken == "quit" || cmdToken == "exit") {
        out = Command{};
        out.type = CommandType::Quit;
        return true;
    }

    err = "unknown command";
    return false;
}

CommandResult executeCommand(const Command& cmd,
                            WorldDataContext& data,
                            WorldControlContext& control,
                            WorldRuntimeContext& runtime) {
    CommandResult res;
    res.ok = false;

    switch (cmd.type) {
        case CommandType::Produce: {
            auto base = data.findBase(cmd.baseId, cmd.baseFaction);
            if (!base) {
                res.message = "base not found";
                return res;
            }
            base->issueProduce(cmd.produceType);
            res.ok = true;
            res.normalized = "produce";
            res.message = "queued unit";
            return res;
        }
        case CommandType::Move: {
            auto unit = data.findUnit(cmd.unitId);
            if (!unit || !cmd.hasCoord) {
                res.message = "unit not found or coord missing";
                return res;
            }
            unit->issueMove(cmd.coord);
            res.ok = true;
            res.normalized = "move";
            res.message = "unit moving";
            return res;
        }
        case CommandType::Attack: {
            auto unit = data.findUnit(cmd.unitId);
            if (!unit) {
                res.message = "attacker not found";
                return res;
            }

            std::shared_ptr<IAttackable> target;
            if (cmd.targetIsBase) {
                auto b = data.findBase(-1, cmd.baseFaction);
                if (b) target = b;
            } else if (cmd.targetId >= 0) {
                target = data.findAttackable(cmd.targetId);
            }

            if (!target) {
                res.message = "target not found";
                return res;
            }

            unit->issueAttackTarget(target);
            res.ok = true;
            res.normalized = "attack";
            res.message = "attack order set";
            return res;
        }
        case CommandType::Stop: {
            auto unit = data.findUnit(cmd.unitId);
            if (!unit) {
                res.message = "unit not found";
                return res;
            }
            unit->issueStop();
            res.ok = true;
            res.normalized = "stop";
            res.message = "unit stopped";
            return res;
        }
        case CommandType::Select: {
            auto unit = data.findUnit(cmd.unitId);
            if (!unit) {
                res.message = "unit not found";
                return res;
            }
            control.setSelection({unit->id});
            res.ok = true;
            res.normalized = "select";
            res.message = "selected unit " + std::to_string(unit->id);
            return res;
        }
        case CommandType::DeselectAll: {
            control.clearSelection();
            res.ok = true;
            res.normalized = "deselect";
            res.message = "selection cleared";
            return res;
        }
        case CommandType::Quit: {
            runtime.requestQuit();
            res.ok = true;
            res.normalized = "quit";
            res.message = "user quit";
            return res;
        }
    }

     res.message = "unsupported command";
     return res;
 }
