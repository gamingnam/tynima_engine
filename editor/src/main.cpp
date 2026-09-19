// The editor. A client of the SDK with no special privileges: it sees the
// engine through tynima.h — the same surface a game's runtime gets — and
// draws itself with Dear ImGui, which the engine hosts. If something the
// editor needs is not in tynima.h, tynima.h is incomplete; that rule is
// what keeps the SDK honest, and tools/check_layering.py enforces it.
//
//   tynima-editor [--model path.glb] [--game module.so] [--physics tynima|jolt] [--rhi sdl|metal]
//                 [--headless] [--frames N]
//
// Four docked panels: the hierarchy (every entity, under its parent), the
// inspector (the selected entity's components, the built-in ones editable),
// the viewport (the scene, drawn by the engine into a texture the panel
// shows, with a fly camera: hold the right mouse button to look, WASD/QE to
// move, Shift to hurry), and the console (the engine's log, filtered). A
// stats panel beside the console. The layout is remembered in
// tynima-editor.ini; View > Reset layout puts it back.
#include <tynima.h>

#include <imgui.h>
#include <imgui_internal.h> // the dock builder: the default layout

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;

struct Options {
    std::string model;
    std::string game;
    const char* physics = nullptr;
    const char* rhi = nullptr;
    bool headless = false;
    long max_frames = -1;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            options.model = argv[++i];
        } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            options.game = argv[++i];
        } else if (std::strcmp(argv[i], "--physics") == 0 && i + 1 < argc) {
            options.physics = argv[++i];
        } else if (std::strcmp(argv[i], "--rhi") == 0 && i + 1 < argc) {
            options.rhi = argv[++i];
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            options.headless = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            options.max_frames = std::strtol(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: tynima-editor [--model path.glb] [--game module.so] "
                                 "[--physics tynima|jolt] [--rhi sdl|metal] [--headless] [--frames N]\n");
            std::exit(2);
        }
    }
    return options;
}

// ---------------------------------------------------------------- the world

// The component ids the editor knows the shape of, looked up by name once.
struct Components {
    tynima_component_id name = TYNIMA_NO_COMPONENT;
    tynima_component_id transform = TYNIMA_NO_COMPONENT;
    tynima_component_id local_to_world = TYNIMA_NO_COMPONENT;
    tynima_component_id parent = TYNIMA_NO_COMPONENT;
    tynima_component_id mesh_renderer = TYNIMA_NO_COMPONENT;
    tynima_component_id rigid_body = TYNIMA_NO_COMPONENT;

    void resolve(const tynima_api& api, tynima_engine* engine) {
        name = api.find_component(engine, TYNIMA_COMPONENT_NAME);
        transform = api.find_component(engine, TYNIMA_COMPONENT_TRANSFORM);
        local_to_world = api.find_component(engine, TYNIMA_COMPONENT_LOCAL_TO_WORLD);
        parent = api.find_component(engine, TYNIMA_COMPONENT_PARENT);
        mesh_renderer = api.find_component(engine, TYNIMA_COMPONENT_MESH_RENDERER);
        rigid_body = api.find_component(engine, TYNIMA_COMPONENT_RIGID_BODY);
    }
};

bool same(tynima_entity a, tynima_entity b) {
    return a.index == b.index && a.generation == b.generation;
}

// Every entity, with its parent, gathered once a frame so the hierarchy can
// walk parents to children without asking the world n times each.
struct Hierarchy {
    struct Node {
        tynima_entity entity;
        std::int32_t first_child = -1; // into nodes
        std::int32_t next_sibling = -1;
        bool has_parent = false;
    };
    std::vector<Node> nodes;
    std::vector<std::int32_t> node_of_index; // entity index -> node, or -1

