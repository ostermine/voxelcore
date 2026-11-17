#include "sensors.hpp"

void physics::update_sensors(
    Hitbox& hitbox, const std::vector<Sensor*>& sensors
) {
    auto aabb = hitbox.getAABB();

    for (size_t i = 0; i < sensors.size(); i++) {
        auto& sensor = *sensors[i];
        if (sensor.entity == hitbox.entity) {
            continue;
        }
        bool triggered = false;
        switch (sensor.type) {
            case SensorType::AABB:
                triggered = aabb.intersects(sensor.calculated.aabb);
                break;
            case SensorType::RADIUS:
                triggered = glm::distance2(
                    hitbox.position, glm::vec3(sensor.calculated.radial))
                    < sensor.calculated.radial.w;
                break;
        }
        if (!triggered) {
            continue;
        }
        // once per step: a body moved by PhysicsSolver::stepOne (a player
        // driven by its client's commands, netgame) comes here for every
        // command - a dropped item was picked up as many times
        if (sensor.prevEntered.find(hitbox.entity) == sensor.prevEntered.end() &&
            sensor.nextEntered.find(hitbox.entity) == sensor.nextEntered.end()) {
            sensor.enterCallback(sensor.entity, sensor.index, hitbox.entity);
        }
        sensor.nextEntered.insert(hitbox.entity);
    }
}
