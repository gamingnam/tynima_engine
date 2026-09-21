// The editor. A client of the SDK with no special privileges: it sees the
// engine through tynima.h — the same surface a game's runtime gets — and
// draws itself with Dear ImGui, which the engine hosts. If something the
// editor needs is not in tynima.h, tynima.h is incomplete; that rule is
// what keeps the SDK honest, and tools/check_layering.py enforces it.
//
//   tynima-editor [--scene file.toml] [--model path.glb] [--game module.so] [--physics tynima|jolt]
//                 [--rhi sdl|metal] [--headless] [--frames N]
//
// Four docked panels: the hierarchy (every entity, under its parent), the
// inspector (the selected entity's components, every field of every one,
// drawn from the fields the engine describes — the editor knows no
// component's layout), the viewport (the scene, drawn by the engine into a
// texture the panel shows, with a fly camera: hold the right mouse button to
// look, WASD/QE to move, Shift to hurry; click to select what is under the
// mouse, and move, rotate or scale it with the gizmo — W, E, R switch, Shift
// snaps), and the console (the engine's log, filtered). A stats panel beside
// the console. Every edit in the inspector or the viewport goes on an undo
// stack (Cmd+Z, Cmd+Shift+Z). The layout is remembered in tynima-editor.ini;
// View > Reset layout puts it back.
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
    std::string scene;
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
        if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            options.scene = argv[++i];
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
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
            std::fprintf(stderr,
                         "usage: tynima-editor [--scene file.toml] [--model path.glb] [--game module.so] "
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

// -------------------------------------------------------------------- undo

// An edit is a component's bytes before and after. The inspector snapshots
// a component's bytes before it draws its widgets, and when a widget is
// released after a change, the snapshot and the bytes now become one edit.
// Undo copies the old bytes back; redo the new. What a change means to the
// rest of the engine (a body that must follow its transform) happens after
// either, the same as after the edit.
struct Edit {
    tynima_entity entity;
    tynima_component_id component;
    std::vector<std::uint8_t> before, after;
};

struct UndoStack {
    std::vector<Edit> done, undone;
    static constexpr std::size_t kLimit = 256;

    void push(Edit edit) {
        if (edit.before == edit.after) {
            return;
        }
        done.push_back(std::move(edit));
        undone.clear();
        if (done.size() > kLimit) {
            done.erase(done.begin());
        }
    }
};

// ------------------------------------------------------------------ gizmos

// A rotation from the three columns of a world matrix, scale and all: the
// columns are made unit first (Shepperd's method on the rest).
tynima_quat quat_from_columns(tynima_vec3 c0, tynima_vec3 c1, tynima_vec3 c2) {
    c0 = tynima_vec3_normalize(c0);
    c1 = tynima_vec3_normalize(c1);
    c2 = tynima_vec3_normalize(c2);
    // m[row][col]: the rotation matrix whose columns are the axes.
    const float m00 = c0.x, m01 = c1.x, m02 = c2.x;
    const float m10 = c0.y, m11 = c1.y, m12 = c2.y;
    const float m20 = c0.z, m21 = c1.z, m22 = c2.z;
    const float trace = m00 + m11 + m22;
    tynima_quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return tynima_quat_normalize(q);
}

tynima_quat quat_conjugate(tynima_quat q) {
    return {-q.x, -q.y, -q.z, q.w};
}

tynima_vec3 column(const tynima_mat4& m, int c) {
    return tynima_vec3_make(m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]);
}

