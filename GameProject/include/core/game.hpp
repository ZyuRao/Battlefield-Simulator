#pragma once
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include "gameworld.hpp"

class Game {
private:
    bool running = false;
    GameWorld world;
    std::thread inputThread;
    std::atomic<bool> inputRunning{false};

public:
    Game() : running(false) {}
    ~Game() {
        WorldRuntimeContext runtimeCtx(world.runtime);
        runtimeCtx.stop();
        if(inputRunning.load()) {
            inputRunning.store(false);
        }
        if (inputThread.joinable()) {
            inputThread.join();
        }
    }

    void run() {
        running = true;
        std::cout << "[Main] GameProject started.\n";
        std::cout << "[Game] world created\n";

        WorldDataContext dataCtx(world.state);
        WorldRuntimeContext runtimeCtx(world.runtime);
        WorldControlContext controlCtx(world.control);

        runtimeCtx.start(dataCtx, controlCtx, world.systems);
        inputRunning.store(true);
        inputThread = std::thread([this, &controlCtx]() {
            std::string line;
            while (inputRunning.load()) {
                if (!std::cin.rdbuf()->in_avail()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                if (!std::getline(std::cin, line)) break;
                if (line.empty()) continue;
                controlCtx.enqueueCommand(line);
            }
        });
        
        constexpr double fixedHz = 120.0;
        constexpr double fixedDt = 1.0 / fixedHz;
        double accumulator = 0.0;
        auto lastTime = std::chrono::steady_clock::now();
        // TODO(perf): If simulation falls behind, consider batching or reducing system costs.
        while(running) {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> frameTime = now - lastTime;
            lastTime = now;

            double delta = frameTime.count();
            if (delta > 0.25) delta = 0.25;
            accumulator += delta;

            while (accumulator >= fixedDt) {
                world.update(static_cast<float>(fixedDt));
                accumulator -= fixedDt;
            }

            if(runtimeCtx.shouldQuit()) {
                running = false;
            }
            if(dataCtx.baseA->isDestroyed() || dataCtx.baseB->isDestroyed()) {
                running = false;
                std::string winner = !dataCtx.baseA->isDestroyed() ? "A" : "B";
                std::cout << "Winner:" << winner << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        runtimeCtx.markGameOver();
        inputRunning.store(false);
        if (inputThread.joinable()) {
            inputThread.join();
        }
        if (runtimeCtx.isRenderRunning()) {
            runtimeCtx.join();
        }
        runtimeCtx.stop();
    }
};