    void rebuild(const tynima_api& api, tynima_engine* engine, const Components& components) {
        nodes.clear();
        struct Gather {
            std::vector<Node>* nodes;
        } gather{&nodes};
        api.each_chunk(
            engine, nullptr, 0,
            [](void* user, const tynima_entity* entities, uint32_t count, void* const*) {
                auto* g = static_cast<Gather*>(user);
                for (uint32_t i = 0; i < count; ++i) {
                    g->nodes->push_back(Node{.entity = entities[i]});
                }
            },
            &gather);
        std::sort(nodes.begin(), nodes.end(),
                  [](const Node& a, const Node& b) { return a.entity.index < b.entity.index; });
        std::uint32_t max_index = 0;
        for (const Node& node : nodes) {
            max_index = std::max(max_index, node.entity.index);
        }
        node_of_index.assign(max_index + 1, -1);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            node_of_index[nodes[i].entity.index] = static_cast<std::int32_t>(i);
        }
        if (components.parent == TYNIMA_NO_COMPONENT) {
            return;
        }
        // Children hang off their parents in entity order: the last one
        // linked goes to the front, so link them back to front.
        for (std::size_t i = nodes.size(); i-- > 0;) {
            Node& node = nodes[i];
            const auto* parent =
                static_cast<const tynima_parent*>(api.get_component(engine, node.entity, components.parent));
            if (parent == nullptr || parent->entity.generation == 0 ||
                parent->entity.index >= node_of_index.size()) {
                continue;
            }
            const std::int32_t p = node_of_index[parent->entity.index];
            if (p < 0 || !same(nodes[static_cast<std::size_t>(p)].entity, parent->entity) ||
                p == static_cast<std::int32_t>(i)) {
                continue; // a dead or stale parent: shown at the root
            }
            node.has_parent = true;
            node.next_sibling = nodes[static_cast<std::size_t>(p)].first_child;
            nodes[static_cast<std::size_t>(p)].first_child = static_cast<std::int32_t>(i);
        }
    }
};

// "name" from its Name component, or "entity N".
void entity_label(const tynima_api& api, tynima_engine* engine, const Components& components,
                  tynima_entity entity, char* out, std::size_t capacity) {
    if (components.name != TYNIMA_NO_COMPONENT) {
        const auto* name =
            static_cast<const tynima_name*>(api.get_component(engine, entity, components.name));
        if (name != nullptr && name->text[0] != '\0') {
            std::snprintf(out, capacity, "%s", name->text);
            return;
        }
    }
    std::snprintf(out, capacity, "entity %u", entity.index);
}

// ---------------------------------------------------------------- the panels

struct Editor {
    tynima_engine* engine = nullptr;
    const tynima_api* api = nullptr;
    Components components;
    Hierarchy hierarchy;
    tynima_entity selected{};
    bool show_hierarchy = true, show_inspector = true, show_viewport = true, show_console = true,
         show_stats = true;
    bool show_demo = false;
    bool reset_layout = false;
    bool quit = false;

    // The fly camera in the viewport.
    float yaw = 0.0f, pitch = 0.0f;
    float speed = 3.0f; // metres per second
    bool looking = false;

    // The console's view of the log.
    int console_min_level = TYNIMA_LOG_DEBUG;
    char console_filter[64] = "";
    uint64_t console_first = 0; // "clear" hides everything before this
    bool console_autoscroll = true;

    void draw_menu_bar();
    void draw_hierarchy();
    void draw_hierarchy_node(const Hierarchy::Node& node, int depth);
    void draw_inspector();
    void draw_viewport();
    void draw_console();
    void draw_stats();
    void layout(ImGuiID dockspace);
};

void Editor::layout(ImGuiID dockspace) {
    // Hierarchy left, inspector right, console and stats below, the
    // viewport in the middle.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Stats", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
}