// Distance from a point to a segment, on screen.
float segment_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float length_sq = abx * abx + aby * aby;
    float t = length_sq > 0.0f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / length_sq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float dx = p.x - (a.x + abx * t), dy = p.y - (a.y + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

// Two perpendicular unit vectors around an axis: a basis for its plane.
void plane_basis(tynima_vec3 axis, tynima_vec3* u, tynima_vec3* v) {
    const tynima_vec3 helper =
        std::fabs(axis.y) < 0.9f ? tynima_vec3_make(0.0f, 1.0f, 0.0f) : tynima_vec3_make(1.0f, 0.0f, 0.0f);
    *u = tynima_vec3_normalize(tynima_vec3_cross(helper, axis));
    *v = tynima_vec3_cross(axis, *u);
}

enum class GizmoMode { Translate, Rotate, Scale };

// The handles drawn over the selected entity and dragged with the mouse.
// Translation and rotation work along the world's axes, scale along the
// entity's own. A drag is one undoable edit of the Transform, and the
// change is applied in the entity's parent's space, so a child of a turned
// parent still moves where the mouse points.
struct Gizmo {
    GizmoMode mode = GizmoMode::Translate;
    int hover = -1;  // 0..2: an axis; 3: the centre (uniform scale)
    int active = -1; // the axis being dragged
    // The drag, from where it began.
    tynima_transform start_transform{};
    std::vector<std::uint8_t> start_bytes;
    tynima_vec3 center{}; // world
    tynima_vec3 axis{};   // world, unit
    tynima_vec3 plane_u{}, plane_v{};
    float start_value = 0.0f; // the axis parameter, or the angle, at the first frame
    tynima_quat parent_rotation = tynima_quat_identity();
    tynima_mat4 parent_matrix{};
    bool has_parent = false;
};

// ---------------------------------------------------------------- the panels

struct Editor {
    tynima_engine* engine = nullptr;
    const tynima_api* api = nullptr;
    Components components;
    Hierarchy hierarchy;
    tynima_entity selected{};
    UndoStack undo;
    std::vector<std::uint8_t> frame_bytes; // the component being drawn, before its widgets ran this frame
    std::vector<std::uint8_t> edit_bytes;  // the component as it was when the widget being edited took hold

    // The scene on disk: where Save goes, and the path dialog's text.
    std::string scene_path;
    char path_buffer[512] = "";
    enum class PathDialog { None, Open, SaveAs } path_dialog = PathDialog::None;
    std::string scene_status; // the last save or load, for the status line
    bool show_hierarchy = true, show_inspector = true, show_viewport = true, show_console = true,
         show_stats = true;
    bool show_demo = false;
    bool reset_layout = false;
    bool quit = false;

    Gizmo gizmo;

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
    void draw_path_dialog();
    void save_scene(const std::string& path);
    void load_scene(const std::string& path);
    void after_edit(tynima_entity entity, tynima_component_id component);
    bool apply(const Edit& edit, bool forward);
    void undo_last();
    void redo_last();
    void draw_field(const tynima_field& field, void* data);
    void draw_component(tynima_component_id id, const tynima_component_info& info, void* data);
    void draw_hierarchy();
    void draw_hierarchy_node(const Hierarchy::Node& node, int depth);
    void draw_inspector();
    void draw_viewport();
    void draw_viewport_overlay(const ImVec2& image_min, const ImVec2& image_size, const tynima_camera& camera,
                               bool hovered);
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
        if (ImGui::MenuItem("New scene")) {
            tynima_clear_scene(engine);
            selected = tynima_entity{};
            undo.done.clear();
            undo.undone.clear();
            scene_path.clear();
            scene_status = "new scene";
        }
        if (ImGui::MenuItem("Open scene...", "Cmd+O")) {
            path_dialog = PathDialog::Open;
            std::snprintf(path_buffer, sizeof path_buffer, "%s", scene_path.c_str());
        }
        if (ImGui::MenuItem("Save scene", "Cmd+S")) {
            if (scene_path.empty()) {
                path_dialog = PathDialog::SaveAs;
            } else {
                save_scene(scene_path);
            }
        }
        if (ImGui::MenuItem("Save scene as...", "Cmd+Shift+S")) {
            path_dialog = PathDialog::SaveAs;
            std::snprintf(path_buffer, sizeof path_buffer, "%s", scene_path.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", scene_path.empty() ? "(unsaved scene)" : scene_path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Cmd+Q")) {
            quit = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        char label[64];
        std::snprintf(label, sizeof label, "Undo (%zu)", undo.done.size());
        if (ImGui::MenuItem(label, "Cmd+Z", false, !undo.done.empty())) {
            undo_last();
        }
        std::snprintf(label, sizeof label, "Redo (%zu)", undo.undone.size());
        if (ImGui::MenuItem(label, "Cmd+Shift+Z", false, !undo.undone.empty())) {
            redo_last();
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
    if (!scene_status.empty()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(scene_status.c_str()).x - 16.0f);
        ImGui::TextDisabled("%s", scene_status.c_str());
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
    ImGui::SameLine();
    if (ImGui::SmallButton("New entity")) {
        // A name, a transform and a world matrix: enough to draw and to place.
        tynima_name name{};
        std::snprintf(name.text, sizeof name.text, "entity");
        tynima_transform transform{};
        transform.rotation = tynima_quat_identity();
        transform.scale = tynima_vec3_make(1.0f, 1.0f, 1.0f);
        tynima_local_to_world local_to_world{};
        const tynima_component_id ids[3] = {components.name, components.transform, components.local_to_world};
        const void* values[3] = {&name, &transform, &local_to_world};
        selected = api->create_entity(engine, ids, values, 3);
    }
    if (selected.generation != 0 && api->entity_alive(engine, selected)) {
        ImGui::SameLine();
        // Delete or Backspace too, with the hierarchy focused and no text field taking keys.
        const bool key =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
        if (ImGui::SmallButton("Delete") || key) {
            (void)api->destroy_entity(engine, selected);
            selected = tynima_entity{};
        }
    }
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

// A path typed into a small modal: the editor has no native file dialog yet.
void Editor::draw_path_dialog() {
    if (path_dialog == PathDialog::None) {
        return;
    }
    const char* title = path_dialog == PathDialog::Open ? "Open scene" : "Save scene as";
    ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("a .toml scene file, relative to the working directory or absolute");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool entered =
            ImGui::InputText("path", path_buffer, sizeof path_buffer, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool confirmed = ImGui::Button(path_dialog == PathDialog::Open ? "Open" : "Save") || entered;
        ImGui::SameLine();
        const bool cancelled = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (confirmed && path_buffer[0] != '\0') {
            if (path_dialog == PathDialog::Open) {
                load_scene(path_buffer);
            } else {
                save_scene(path_buffer);
            }
            path_dialog = PathDialog::None;
            ImGui::CloseCurrentPopup();
        } else if (cancelled) {
            path_dialog = PathDialog::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Editor::save_scene(const std::string& path) {
    if (tynima_save_scene(engine, path.c_str())) {
        scene_path = path;
        scene_status = "saved " + path;
    } else {
        scene_status = std::string("save failed: ") + tynima_last_error();
    }
}

void Editor::load_scene(const std::string& path) {
    if (tynima_load_scene(engine, path.c_str(), true)) {
        scene_path = path;
        scene_status = "opened " + path;
        selected = tynima_entity{};
        undo.done.clear(); // every handle in it is gone
        undo.undone.clear();
    } else {
        scene_status = std::string("open failed: ") + tynima_last_error();
    }
}

// A body drives its entity's transform every frame: an edit to the
// transform sticks only if the body moves too.
void Editor::after_edit(tynima_entity entity, tynima_component_id component) {
    if (component != components.transform || components.rigid_body == TYNIMA_NO_COMPONENT) {
        return;
    }
    const auto* transform =
        static_cast<const tynima_transform*>(api->get_component(engine, entity, component));
    const auto* body =
        static_cast<const tynima_rigid_body*>(api->get_component(engine, entity, components.rigid_body));
    if (transform != nullptr && body != nullptr) {
        api->body_set_transform(engine, body->body, transform->position, transform->rotation);
        api->body_set_velocity(engine, body->body, tynima_vec3_make(0.0f, 0.0f, 0.0f),
                               tynima_vec3_make(0.0f, 0.0f, 0.0f));
    }
}

bool Editor::apply(const Edit& edit, bool forward) {
    const std::vector<std::uint8_t>& bytes = forward ? edit.after : edit.before;
    void* data = api->get_component(engine, edit.entity, edit.component);
    tynima_component_info info{};
    if (data == nullptr || !api->component_info(engine, edit.component, &info) || info.size != bytes.size()) {
        return false; // the entity or the component is gone: the edit is stale, and stays skipped
    }
    std::memcpy(data, bytes.data(), bytes.size());
    after_edit(edit.entity, edit.component);
    return true;
}

void Editor::undo_last() {
    while (!undo.done.empty()) {
        Edit edit = std::move(undo.done.back());
        undo.done.pop_back();
        const bool applied = apply(edit, false);
        undo.undone.push_back(std::move(edit));
        if (applied) {
            return;
        }
    }
}

void Editor::redo_last() {
    while (!undo.undone.empty()) {
        Edit edit = std::move(undo.undone.back());
        undo.undone.pop_back();
        const bool applied = apply(edit, true);
        undo.done.push_back(std::move(edit));
        if (applied) {
            return;
        }
    }
}

// One field, as a widget for its kind. `data` is the field's own bytes.
void Editor::draw_field(const tynima_field& field, void* data) {
    const bool read_only = (field.flags & TYNIMA_FIELD_READ_ONLY) != 0;
    ImGui::BeginDisabled(read_only);
    switch (field.kind) {
    case TYNIMA_FIELD_BOOL:
        ImGui::Checkbox(field.name, static_cast<bool*>(data));
        break;
    case TYNIMA_FIELD_INT8:
        ImGui::DragScalar(field.name, ImGuiDataType_S8, data, 0.2f);
        break;
    case TYNIMA_FIELD_UINT8:
        ImGui::DragScalar(field.name, ImGuiDataType_U8, data, 0.2f);
        break;
    case TYNIMA_FIELD_INT16:
        ImGui::DragScalar(field.name, ImGuiDataType_S16, data, 0.2f);
        break;
    case TYNIMA_FIELD_UINT16:
        ImGui::DragScalar(field.name, ImGuiDataType_U16, data, 0.2f);
        break;
    case TYNIMA_FIELD_INT32:
        ImGui::DragScalar(field.name, ImGuiDataType_S32, data, 0.2f);
        break;
    case TYNIMA_FIELD_UINT32:
        ImGui::DragScalar(field.name, ImGuiDataType_U32, data, 0.2f);
        break;
    case TYNIMA_FIELD_INT64:
        ImGui::DragScalar(field.name, ImGuiDataType_S64, data, 0.2f);
        break;
    case TYNIMA_FIELD_UINT64:
        ImGui::DragScalar(field.name, ImGuiDataType_U64, data, 0.2f);
        break;
    case TYNIMA_FIELD_FLOAT:
        ImGui::DragFloat(field.name, static_cast<float*>(data), 0.01f, 0.0f, 0.0f, "%.3f");
        break;
    case TYNIMA_FIELD_DOUBLE:
        ImGui::DragScalar(field.name, ImGuiDataType_Double, data, 0.01f);
        break;
    case TYNIMA_FIELD_VEC2:
        ImGui::DragFloat2(field.name, static_cast<float*>(data), 0.01f, 0.0f, 0.0f, "%.3f");
        break;
    case TYNIMA_FIELD_VEC3:
        ImGui::DragFloat3(field.name, static_cast<float*>(data), 0.01f, 0.0f, 0.0f, "%.3f");
        break;
    case TYNIMA_FIELD_VEC4:
        ImGui::DragFloat4(field.name, static_cast<float*>(data), 0.01f, 0.0f, 0.0f, "%.3f");
        break;
    case TYNIMA_FIELD_QUAT: {
        // Edited as yaw, pitch and roll in degrees; the quaternion follows.
        auto* quat = static_cast<tynima_quat*>(data);
        float angles[3];
        tynima_quat_to_euler(*quat, &angles[0], &angles[1], &angles[2]);
        float euler[3] = {angles[0] * 180.0f / kPi, angles[1] * 180.0f / kPi, angles[2] * 180.0f / kPi};
        if (ImGui::DragFloat3(field.name, euler, 0.5f, 0.0f, 0.0f, "%.1f°")) {
            *quat = tynima_quat_from_euler(euler[0] * kPi / 180.0f, euler[1] * kPi / 180.0f,
                                           euler[2] * kPi / 180.0f);
        }
        break;
    }
    case TYNIMA_FIELD_MAT4: {
        const auto* m = static_cast<const tynima_mat4*>(data);
        ImGui::Text("%s", field.name);
        for (int row = 0; row < 4; ++row) {
            ImGui::Text("%8.3f %8.3f %8.3f %8.3f", static_cast<double>(m->m[row]),
                        static_cast<double>(m->m[4 + row]), static_cast<double>(m->m[8 + row]),
                        static_cast<double>(m->m[12 + row]));
        }
        break;
    }
    case TYNIMA_FIELD_ENTITY: {
        const auto* entity = static_cast<const tynima_entity*>(data);
        char label[TYNIMA_NAME_CAPACITY + 32];
        if (entity->generation == 0) {
            std::snprintf(label, sizeof label, "none");
        } else {
            entity_label(*api, engine, components, *entity, label, sizeof label);
        }
        ImGui::Text("%s: %s", field.name, label);
        if (entity->generation != 0 && api->entity_alive(engine, *entity)) {
            ImGui::SameLine();
            ImGui::PushID(field.name);
            if (ImGui::SmallButton("select")) {
                selected = *entity;
            }
            ImGui::PopID();
        }
        break;
    }
    case TYNIMA_FIELD_HANDLE: {
        const auto* handle = static_cast<const std::uint32_t*>(data);
        ImGui::Text("%s: #%u.%u", field.name, handle[0], handle[1]);
        break;
    }
    case TYNIMA_FIELD_STRING:
        ImGui::InputText(field.name, static_cast<char*>(data), field.count);
        break;
    case TYNIMA_FIELD_BYTES:
        ImGui::TextDisabled("%s: %u bytes", field.name, field.size);
        break;
    }
    ImGui::EndDisabled();
}

void Editor::draw_component(tynima_component_id id, const tynima_component_info& info, void* data) {
    const uint32_t field_count = api->component_field_count(engine, id);
    if (field_count == 0) {
        ImGui::TextDisabled("%u bytes, aligned to %u — not described by whoever registered it", info.size,
                            info.alignment);
        return;
    }
    // The bytes before any widget runs this frame. An edit that begins this
    // frame undoes to these; one that began earlier keeps the bytes it
    // started from, since a drag changes the value a little every frame.
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    frame_bytes.assign(bytes, bytes + info.size);
    for (uint32_t i = 0; i < field_count; ++i) {
        tynima_field field{};
        if (!api->component_field(engine, id, i, &field) || (field.flags & TYNIMA_FIELD_HIDDEN) != 0) {
            continue;
        }
        if (field.offset + field.size > info.size) {
            continue; // a description that lies: not this editor's problem to act on
        }
        ImGui::PushID(static_cast<int>(i));
        draw_field(field, static_cast<std::uint8_t*>(data) + field.offset);
        // An edit begins when a widget takes hold and ends when it lets go
        // having changed something: the whole component, as it was when the
        // widget took hold and as it is now, goes on the stack.
        if (ImGui::IsItemActivated()) {
            edit_bytes = frame_bytes;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Edit edit{.entity = selected, .component = id, .before = edit_bytes, .after = {}};
            if (edit.before.size() != info.size) {
                edit.before = frame_bytes; // took hold before this editor was watching: the frame's, at least
            }
            edit.after.assign(bytes, bytes + info.size);
            undo.push(std::move(edit));
            after_edit(selected, id);
        } else if (ImGui::IsItemActive() && !std::equal(frame_bytes.begin(), frame_bytes.end(), bytes)) {
            after_edit(selected, id); // mid-drag: the body follows as the value moves
        }
        ImGui::PopID();
    }
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
        const bool open = ImGui::CollapsingHeader(info.name, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24.0f);
        const bool remove = ImGui::SmallButton("x");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove %s", info.name);
        }
        if (open && !remove) {
            draw_component(ids[i], info, data);
        }
        ImGui::PopID();
        if (remove) {
            (void)api->remove_component(engine, selected, ids[i]);
            break; // the list just changed
        }
    }
    // Any registered component the entity lacks, added as its defaults.
    ImGui::Separator();
    if (ImGui::BeginCombo("##add", "Add component...")) {
        const uint32_t type_count = api->component_count(engine);
        for (uint32_t id = 0; id < type_count; ++id) {
            bool present = false;
            for (uint32_t i = 0; i < count; ++i) {
                present = present || ids[i] == id;
            }
            tynima_component_info info{};
            if (present || !api->component_info(engine, id, &info)) {
                continue;
            }
            if (ImGui::Selectable(info.name)) {
                std::vector<std::uint8_t> bytes(info.size, 0);
                if (const void* defaults = api->component_defaults(engine, id)) {
                    std::memcpy(bytes.data(), defaults, info.size);
                }
                (void)api->add_component(engine, selected, id, bytes.data());
            }
        }
        ImGui::EndCombo();
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
    const ImVec2 image_min = ImGui::GetItemRectMin();
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
    if (size.x >= 1.0f && size.y >= 1.0f) {
        draw_viewport_overlay(image_min, size, camera, hovered); // without a GPU too: over the notice
    }
    // A little overlay in the corner: where the camera is, and the gizmo's mode.
    static constexpr const char* kModes[] = {"move (W)", "rotate (E)", "scale (R)"};
    ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
    ImGui::TextDisabled("%.1f, %.1f, %.1f  %.1f m/s%s   %s%s", static_cast<double>(camera.position.x),
                        static_cast<double>(camera.position.y), static_cast<double>(camera.position.z),
                        static_cast<double>(speed), looking ? "  (looking)" : "  right-drag to look",
                        kModes[static_cast<int>(gizmo.mode)], gizmo.active >= 0 ? "  (shift snaps)" : "");
    ImGui::End();
}

// What the viewport draws over the picture: the selected entity's bounds,
// the gizmo, and the click that selects. Screen space is the image's.
void Editor::draw_viewport_overlay(const ImVec2& image_min, const ImVec2& image_size,
                                   const tynima_camera& camera, bool hovered) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float aspect = image_size.x / std::max(image_size.y, 1.0f);
    const auto project = [&](tynima_vec3 point, ImVec2* out) {
        float ndc_x, ndc_y, depth;
        if (!tynima_camera_project(&camera, aspect, point, &ndc_x, &ndc_y, &depth)) {
            return false;
        }
        *out = ImVec2(image_min.x + (ndc_x + 1.0f) * 0.5f * image_size.x,
                      image_min.y + (1.0f - ndc_y) * 0.5f * image_size.y);
        return true;
    };
    const ImVec2 mouse = ImGui::GetMousePos();
    const auto mouse_ray = [&]() {
        const float ndc_x = (mouse.x - image_min.x) / image_size.x * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (mouse.y - image_min.y) / image_size.y * 2.0f;
        return tynima_camera_ray(&camera, aspect, ndc_x, ndc_y);
    };
    const bool shift =
        api->key_down(engine, TYNIMA_KEY_LeftShift) || api->key_down(engine, TYNIMA_KEY_RightShift);
    const bool selected_alive = selected.generation != 0 && api->entity_alive(engine, selected);

    // The mode keys, while the mouse is over the picture and not looking.
    if (hovered && !looking && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            gizmo.mode = GizmoMode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            gizmo.mode = GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            gizmo.mode = GizmoMode::Scale;
    }

    // The selection's bounds: the model's box through the world matrix.
    auto* local_to_world = selected_alive ? static_cast<const tynima_local_to_world*>(api->get_component(
                                                engine, selected, components.local_to_world))
                                          : nullptr;
    const auto* renderer = selected_alive ? static_cast<const tynima_mesh_renderer*>(api->get_component(
                                                engine, selected, components.mesh_renderer))
                                          : nullptr;
    tynima_vec3 lo, hi;
    if (local_to_world != nullptr && renderer != nullptr &&
        tynima_model_bounds(engine, renderer->model, &lo, &hi)) {
        ImVec2 corners[8];
        bool visible = true;
        for (int i = 0; i < 8; ++i) {
            const tynima_vec3 local =
                tynima_vec3_make((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            const tynima_mat4& m = local_to_world->matrix;
            const tynima_vec3 world =
                tynima_vec3_add(tynima_vec3_add(tynima_vec3_add(tynima_vec3_scale(column(m, 0), local.x),
                                                                tynima_vec3_scale(column(m, 1), local.y)),
                                                tynima_vec3_scale(column(m, 2), local.z)),
                                column(m, 3));
            visible = visible && project(world, &corners[i]);
        }
        if (visible) {
            static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                                  {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& edge : kEdges) {
                draw->AddLine(corners[edge[0]], corners[edge[1]], IM_COL32(255, 200, 60, 160), 1.0f);
            }
        }
    }

    // The gizmo, for an entity with a transform.
    auto* transform =
        selected_alive
            ? static_cast<tynima_transform*>(api->get_component(engine, selected, components.transform))
            : nullptr;
    bool over_gizmo = false;
    if (transform != nullptr && !looking) {
        const tynima_vec3 center =
            local_to_world != nullptr ? column(local_to_world->matrix, 3) : transform->position;
        float depth = 0.0f;
        ImVec2 center_screen;
        const bool center_visible = project(center, &center_screen);
        if (center_visible) {
            tynima_camera_project(&camera, aspect, center, nullptr, nullptr, &depth);
        }
        // A size that looks the same at any distance: a fixed fraction of the view.
        const float size = depth * std::tan(0.5f * camera.fov_y) * 0.25f;
        tynima_vec3 axes[3] = {tynima_vec3_make(1.0f, 0.0f, 0.0f), tynima_vec3_make(0.0f, 1.0f, 0.0f),
                               tynima_vec3_make(0.0f, 0.0f, 1.0f)};
        if (gizmo.mode == GizmoMode::Scale && local_to_world != nullptr) {
            for (int i = 0; i < 3; ++i) {
                axes[i] = tynima_vec3_normalize(column(local_to_world->matrix, i));
            }
        }
        static constexpr ImU32 kAxisColors[3] = {IM_COL32(235, 70, 70, 255), IM_COL32(90, 210, 90, 255),
                                                 IM_COL32(80, 140, 240, 255)};
        static constexpr ImU32 kHot = IM_COL32(255, 230, 80, 255);
        constexpr float kHitDistance = 9.0f; // points

        // Hover: the nearest handle to the mouse, if any is near enough.
        if (gizmo.active < 0) {
            gizmo.hover = -1;
            float best = kHitDistance;
            if (center_visible && hovered) {
                for (int i = 0; i < 3; ++i) {
                    if (gizmo.mode == GizmoMode::Rotate) {
                        tynima_vec3 u, v;
                        plane_basis(axes[i], &u, &v);
                        ImVec2 previous;
                        bool have_previous = false;
                        for (int k = 0; k <= 48; ++k) {
                            const float angle = static_cast<float>(k) / 48.0f * 2.0f * kPi;
                            const tynima_vec3 point = tynima_vec3_add(
                                center,
                                tynima_vec3_scale(tynima_vec3_add(tynima_vec3_scale(u, std::cos(angle)),
                                                                  tynima_vec3_scale(v, std::sin(angle))),
                                                  size));
                            ImVec2 on_screen;
                            const bool ok = project(point, &on_screen);
                            if (ok && have_previous) {
                                const float d = segment_distance(mouse, previous, on_screen);
                                if (d < best) {
                                    best = d;
                                    gizmo.hover = i;
                                }
                            }
                            previous = on_screen;
                            have_previous = ok;
                        }
                    } else {
                        ImVec2 tip;
                        if (project(tynima_vec3_add(center, tynima_vec3_scale(axes[i], size)), &tip)) {
                            const float d = segment_distance(mouse, center_screen, tip);
                            if (d < best) {
                                best = d;
                                gizmo.hover = i;
                            }
                        }
                    }
                }
                if (gizmo.mode == GizmoMode::Scale) {
                    const float dx = mouse.x - center_screen.x, dy = mouse.y - center_screen.y;
                    if (std::sqrt(dx * dx + dy * dy) < kHitDistance) {
                        gizmo.hover = 3;
                    }
                }
            }
        }
        over_gizmo = gizmo.hover >= 0;

        // A drag begins on the hovered handle and ends when the button is released.
        if (gizmo.active < 0 && gizmo.hover >= 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            gizmo.active = gizmo.hover;
            gizmo.start_transform = *transform;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(transform);
            gizmo.start_bytes.assign(bytes, bytes + sizeof(tynima_transform));
            gizmo.center = center;
            gizmo.axis = axes[std::min(gizmo.active, 2)];
            plane_basis(gizmo.axis, &gizmo.plane_u, &gizmo.plane_v);
            gizmo.has_parent = false;
            if (const auto* parent = static_cast<const tynima_parent*>(
                    api->get_component(engine, selected, components.parent))) {
                if (const auto* parent_matrix = static_cast<const tynima_local_to_world*>(
                        api->get_component(engine, parent->entity, components.local_to_world))) {
                    gizmo.has_parent = true;
                    gizmo.parent_matrix = parent_matrix->matrix;
                    gizmo.parent_rotation =
                        quat_from_columns(column(parent_matrix->matrix, 0), column(parent_matrix->matrix, 1),
                                          column(parent_matrix->matrix, 2));
                }
            }
        }
        // Where the mouse ray meets the handle: the closest point on the axis
        // line, or the angle in the axis's plane.
        const auto axis_parameter = [&](tynima_vec3 c, tynima_vec3 a) {
            const tynima_vec3 d = mouse_ray();
            const tynima_vec3 r = tynima_vec3_sub(c, camera.position);
            const float b = tynima_vec3_dot(a, d), cc = tynima_vec3_dot(a, r), f = tynima_vec3_dot(d, r);
            const float denom = 1.0f - b * b;
            return denom > 1e-6f ? (b * f - cc) / denom : 0.0f;
        };
        const auto plane_angle = [&](tynima_vec3 c, tynima_vec3 n, tynima_vec3 u, tynima_vec3 v,
                                     float* angle) {
            const tynima_vec3 d = mouse_ray();
            const float denom = tynima_vec3_dot(d, n);
            if (std::fabs(denom) < 1e-6f) {
                return false;
            }
            const float t = tynima_vec3_dot(tynima_vec3_sub(c, camera.position), n) / denom;
            const tynima_vec3 hit =
                tynima_vec3_sub(tynima_vec3_add(camera.position, tynima_vec3_scale(d, t)), c);
            *angle = std::atan2(tynima_vec3_dot(hit, v), tynima_vec3_dot(hit, u));
            return true;
        };
        if (gizmo.active >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // The first frame: remember where the mouse took hold.
            if (gizmo.mode == GizmoMode::Rotate) {
                (void)plane_angle(gizmo.center, gizmo.axis, gizmo.plane_u, gizmo.plane_v, &gizmo.start_value);
            } else if (gizmo.active == 3) {
                gizmo.start_value = mouse.x;
            } else {
                gizmo.start_value = axis_parameter(gizmo.center, gizmo.axis);
            }
        }
        if (gizmo.active >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            *transform = gizmo.start_transform;
            if (gizmo.mode == GizmoMode::Translate) {
                float delta = axis_parameter(gizmo.center, gizmo.axis) - gizmo.start_value;
                if (shift) {
                    delta = std::round(delta / 0.1f) * 0.1f;
                }
                tynima_vec3 world_delta = tynima_vec3_scale(gizmo.axis, delta);
                tynima_vec3 local_delta = world_delta;
                if (gizmo.has_parent) {
                    // Along the parent's axes, scale and all: dot with each
                    // column over its length squared.
                    float components_local[3];
                    for (int i = 0; i < 3; ++i) {
                        const tynima_vec3 c = column(gizmo.parent_matrix, i);
                        const float length_sq = tynima_vec3_dot(c, c);
                        components_local[i] =
                            length_sq > 1e-12f ? tynima_vec3_dot(world_delta, c) / length_sq : 0.0f;
                    }
                    local_delta =
                        tynima_vec3_make(components_local[0], components_local[1], components_local[2]);
                }
                transform->position = tynima_vec3_add(gizmo.start_transform.position, local_delta);
            } else if (gizmo.mode == GizmoMode::Rotate) {
                float angle;
                if (plane_angle(gizmo.center, gizmo.axis, gizmo.plane_u, gizmo.plane_v, &angle)) {
                    float delta = angle - gizmo.start_value;
                    while (delta > kPi)
                        delta -= 2.0f * kPi;
                    while (delta < -kPi)
                        delta += 2.0f * kPi;
                    if (shift) {
                        const float step = 15.0f * kPi / 180.0f;
                        delta = std::round(delta / step) * step;
                    }
                    // About a world axis: R' = P^-1 D P R for a child of P.
                    tynima_quat turn = tynima_quat_from_axis_angle(gizmo.axis, delta);
                    if (gizmo.has_parent) {
                        turn = tynima_quat_mul(tynima_quat_mul(quat_conjugate(gizmo.parent_rotation), turn),
                                               gizmo.parent_rotation);
                    }
                    transform->rotation =
                        tynima_quat_normalize(tynima_quat_mul(turn, gizmo.start_transform.rotation));
                }
            } else if (gizmo.active == 3) {
                float factor = 1.0f + (mouse.x - gizmo.start_value) / 150.0f;
                if (shift) {
                    factor = std::round(factor / 0.1f) * 0.1f;
                }
                factor = std::max(factor, 0.01f);
                transform->scale = tynima_vec3_scale(gizmo.start_transform.scale, factor);
            } else {
                float factor = 1.0f + (axis_parameter(gizmo.center, gizmo.axis) - gizmo.start_value) / size;
                if (shift) {
                    factor = std::round(factor / 0.1f) * 0.1f;
                }
                factor = std::max(factor, 0.01f);
                float* scale = &transform->scale.x;
                scale[gizmo.active] = (&gizmo.start_transform.scale.x)[gizmo.active] * factor;
            }
            after_edit(selected, components.transform);
        }
        if (gizmo.active >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Edit edit{.entity = selected,
                      .component = components.transform,
                      .before = gizmo.start_bytes,
                      .after = {}};
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(transform);
            edit.after.assign(bytes, bytes + sizeof(tynima_transform));
            undo.push(std::move(edit));
            gizmo.active = -1;
        }

        // Drawn last, over everything: the axes, and the circles or the ends.
        if (center_visible) {
            for (int i = 0; i < 3; ++i) {
                const bool hot = gizmo.active == i || (gizmo.active < 0 && gizmo.hover == i);
                const ImU32 color = hot ? kHot : kAxisColors[i];
                if (gizmo.mode == GizmoMode::Rotate) {
                    tynima_vec3 u, v;
                    plane_basis(axes[i], &u, &v);
                    ImVec2 previous;
                    bool have_previous = false;
                    for (int k = 0; k <= 64; ++k) {
                        const float angle = static_cast<float>(k) / 64.0f * 2.0f * kPi;
                        const tynima_vec3 point = tynima_vec3_add(
                            center, tynima_vec3_scale(tynima_vec3_add(tynima_vec3_scale(u, std::cos(angle)),
                                                                      tynima_vec3_scale(v, std::sin(angle))),
                                                      size));
                        ImVec2 on_screen;
                        const bool ok = project(point, &on_screen);
                        if (ok && have_previous) {
                            draw->AddLine(previous, on_screen, color, hot ? 3.0f : 2.0f);
                        }
                        previous = on_screen;
                        have_previous = ok;
                    }
                } else {
                    ImVec2 tip;
                    if (!project(tynima_vec3_add(center, tynima_vec3_scale(axes[i], size)), &tip)) {
                        continue;
                    }
                    draw->AddLine(center_screen, tip, color, hot ? 4.0f : 2.5f);
                    if (gizmo.mode == GizmoMode::Translate) {
                        // An arrowhead pointing along the axis on screen.
                        float dx = tip.x - center_screen.x, dy = tip.y - center_screen.y;
                        const float length = std::sqrt(dx * dx + dy * dy);
                        if (length > 1.0f) {
                            dx /= length;
                            dy /= length;
                            const ImVec2 base(tip.x - dx * 12.0f, tip.y - dy * 12.0f);
                            draw->AddTriangleFilled(tip, ImVec2(base.x - dy * 5.0f, base.y + dx * 5.0f),
                                                    ImVec2(base.x + dy * 5.0f, base.y - dx * 5.0f), color);
                        }
                    } else {
                        draw->AddRectFilled(ImVec2(tip.x - 5.0f, tip.y - 5.0f),
                                            ImVec2(tip.x + 5.0f, tip.y + 5.0f), color);
                    }
                }
            }
            if (gizmo.mode == GizmoMode::Scale) {
                const bool hot = gizmo.active == 3 || (gizmo.active < 0 && gizmo.hover == 3);
                draw->AddRectFilled(ImVec2(center_screen.x - 6.0f, center_screen.y - 6.0f),
                                    ImVec2(center_screen.x + 6.0f, center_screen.y + 6.0f),
                                    hot ? kHot : IM_COL32(220, 220, 220, 255));
            }
        }
    } else {
        gizmo.hover = -1;
        gizmo.active = -1;
    }

    // A click on the picture, not on a handle: select what is there, or nothing.
    if (hovered && !looking && !over_gizmo && gizmo.active < 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        tynima_entity hit{};
        float distance = 0.0f;
        selected = tynima_pick(engine, camera.position, mouse_ray(), &hit, &distance) ? hit : tynima_entity{};
    }
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
    ImGui::Text("%u models, %u reloaded; %u game module reloads", tynima_model_count(engine),
                stats.model_reloads, stats.game_reloads);
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
    if (!options.scene.empty()) {
        editor.load_scene(options.scene);
        if (editor.scene_status.rfind("open failed", 0) == 0) {
            std::fprintf(stderr, "tynima-editor: %s\n", editor.scene_status.c_str());
        }
    }

    long frames = 0;
    while (tynima_engine_begin_frame(engine)) {
        editor.draw_menu_bar();
        const ImGuiID dockspace = ImGui::GetID("editor dockspace");
        if (editor.reset_layout || ImGui::DockBuilderGetNode(dockspace) == nullptr) {
            editor.layout(dockspace);
            editor.reset_layout = false;
        }
        ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
        // The shortcuts, routed globally (ImGui reads Ctrl as Cmd on a Mac);
        // a text field being edited keeps its own Cmd+Z.
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
            editor.undo_last();
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
            editor.redo_last();
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
            if (editor.scene_path.empty()) {
                editor.path_dialog = Editor::PathDialog::SaveAs;
            } else {
                editor.save_scene(editor.scene_path);
            }
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
            editor.path_dialog = Editor::PathDialog::SaveAs;
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
            editor.path_dialog = Editor::PathDialog::Open;
        }
        editor.draw_path_dialog();
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
