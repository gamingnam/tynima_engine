#include <tynima/core/profile.h>
#include <tynima/scene/systems.h>

namespace tynima::scene {

std::uint32_t collect_draws(World& world, const render::Model* models, std::uint32_t model_count,
                            render::DrawItem* out, std::uint32_t max) noexcept {
    TY_PROFILE_SCOPE_NAMED("scene::collect_draws");
    std::uint32_t count = 0;
    world.each<LocalToWorld, MeshRenderer>([&](Entity, LocalToWorld& local_to_world, MeshRenderer& renderer) {
        if (!renderer.visible || renderer.model >= model_count) {
            return;
        }
        if (count < max) {
            out[count] = {.model = &models[renderer.model], .world = local_to_world.matrix};
        }
        ++count;
    });
    return count;
}

} // namespace tynima::scene