void Editor::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Quit", "Cmd+Q")) {
            quit = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &show_hierarchy);
        ImGui::MenuItem("Inspector", nullptr, &show_inspector);
        ImGui::MenuItem("Viewport", nullptr, &show_viewport);
        ImGui::MenuItem("Console", nullptr, &show_console);
        ImGui::MenuItem("Stats", nullptr, &show_stats);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            reset_layout = true;
            show_hierarchy = show_inspector = show_viewport = show_console = show_stats = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Render")) {
        tynima_render_settings settings{};
        tynima_get_render_settings(engine, &settings);
        const tynima_render_settings before = settings;
        static constexpr const char* kPaths[] = {"Forward", "Fused deferred (the tile)", "Split deferred"};
        static constexpr const char* kModels[] = {"Unlit", "Blinn-Phong", "Cook-Torrance"};
        static constexpr const char* kTonemaps[] = {"None", "ACES", "AgX"};
        static constexpr const char* kAntiAliasing[] = {"None", "FXAA", "TAA"};
        if (ImGui::BeginMenu("Shading path")) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::MenuItem(kPaths[i], nullptr, static_cast<int>(settings.path) == i)) {
                    settings.path = static_cast<tynima_shading_path>(i);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Shading model")) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::MenuItem(kModels[i], nullptr, static_cast<int>(settings.model) == i)) {
                    settings.model = static_cast<tynima_shading_model>(i);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug view")) {
            for (uint32_t i = 0; i < TYNIMA_DEBUG_VIEW_COUNT; ++i) {
                if (ImGui::MenuItem(tynima_debug_view_name(i), nullptr, settings.debug_view == i)) {
                    settings.debug_view = i;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Shadows", nullptr, &settings.shadows);
        ImGui::MenuItem("Point lights", nullptr, &settings.point_lights);
        ImGui::MenuItem("Bloom", nullptr, &settings.bloom);
        if (ImGui::BeginMenu("Tonemap")) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::MenuItem(kTonemaps[i], nullptr, static_cast<int>(settings.tonemap) == i)) {
                    settings.tonemap = static_cast<tynima_tonemap>(i);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Anti-aliasing")) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::MenuItem(kAntiAliasing[i], nullptr,
                                    static_cast<int>(settings.anti_aliasing) == i)) {
                    settings.anti_aliasing = static_cast<tynima_anti_aliasing>(i);
                }
            }
            ImGui::EndMenu();
        }
        if (std::memcmp(&settings, &before, sizeof settings) != 0) {
            tynima_set_render_settings(engine, &settings);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Dear ImGui demo", nullptr, &show_demo);
        ImGui::Separator();
        ImGui::TextDisabled("Tynima %s, API %u", tynima_get_version_string(), TYNIMA_API_VERSION);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::draw_hierarchy_node(const Hierarchy::Node& node, int depth) {
    char label[TYNIMA_NAME_CAPACITY + 32];
    entity_label(*api, engine, components, node.entity, label, sizeof label);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.first_child < 0) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (same(node.entity, selected)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (depth < 2) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const bool open = ImGui::TreeNodeEx(
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(node.entity.index)), flags, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected = node.entity;
    }
    if (open && node.first_child >= 0) {
        for (std::int32_t child = node.first_child; child >= 0;
             child = hierarchy.nodes[static_cast<std::size_t>(child)].next_sibling) {
            draw_hierarchy_node(hierarchy.nodes[static_cast<std::size_t>(child)], depth + 1);
        }
        ImGui::TreePop();
    }
}

