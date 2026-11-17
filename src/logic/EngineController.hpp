#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

class Engine;
class World;
class ContentReport;
class LevelController;
struct ContentRemovalRequest;

class EngineController {
    Engine& engine;

    int64_t localPlayer = -1;
    void onMissingContent(
        const std::string& worldName,
        const std::shared_ptr<ContentReport>& report
    );
public:
    EngineController(Engine& engine);

    /// @brief Load world, convert if required and set to LevelScreen.
    /// @param name world name
    /// @param confirmConvert automatically confirm convert if requested
    void openWorld(const std::string& name, bool confirmConvert);

    /// @brief Delete world, by default after a confirmation dialog
    /// @param name world name
    /// @param confirm show the dialog first (no dialog in headless mode)
    void deleteWorld(const std::string& name, bool confirm = true);

    void deleteWorldContent(
        const std::string& name, ContentRemovalRequest request, bool loadWorld
    );

    void reconfigPacks(
        LevelController* controller,
        const std::vector<std::string>& packsToAdd,
        const std::vector<std::string>& packsToRemove
    );

    void createWorld(
        const std::string& name,
        const std::string& seedstr,
        const std::string& generatorID
    );

    void setLocalPlayer(int64_t player);

    void reopenWorld(World& world);
};