void Editor::draw_hierarchy() {
    if (!ImGui::Begin("Hierarchy", &show_hierarchy)) {
        ImGui::End();
        return;
    }
    hierarchy.rebuild(*api, engine, components);
    ImGui::TextDisabled("%zu entities", hierarchy.nodes.size());
    ImGui::Separator();
    if (ImGui::BeginChild("tree")) {
        for (const Hierarchy::Node& node : hierarchy.nodes) {
            if (!node.has_parent) {
                draw_hierarchy_node(node, 0);
            }
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::draw_inspector() {
    if (!ImGui::Begin("Inspector", &show_inspector)) {
        ImGui::End();
        return;
    }
    if (selected.generation == 0 || !api->entity_alive(engine, selected)) {
        ImGui::TextDisabled("Select an entity in the hierarchy.");
        ImGui::End();
        return;
    }
    char label[TYNIMA_NAME_CAPACITY + 32];
    entity_label(*api, engine, components, selected, label, sizeof label);
    ImGui::Text("%s", label);
    ImGui::SameLine();
    ImGui::TextDisabled("#%u.%u", selected.index, selected.generation);
    ImGui::Separator();

    tynima_component_id ids[TYNIMA_MAX_COMPONENT_TYPES];
    const uint32_t count = std::min(api->entity_components(engine, selected, ids, TYNIMA_MAX_COMPONENT_TYPES),
                                    TYNIMA_MAX_COMPONENT_TYPES);
    for (uint32_t i = 0; i < count; ++i) {
        tynima_component_info info{};
        if (!api->component_info(engine, ids[i], &info)) {
            continue;
        }
        void* data = api->get_component(engine, selected, ids[i]);
        if (data == nullptr) {
            continue;
        }
        ImGui::PushID(static_cast<int>(ids[i]));
        if (ImGui::CollapsingHeader(info.name, ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ids[i] == components.name) {
                auto* name = static_cast<tynima_name*>(data);
                ImGui::InputText("text", name->text, sizeof name->text);
            } else if (ids[i] == components.transform) {
                auto* transform = static_cast<tynima_transform*>(data);
                bool changed =
                    ImGui::DragFloat3("position", &transform->position.x, 0.01f, 0.0f, 0.0f, "%.3f");
                float yaw_deg, pitch_deg, roll_deg;
                tynima_quat_to_euler(transform->rotation, &yaw_deg, &pitch_deg, &roll_deg);
                float euler[3] = {yaw_deg * 180.0f / kPi, pitch_deg * 180.0f / kPi, roll_deg * 180.0f / kPi};
                if (ImGui::DragFloat3("rotation", euler, 0.5f, 0.0f, 0.0f, "%.1f°")) {
                    transform->rotation = tynima_quat_from_euler(
                        euler[0] * kPi / 180.0f, euler[1] * kPi / 180.0f, euler[2] * kPi / 180.0f);
                    changed = true;
                }
                changed |= ImGui::DragFloat3("scale", &transform->scale.x, 0.01f, 0.0f, 0.0f, "%.3f");
                ImGui::TextDisabled("yaw, pitch, roll");
                // A body drives its entity's transform every frame: an edit
                // sticks only if the body moves too.
                if (changed && components.rigid_body != TYNIMA_NO_COMPONENT) {
                    if (const auto* body = static_cast<const tynima_rigid_body*>(
                            api->get_component(engine, selected, components.rigid_body))) {
                        api->body_set_transform(engine, body->body, transform->position, transform->rotation);
                        api->body_set_velocity(engine, body->body, tynima_vec3_make(0.0f, 0.0f, 0.0f),
                                               tynima_vec3_make(0.0f, 0.0f, 0.0f));
                    }
                }
            } else if (ids[i] == components.local_to_world) {
                const auto* l = static_cast<const tynima_local_to_world*>(data);
                for (int row = 0; row < 4; ++row) {
                    ImGui::Text("%8.3f %8.3f %8.3f %8.3f", static_cast<double>(l->matrix.m[row]),
                                static_cast<double>(l->matrix.m[4 + row]),
                                static_cast<double>(l->matrix.m[8 + row]),
                                static_cast<double>(l->matrix.m[12 + row]));
                }
                ImGui::TextDisabled("written by the engine each frame");
            } else if (ids[i] == components.parent) {
                auto* parent = static_cast<tynima_parent*>(data);
                char parent_label[TYNIMA_NAME_CAPACITY + 32];
                entity_label(*api, engine, components, parent->entity, parent_label, sizeof parent_label);
                ImGui::Text("%s", parent_label);
                ImGui::SameLine();
                if (ImGui::SmallButton("select")) {
                    selected = parent->entity;
                }
            } else if (ids[i] == components.mesh_renderer) {
                auto* renderer = static_cast<tynima_mesh_renderer*>(data);
                int model = static_cast<int>(renderer->model);
                const int model_count = static_cast<int>(tynima_model_count(engine));
                if (ImGui::InputInt("model", &model)) {
                    renderer->model = static_cast<uint32_t>(std::max(model, 0));
                }
                if (model >= model_count) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(none loaded)");
                }
                ImGui::Checkbox("visible", &renderer->visible);
            } else if (ids[i] == components.rigid_body) {
                const auto* body = static_cast<const tynima_rigid_body*>(data);
                ImGui::Text("body #%u.%u", body->body.index, body->body.generation);
                ImGui::TextDisabled("drives the transform; edit the transform to move it");
            } else {
                ImGui::TextDisabled("%u bytes, aligned to %u — no editor for this component yet", info.size,
                                    info.alignment);
            }
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void Editor::draw_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin("Viewport", &show_viewport);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const float density = tynima_ui_pixel_density(engine);
    const auto width = static_cast<uint32_t>(std::max(size.x * density, 1.0f));
    const auto height = static_cast<uint32_t>(std::max(size.y * density, 1.0f));
    const uint64_t texture =
        size.x >= 1.0f && size.y >= 1.0f ? tynima_ui_scene_texture(engine, width, height) : 0;
    if (texture != 0) {
        ImGui::Image(static_cast<ImTextureID>(texture), size);
    } else {
        ImGui::TextDisabled("  no GPU: nothing to show");
    }
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsWindowHovered();

    // The fly camera: a right-drag over the picture looks around; while it
    // lasts, the keys move the camera whatever panel the mouse is over.
    if (hovered && api->mouse_pressed(engine, TYNIMA_MOUSE_RIGHT)) {
        looking = true;
        tynima_set_relative_mouse(engine, true);
    }
    if (looking && api->mouse_released(engine, TYNIMA_MOUSE_RIGHT)) {
        looking = false;
        tynima_set_relative_mouse(engine, false);
    }
    tynima_camera camera{};
    tynima_get_camera(engine, &camera);
    if (looking) {
        constexpr float kSensitivity = 0.0025f; // radians per pixel
        float dx = 0.0f, dy = 0.0f;
        api->mouse_delta(engine, &dx, &dy);
        yaw -= dx * kSensitivity;
        pitch = std::clamp(pitch - dy * kSensitivity, -0.49f * kPi, 0.49f * kPi);
        camera.rotation = tynima_quat_from_euler(yaw, pitch, 0.0f);
        const tynima_vec3 forward = tynima_quat_rotate(camera.rotation, tynima_vec3_make(0.0f, 0.0f, -1.0f));
        const tynima_vec3 right = tynima_quat_rotate(camera.rotation, tynima_vec3_make(1.0f, 0.0f, 0.0f));
        tynima_vec3 move = tynima_vec3_make(0.0f, 0.0f, 0.0f);
        if (api->key_down(engine, TYNIMA_KEY_W))
            move = tynima_vec3_add(move, forward);
        if (api->key_down(engine, TYNIMA_KEY_S))
            move = tynima_vec3_sub(move, forward);
        if (api->key_down(engine, TYNIMA_KEY_D))
            move = tynima_vec3_add(move, right);
        if (api->key_down(engine, TYNIMA_KEY_A))
            move = tynima_vec3_sub(move, right);
        if (api->key_down(engine, TYNIMA_KEY_E))
            move = tynima_vec3_add(move, tynima_vec3_make(0.0f, 1.0f, 0.0f));
        if (api->key_down(engine, TYNIMA_KEY_Q))
            move = tynima_vec3_sub(move, tynima_vec3_make(0.0f, 1.0f, 0.0f));
        const bool hurry =
            api->key_down(engine, TYNIMA_KEY_LeftShift) || api->key_down(engine, TYNIMA_KEY_RightShift);
        float wx = 0.0f, wy = 0.0f;
        api->mouse_wheel(engine, &wx, &wy);
        speed = std::clamp(speed * std::pow(1.15f, wy), 0.05f, 200.0f); // the wheel sets the pace
        const float step = speed * (hurry ? 4.0f : 1.0f) * tynima_engine_dt(engine);
        camera.position =
            tynima_vec3_add(camera.position, tynima_vec3_scale(tynima_vec3_normalize(move), step));
        tynima_set_camera(engine, &camera);
    }
    // A little overlay in the corner: where the camera is.
    ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
    ImGui::TextDisabled("%.1f, %.1f, %.1f  %.1f m/s%s", static_cast<double>(camera.position.x),
                        static_cast<double>(camera.position.y), static_cast<double>(camera.position.z),
                        static_cast<double>(speed), looking ? "  (looking)" : "  right-drag to look");
    ImGui::End();
}

void Editor::draw_console() {
    if (!ImGui::Begin("Console", &show_console)) {
        ImGui::End();
        return;
    }
    static constexpr const char* kLevels[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    ImGui::SetNextItemWidth(90.0f);
    ImGui::Combo("##level", &console_min_level, kLevels, 6);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##filter", "filter", console_filter, sizeof console_filter);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        console_first = tynima_log_count(engine);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &console_autoscroll);
    ImGui::SameLine();
    int engine_level = static_cast<int>(tynima_log_level_get());
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("logged", &engine_level, kLevels, 6)) {
        tynima_log_level_set(static_cast<tynima_log_level>(engine_level));
    }
    ImGui::Separator();

    if (ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const uint64_t count = tynima_log_count(engine);
        const uint64_t first = std::max(console_first, count > TYNIMA_LOG_RING ? count - TYNIMA_LOG_RING : 0);
        // The ring is walked whole and filtered; a few thousand lines is nothing.
        tynima_log_entry entry{};
        for (uint64_t i = first; i < count; ++i) {
            if (!tynima_log_get(engine, i, &entry) || static_cast<int>(entry.level) < console_min_level) {
                continue;
            }
            if (console_filter[0] != '\0' && std::strstr(entry.message, console_filter) == nullptr &&
                std::strstr(entry.category, console_filter) == nullptr) {
                continue;
            }
            ImVec4 color(0.85f, 0.85f, 0.85f, 1.0f);
            switch (entry.level) {
            case TYNIMA_LOG_TRACE:
            case TYNIMA_LOG_DEBUG:
                color = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
                break;
            case TYNIMA_LOG_WARN:
                color = ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
                break;
            case TYNIMA_LOG_ERROR:
            case TYNIMA_LOG_FATAL:
                color = ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
                break;
            default:
                break;
            }
            ImGui::TextColored(color, "%8.3f %-5s %-9s %s", static_cast<double>(entry.time_ns) / 1e9,
                               kLevels[static_cast<int>(entry.level)], entry.category, entry.message);
        }
        if (console_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::draw_stats() {
    if (!ImGui::Begin("Stats", &show_stats)) {
        ImGui::End();
        return;
    }
    tynima_stats stats{};
    tynima_get_stats(engine, &stats);
    const float dt = tynima_engine_dt(engine);
    ImGui::Text("%.0f frames/s, %.2f ms a frame on the CPU",
                static_cast<double>(dt > 0.0f ? 1.0f / dt : 0.0f), static_cast<double>(stats.frame_ms));
    if (stats.gpu_ms > 0.0f) {
        ImGui::Text("%.2f ms a frame on the GPU, %.1f MB allocated by the device",
                    static_cast<double>(stats.gpu_ms), static_cast<double>(stats.gpu_bytes) / 1048576.0);
    } else {
        ImGui::TextDisabled("GPU time unknown on this backend");
    }
    ImGui::Separator();
    ImGui::Text("%u entities in %u archetypes", stats.entities, stats.archetypes);
    ImGui::Text("%u bodies, %u awake", stats.bodies, stats.bodies_awake);
    ImGui::Text("%u models, %u game module reloads", tynima_model_count(engine), stats.game_reloads);
    ImGui::Separator();
    ImGui::Text("frame graph: %u passes (%u culled), %u transients never leave the tile", stats.graph_passes,
                stats.graph_culled, stats.graph_memoryless);
    ImGui::Text("%.1f MB of textures in memory, %.1f MB memoryless",
                static_cast<double>(stats.graph_bytes) / 1048576.0,
                static_cast<double>(stats.graph_bytes_memoryless) / 1048576.0);
    ImGui::Text("engine heap allocations this frame: %llu",
                static_cast<unsigned long long>(stats.heap_allocations));
    ImGui::End();
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);
    tynima_engine_desc desc{};
    desc.title = "Tynima editor";
    desc.width = 1600;
    desc.height = 960;
    desc.headless = options.headless;
    desc.game_module = options.game.empty() ? nullptr : options.game.c_str();
    desc.physics = options.physics;
    desc.rhi = options.rhi;
    tynima_engine* engine = tynima_engine_create(&desc);
    if (engine == nullptr) {
        std::fprintf(stderr, "tynima-editor: %s\n", tynima_last_error());
        return 1;
    }
    Editor editor;
    editor.engine = engine;
    editor.api = tynima_engine_api(engine);
    editor.components.resolve(*editor.api, engine);

    // Something to look at: the model as one entity, with a floor of its own name.
    if (!options.model.empty()) {
        const uint32_t model = tynima_load_model(engine, options.model.c_str());
        if (model != TYNIMA_NO_MODEL) {
            tynima_name name{};
            std::snprintf(name.text, sizeof name.text, "%s", options.model.c_str());
            // The file's base name, if it fits.
            if (const char* slash = std::strrchr(options.model.c_str(), '/')) {
                std::snprintf(name.text, sizeof name.text, "%s", slash + 1);
            }
            tynima_transform transform{};
            transform.rotation = tynima_quat_identity();
            transform.scale = tynima_vec3_make(1.0f, 1.0f, 1.0f);
            tynima_local_to_world local_to_world{};
            tynima_mesh_renderer renderer{.model = model, .visible = true};
            const tynima_component_id ids[4] = {editor.components.name, editor.components.transform,
                                                editor.components.local_to_world,
                                                editor.components.mesh_renderer};
            const void* values[4] = {&name, &transform, &local_to_world, &renderer};
            editor.selected = editor.api->create_entity(engine, ids, values, 4);
        }
    }
    tynima_camera camera{};
    tynima_get_camera(engine, &camera);
    camera.position = tynima_vec3_make(0.0f, 0.4f, 1.2f);
    editor.pitch = -0.25f;
    camera.rotation = tynima_quat_from_euler(editor.yaw, editor.pitch, 0.0f);
    tynima_set_camera(engine, &camera);

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(tynima_ui_context(engine)));
    ImGui::GetIO().IniFilename = "tynima-editor.ini";

    long frames = 0;
    while (tynima_engine_begin_frame(engine)) {
        editor.draw_menu_bar();
        const ImGuiID dockspace = ImGui::GetID("editor dockspace");
        if (editor.reset_layout || ImGui::DockBuilderGetNode(dockspace) == nullptr) {
            editor.layout(dockspace);
            editor.reset_layout = false;
        }
        ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
        if (editor.show_hierarchy)
            editor.draw_hierarchy();
        if (editor.show_inspector)
            editor.draw_inspector();
        if (editor.show_viewport)
            editor.draw_viewport();
        if (editor.show_console)
            editor.draw_console();
        if (editor.show_stats)
            editor.draw_stats();
        if (editor.show_demo)
            ImGui::ShowDemoWindow(&editor.show_demo);
        tynima_engine_end_frame(engine);
        if (editor.quit || (options.max_frames >= 0 && ++frames >= options.max_frames)) {
            break;
        }
    }
    tynima_engine_destroy(engine);
    return 0;
}
