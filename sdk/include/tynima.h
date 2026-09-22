/*
 * tynima.h — the Tynima public C ABI.
 *
 * Everything a game module may call is declared here, in C, so it can be
 * bound from any language and survive hot reload across a library boundary.
 * A game module never links the engine: on load it receives a tynima_api
 * table of function pointers and a tynima_engine context, and calls through
 * those. Engine internals (tynima/<module>/...) are never visible here.
 *
 * The second half is for hosts: the runtime that runs a game, and the
 * editor, which is a host with panels — tynima_engine_create() brings up
 * the window, the GPU, the world, the physics and the game module, and
 * tynima_engine_begin_frame()/end_frame() run a frame of all of it. A host
 * sees the world through the same table a game module does; the editor is
 * a client of this header and nothing else.
 *
 * Version history:
 *   1  components, entities, chunk iteration, keys, time, log
 *   2  log takes a level
 *   3  physics: impulses on a RigidBody's body
 *   4  hosting: the engine, frames, models, the camera, the UI, the log ring;
 *      component queries; the built-in components as C structs; body poses
 *   5  reflection: a component's fields, read by tools and described by modules
 *   6  a component's defaults; scenes saved and loaded as text
 *   7  picking, a model's bounds, and the camera's rays and projection inline
 *   8  models cook on load and reload when their file changes
 *   9  a game's own half: the camera, the lighting, models from shapes,
 *      physics bodies and ray casts, all through the table a module gets
 *  10  no new entries: the engine can run Lua scripts against this table
 *      (engine/script), and tynima_stats counts how often one reloaded
 *
 * The rule that keeps this ABI: the table only ever grows at the end, and
 * no declaration in it ever changes. A module built against an older
 * version loads into a newer engine and calls the entries it knows; a
 * module built against a newer one is refused, since the entries it wants
 * are not there. sdk/src/abi.cpp pins every layout this file promises, so
 * a change that would break a built module fails the build instead.
 */
#ifndef TYNIMA_H
#define TYNIMA_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TYNIMA_API_VERSION 10u
/* The oldest a game module may have been built against and still load. */
#define TYNIMA_API_VERSION_MIN 1u

/* ---- engine version ---- */

typedef struct tynima_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} tynima_version;

tynima_version tynima_get_version(void);
const char* tynima_get_version_string(void); /* "major.minor.patch", static storage */

/* ---- math ---- */

/* The engine's own layouts: three or four floats, no padding. */
typedef struct tynima_vec3 {
    float x, y, z;
} tynima_vec3;

typedef struct tynima_vec4 {
    float x, y, z, w;
} tynima_vec4;

/* A unit quaternion; (0, 0, 0, 1) is no rotation. */
typedef struct tynima_quat {
    float x, y, z, w;
} tynima_quat;

/* Column-major, like the engine's Mat4: m[column * 4 + row]. */
typedef struct tynima_mat4 {
    float m[16];
} tynima_mat4;

/* ---- a little math, inline, for hosts and modules written in C ---- */

static inline tynima_vec3 tynima_vec3_make(float x, float y, float z) {
    tynima_vec3 v = {x, y, z};
    return v;
}
static inline tynima_vec3 tynima_vec3_add(tynima_vec3 a, tynima_vec3 b) {
    return tynima_vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline tynima_vec3 tynima_vec3_sub(tynima_vec3 a, tynima_vec3 b) {
    return tynima_vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline tynima_vec3 tynima_vec3_scale(tynima_vec3 v, float s) {
    return tynima_vec3_make(v.x * s, v.y * s, v.z * s);
}
static inline float tynima_vec3_dot(tynima_vec3 a, tynima_vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline tynima_vec3 tynima_vec3_cross(tynima_vec3 a, tynima_vec3 b) {
    return tynima_vec3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float tynima_vec3_length(tynima_vec3 v) {
    return sqrtf(tynima_vec3_dot(v, v));
}
/* Zero stays zero. */
static inline tynima_vec3 tynima_vec3_normalize(tynima_vec3 v) {
    const float length = tynima_vec3_length(v);
    return length > 0.0f ? tynima_vec3_scale(v, 1.0f / length) : v;
}

static inline tynima_quat tynima_quat_identity(void) {
    tynima_quat q = {0.0f, 0.0f, 0.0f, 1.0f};
    return q;
}
/* `axis` must be unit length; `angle` in radians, right-hand rule. */
static inline tynima_quat tynima_quat_from_axis_angle(tynima_vec3 axis, float angle) {
    const float s = sinf(0.5f * angle), c = cosf(0.5f * angle);
    tynima_quat q = {axis.x * s, axis.y * s, axis.z * s, c};
    return q;
}
/* The rotation that applies b first and then a, like the engine's Quat operator*. */
static inline tynima_quat tynima_quat_mul(tynima_quat a, tynima_quat b) {
    tynima_quat q = {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return q;
}
static inline tynima_quat tynima_quat_normalize(tynima_quat q) {
    const float length = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length <= 0.0f) {
        return tynima_quat_identity();
    }
    q.x /= length;
    q.y /= length;
    q.z /= length;
    q.w /= length;
    return q;
}
/* Rotates v by the unit quaternion q. */
static inline tynima_vec3 tynima_quat_rotate(tynima_quat q, tynima_vec3 v) {
    const tynima_vec3 u = tynima_vec3_make(q.x, q.y, q.z);
    const tynima_vec3 t = tynima_vec3_scale(tynima_vec3_cross(u, v), 2.0f);
    return tynima_vec3_add(tynima_vec3_add(v, tynima_vec3_scale(t, q.w)), tynima_vec3_cross(u, t));
}
/* Yaw about +y, then pitch about the resulting +x, then roll about the
 * resulting +z: the order a camera or a character wants. Radians. */
static inline tynima_quat tynima_quat_from_euler(float yaw, float pitch, float roll) {
    const tynima_quat y = tynima_quat_from_axis_angle(tynima_vec3_make(0.0f, 1.0f, 0.0f), yaw);
    const tynima_quat x = tynima_quat_from_axis_angle(tynima_vec3_make(1.0f, 0.0f, 0.0f), pitch);
    const tynima_quat z = tynima_quat_from_axis_angle(tynima_vec3_make(0.0f, 0.0f, 1.0f), roll);
    return tynima_quat_mul(tynima_quat_mul(y, x), z);
}
/* The inverse of the above, for a unit quaternion: pitch in [-pi/2, pi/2]. */
static inline void tynima_quat_to_euler(tynima_quat q, float* yaw, float* pitch, float* roll) {
    /* The rotated basis, then the angles read off it: forward tells yaw and pitch, up the roll. */
    const tynima_vec3 f = tynima_quat_rotate(q, tynima_vec3_make(0.0f, 0.0f, -1.0f));
    const tynima_vec3 u = tynima_quat_rotate(q, tynima_vec3_make(0.0f, 1.0f, 0.0f));
    const float p = asinf(f.y < -1.0f ? -1.0f : (f.y > 1.0f ? 1.0f : f.y));
    const float yw = atan2f(-f.x, -f.z);
    /* Undo yaw and pitch from the up vector: what is left is the roll about forward. */
    const tynima_quat unyaw_unpitch =
        tynima_quat_mul(tynima_quat_from_axis_angle(tynima_vec3_make(1.0f, 0.0f, 0.0f), -p),
                        tynima_quat_from_axis_angle(tynima_vec3_make(0.0f, 1.0f, 0.0f), -yw));
    const tynima_vec3 u0 = tynima_quat_rotate(unyaw_unpitch, u);
    if (yaw)
        *yaw = yw;
    if (pitch)
        *pitch = p;
    if (roll)
        *roll = atan2f(-u0.x, u0.y);
}

/* ---- world ---- */

/* An entity handle: index and generation, the same bits as scene::Entity.
 * Generation 0 is the null entity. */
typedef struct tynima_entity {
    uint32_t index;
    uint32_t generation;
} tynima_entity;

typedef uint32_t tynima_component_id;
#define TYNIMA_NO_COMPONENT 0xFFFFFFFFu
#define TYNIMA_MAX_COMPONENT_TYPES 64u

/* What the world knows about a component type. `name` has static storage. */
typedef struct tynima_component_info {
    const char* name;
    uint32_t size;
    uint32_t alignment;
} tynima_component_info;

/* A component's fields, as reflection sees them (core/reflect.h): what an
 * inspector draws and a scene file writes. The engine describes its own
 * components; a module describes its through the table. */
typedef enum tynima_field_kind {
    TYNIMA_FIELD_BOOL,
    TYNIMA_FIELD_INT8,
    TYNIMA_FIELD_UINT8,
    TYNIMA_FIELD_INT16,
    TYNIMA_FIELD_UINT16,
    TYNIMA_FIELD_INT32,
    TYNIMA_FIELD_UINT32,
    TYNIMA_FIELD_INT64,
    TYNIMA_FIELD_UINT64,
    TYNIMA_FIELD_FLOAT,
    TYNIMA_FIELD_DOUBLE,
    TYNIMA_FIELD_VEC2,
    TYNIMA_FIELD_VEC3,
    TYNIMA_FIELD_VEC4,
    TYNIMA_FIELD_QUAT,
    TYNIMA_FIELD_MAT4,
    TYNIMA_FIELD_ENTITY, /* a tynima_entity */
    TYNIMA_FIELD_HANDLE, /* another two-integer handle: shown, never edited */
    TYNIMA_FIELD_STRING, /* a char array of `count` bytes, NUL-terminated */
    TYNIMA_FIELD_BYTES   /* anything else: `size` opaque bytes */
} tynima_field_kind;

#define TYNIMA_FIELD_READ_ONLY 1u /* shown, never edited */
#define TYNIMA_FIELD_HIDDEN 2u    /* an implementation detail: not shown */
#define TYNIMA_MAX_COMPONENT_FIELDS 64u

typedef struct tynima_field {
    const char* name;
    tynima_field_kind kind;
    uint32_t offset; /* bytes from the component's start */
    uint32_t size;   /* bytes */
    uint32_t count;  /* a string's capacity; 1 otherwise */
    uint32_t flags;  /* TYNIMA_FIELD_* */
} tynima_field;

/* Called once per chunk of entities: `count` entities, and for each
 * requested component a pointer to its contiguous array, in request order. */
typedef void (*tynima_chunk_fn)(void* user, const tynima_entity* entities, uint32_t count,
                                void* const* columns);

/* ---- the built-in components, as C sees them ----
 *
 * The engine registers these under the names given; the layouts below are
 * the engine's own, checked against it when the engine is built. A module
 * written in C++ may include tynima/scene/components.h instead. */

#define TYNIMA_COMPONENT_NAME "Name"
#define TYNIMA_COMPONENT_TRANSFORM "Transform"
#define TYNIMA_COMPONENT_LOCAL_TO_WORLD "LocalToWorld"
#define TYNIMA_COMPONENT_PARENT "Parent"
#define TYNIMA_COMPONENT_MESH_RENDERER "MeshRenderer"
#define TYNIMA_COMPONENT_RIGID_BODY "RigidBody"

/* What an entity is called, for people. NUL-terminated; longer names are cut. */
#define TYNIMA_NAME_CAPACITY 32u
typedef struct tynima_name {
    char text[TYNIMA_NAME_CAPACITY];
} tynima_name;

/* Where an entity is, relative to its parent (or the world). */
typedef struct tynima_transform {
    tynima_vec3 position;
    tynima_quat rotation;
    tynima_vec3 scale;
} tynima_transform;

/* The entity's world matrix, written by the engine every frame. */
typedef struct tynima_local_to_world {
    tynima_mat4 matrix;
} tynima_local_to_world;

/* Makes the entity's transform relative to another entity's. */
typedef struct tynima_parent {
    tynima_entity entity;
} tynima_parent;

/* Draws a model (tynima_load_model's index) at the entity's world matrix. */
typedef struct tynima_mesh_renderer {
    uint32_t model;
    bool visible;
} tynima_mesh_renderer;

/* A physics body handle: the same bits as physics::BodyHandle. Generation 0 is null. */
typedef struct tynima_body {
    uint32_t index;
    uint32_t generation;
} tynima_body;

/* Ties the entity to a body, which drives its transform. The rest is the
 * engine's: where the body was before the last step, for drawing between
 * steps. Change a body's pose through the table, not through the struct. */
typedef struct tynima_rigid_body {
    tynima_body body;
    tynima_vec3 previous_position;
    tynima_quat previous_rotation;
    bool has_previous;
} tynima_rigid_body;

/* ---- input ---- */

/* Physical keys — where the key is on the board, not what is printed on
 * it: TYNIMA_KEY_A is the key left of S on a US layout wherever it is on
 * yours. The values match the engine's platform::Key exactly, which
 * sdk/src/api.cpp asserts name by name, and new keys are only ever
 * appended, so a module built against an older header keeps its numbers. */
typedef enum tynima_key {
    TYNIMA_KEY_Unknown, TYNIMA_KEY_A, TYNIMA_KEY_B, TYNIMA_KEY_C, TYNIMA_KEY_D, TYNIMA_KEY_E,
    TYNIMA_KEY_F, TYNIMA_KEY_G, TYNIMA_KEY_H, TYNIMA_KEY_I, TYNIMA_KEY_J, TYNIMA_KEY_K, TYNIMA_KEY_L,
    TYNIMA_KEY_M, TYNIMA_KEY_N, TYNIMA_KEY_O, TYNIMA_KEY_P, TYNIMA_KEY_Q, TYNIMA_KEY_R, TYNIMA_KEY_S,
    TYNIMA_KEY_T, TYNIMA_KEY_U, TYNIMA_KEY_V, TYNIMA_KEY_W, TYNIMA_KEY_X, TYNIMA_KEY_Y, TYNIMA_KEY_Z,
    TYNIMA_KEY_Digit0, TYNIMA_KEY_Digit1, TYNIMA_KEY_Digit2, TYNIMA_KEY_Digit3, TYNIMA_KEY_Digit4,
    TYNIMA_KEY_Digit5, TYNIMA_KEY_Digit6, TYNIMA_KEY_Digit7, TYNIMA_KEY_Digit8, TYNIMA_KEY_Digit9,
    TYNIMA_KEY_F1, TYNIMA_KEY_F2, TYNIMA_KEY_F3, TYNIMA_KEY_F4, TYNIMA_KEY_F5, TYNIMA_KEY_F6,
    TYNIMA_KEY_F7, TYNIMA_KEY_F8, TYNIMA_KEY_F9, TYNIMA_KEY_F10, TYNIMA_KEY_F11, TYNIMA_KEY_F12,
    TYNIMA_KEY_Escape, TYNIMA_KEY_Enter, TYNIMA_KEY_Tab, TYNIMA_KEY_Backspace, TYNIMA_KEY_Space,
    TYNIMA_KEY_Minus, TYNIMA_KEY_Equals, TYNIMA_KEY_LeftBracket, TYNIMA_KEY_RightBracket,
    TYNIMA_KEY_Backslash, TYNIMA_KEY_Semicolon, TYNIMA_KEY_Apostrophe, TYNIMA_KEY_Grave, TYNIMA_KEY_Comma,
    TYNIMA_KEY_Period, TYNIMA_KEY_Slash, TYNIMA_KEY_CapsLock, TYNIMA_KEY_Insert, TYNIMA_KEY_Delete,
    TYNIMA_KEY_Home, TYNIMA_KEY_End, TYNIMA_KEY_PageUp, TYNIMA_KEY_PageDown, TYNIMA_KEY_Left,
    TYNIMA_KEY_Right, TYNIMA_KEY_Up, TYNIMA_KEY_Down, TYNIMA_KEY_LeftShift, TYNIMA_KEY_RightShift,
    TYNIMA_KEY_LeftCtrl, TYNIMA_KEY_RightCtrl, TYNIMA_KEY_LeftAlt, TYNIMA_KEY_RightAlt,
    TYNIMA_KEY_LeftSuper, TYNIMA_KEY_RightSuper,
    TYNIMA_KEY_COUNT
} tynima_key;

typedef enum tynima_mouse_button {
    TYNIMA_MOUSE_LEFT,
    TYNIMA_MOUSE_MIDDLE,
    TYNIMA_MOUSE_RIGHT,
    TYNIMA_MOUSE_X1,
    TYNIMA_MOUSE_X2,
    TYNIMA_MOUSE_BUTTON_COUNT
} tynima_mouse_button;

/* ---- logging ---- */

/* Severity, matching the engine's core::LogLevel. Events below the engine's
 * minimum level (Debug in Debug builds, Info otherwise, or the TYNIMA_LOG
 * environment variable) are dropped. */
typedef enum tynima_log_level {
    TYNIMA_LOG_TRACE,
    TYNIMA_LOG_DEBUG,
    TYNIMA_LOG_INFO,
    TYNIMA_LOG_WARN,
    TYNIMA_LOG_ERROR,
    TYNIMA_LOG_FATAL
} tynima_log_level;

/* ---- what a game sets up: the view, the light, the shapes ----
 *
 * These are the engine's own, in C: the camera the scene is drawn from,
 * the light it is lit by, and the shape a body collides with — which is
 * also the shape a placeholder model is made of, so a scene can be built
 * before there is any art. A game reaches them through the table below; a
 * host, through the functions of the same names further down. */

/* The camera the scene is drawn from. Identity rotation looks down -z; the
 * projection is infinite, from `near` metres out. */
typedef struct tynima_camera {
    tynima_vec3 position;
    tynima_quat rotation;
    float fov_y; /* radians */
    float near;  /* metres */
} tynima_camera;

/* The one directional light, the ambient term beside it, and the sky the
 * frame is cleared to. Colours are linear radiance: above 1 is what the
 * tonemapper is for. */
typedef struct tynima_lighting {
    tynima_vec3 sun_direction; /* towards the light, unit length */
    float sun_intensity;
    tynima_vec3 sun_color;
    float ambient;
    tynima_vec3 sky;
} tynima_lighting;

/* A point light: where, how far it reaches, and its colour. Beyond
 * `radius` it contributes nothing. */
#define TYNIMA_MAX_LIGHTS 256u
typedef struct tynima_point_light {
    tynima_vec3 position;
    float radius; /* metres */
    tynima_vec3 color;
} tynima_point_light;

/* A collision shape, at `center` in the body's own frame — so a mesh whose
 * origin is at its base can still carry a box around its middle. */
typedef enum tynima_shape_type {
    TYNIMA_SHAPE_BOX,
    TYNIMA_SHAPE_SPHERE,
    TYNIMA_SHAPE_CAPSULE
} tynima_shape_type;
typedef struct tynima_shape {
    tynima_shape_type type;
    tynima_vec3 half_extents; /* box */
    float radius;             /* sphere, capsule */
    float half_height;        /* capsule: half the straight part */
    tynima_vec3 center;
} tynima_shape;
static inline tynima_shape tynima_shape_box(tynima_vec3 half_extents) {
    tynima_shape s;
    s.type = TYNIMA_SHAPE_BOX;
    s.half_extents = half_extents;
    s.radius = 0.5f;
    s.half_height = 0.5f;
    s.center = tynima_vec3_make(0.0f, 0.0f, 0.0f);
    return s;
}
static inline tynima_shape tynima_shape_sphere(float radius) {
    tynima_shape s = tynima_shape_box(tynima_vec3_make(0.5f, 0.5f, 0.5f));
    s.type = TYNIMA_SHAPE_SPHERE;
    s.radius = radius;
    return s;
}
static inline tynima_shape tynima_shape_capsule(float radius, float half_height) {
    tynima_shape s = tynima_shape_sphere(radius);
    s.type = TYNIMA_SHAPE_CAPSULE;
    s.half_height = half_height;
    return s;
}

/* How a body moves: the floor never, a platform when told, everything else
 * because the simulation says so. */
typedef enum tynima_motion {
    TYNIMA_MOTION_STATIC,
    TYNIMA_MOTION_KINEMATIC, /* moved by hand; pushes dynamic bodies, is not pushed */
    TYNIMA_MOTION_DYNAMIC
} tynima_motion;

typedef struct tynima_body_desc {
    tynima_shape shape;
    tynima_vec3 position;
    tynima_quat rotation;
    tynima_motion motion;
    float mass;        /* kg; 0 = from the shape's volume at the density of water */
    float friction;    /* 0.5 is a sensible start */
    float restitution; /* 0 no bounce, 1 all of it back */
    tynima_vec3 linear_velocity;
    tynima_vec3 angular_velocity;
    bool start_active;   /* false: asleep until something touches it */
    bool lock_rotation;  /* never turns: a character capsule, a puck */
    uint64_t user_data;  /* the game's own, e.g. an entity packed into 64 bits */
} tynima_body_desc;
/* A body's description with everything at its default: a dynamic box of
 * half a metre at the origin. */
static inline tynima_body_desc tynima_body_desc_make(tynima_shape shape, tynima_vec3 position) {
    tynima_body_desc d;
    d.shape = shape;
    d.position = position;
    d.rotation = tynima_quat_identity();
    d.motion = TYNIMA_MOTION_DYNAMIC;
    d.mass = 0.0f;
    d.friction = 0.5f;
    d.restitution = 0.0f;
    d.linear_velocity = tynima_vec3_make(0.0f, 0.0f, 0.0f);
    d.angular_velocity = tynima_vec3_make(0.0f, 0.0f, 0.0f);
    d.start_active = true;
    d.lock_rotation = false;
    d.user_data = 0;
    return d;
}

/* What a ray met: the body, where, the surface's normal there, and how far
 * along the ray it is. */
typedef struct tynima_ray_hit {
    tynima_body body;
    tynima_vec3 position;
    tynima_vec3 normal;
    float distance;
    uint64_t user_data; /* the body's */
} tynima_ray_hit;

/* ---- the API a game module receives ---- */

/* Opaque: the host's engine state for this module. Pass it back unchanged. */
typedef struct tynima_engine tynima_engine;

typedef struct tynima_api {
    uint32_t version; /* TYNIMA_API_VERSION of the host */

    /* Components are identified by name; registering the same name again
     * returns the same id, and the layout must match. */
    tynima_component_id (*register_component)(tynima_engine* engine, const char* name, uint32_t size,
                                              uint32_t alignment);

    /* Entities. `values[i]` is copied as `size` bytes of component `ids[i]`.
     * A null entity comes back when the world is full. */
    tynima_entity (*create_entity)(tynima_engine* engine, const tynima_component_id* ids,
                                   const void* const* values, uint32_t count);
    bool (*destroy_entity)(tynima_engine* engine, tynima_entity entity);
    bool (*entity_alive)(tynima_engine* engine, tynima_entity entity);
    uint32_t (*entity_count)(tynima_engine* engine);

    /* NULL when the entity is dead or lacks the component; valid until the
     * next structural change. */
    void* (*get_component)(tynima_engine* engine, tynima_entity entity, tynima_component_id id);
    bool (*add_component)(tynima_engine* engine, tynima_entity entity, tynima_component_id id,
                          const void* value);
    bool (*remove_component)(tynima_engine* engine, tynima_entity entity, tynima_component_id id);

    /* Iterates every chunk whose entities have all `count` components — with
     * `count` 0, every entity there is. No structural changes from inside the callback. */
    void (*each_chunk)(tynima_engine* engine, const tynima_component_id* ids, uint32_t count,
                       tynima_chunk_fn fn, void* user);

    /* Input, as of the current frame. */
    bool (*key_down)(tynima_engine* engine, tynima_key key);
    bool (*key_pressed)(tynima_engine* engine, tynima_key key);
    bool (*key_released)(tynima_engine* engine, tynima_key key);

    /* Seconds since the engine started, as the host's frame times add up: in
     * a replay it reads exactly as it did in the run that was recorded. */
    double (*time_seconds)(tynima_engine* engine);

    /* Physics. A stale body, or a host without a physics world, is a no-op.
     * Impulses are newton-seconds, applied at once; the body wakes up. The
     * second form applies it at a world-space point, which also spins the body. */
    void (*body_add_impulse)(tynima_engine* engine, tynima_body body, tynima_vec3 impulse);
    void (*body_add_impulse_at)(tynima_engine* engine, tynima_body body, tynima_vec3 impulse,
                                tynima_vec3 point);

    /* One line to the engine's log, in the "game" category. */
    void (*log)(tynima_engine* engine, tynima_log_level level, const char* message);

    /* ---- version 4 ---- */

    /* The component types the world knows: `find_component` answers with
     * TYNIMA_NO_COMPONENT when nothing is registered under the name (it never
     * registers); `component_info` is false for an id that is not one. */
    uint32_t (*component_count)(tynima_engine* engine);
    tynima_component_id (*find_component)(tynima_engine* engine, const char* name);
    bool (*component_info)(tynima_engine* engine, tynima_component_id id, tynima_component_info* out);
    /* The ids an entity has, ascending, into `ids` (at most `max`); returns
     * how many it has in all, 0 for a dead entity. */
    uint32_t (*entity_components)(tynima_engine* engine, tynima_entity entity, tynima_component_id* ids,
                                  uint32_t max);

    /* The mouse, in window points; buttons as of the current frame. */
    bool (*mouse_down)(tynima_engine* engine, tynima_mouse_button button);
    bool (*mouse_pressed)(tynima_engine* engine, tynima_mouse_button button);
    bool (*mouse_released)(tynima_engine* engine, tynima_mouse_button button);
    void (*mouse_position)(tynima_engine* engine, float* x, float* y);
    void (*mouse_delta)(tynima_engine* engine, float* dx, float* dy);
    void (*mouse_wheel)(tynima_engine* engine, float* dx, float* dy);

    /* Moves a body outright — teleports it, keeping its velocity — or sets
     * its velocities. A stale body is a no-op. */
    void (*body_set_transform)(tynima_engine* engine, tynima_body body, tynima_vec3 position,
                               tynima_quat rotation);
    void (*body_set_velocity)(tynima_engine* engine, tynima_body body, tynima_vec3 linear,
                              tynima_vec3 angular);

    /* ---- version 5 ---- */

    /* A component's fields: how many, and each by index (false past them,
     * or for an id that is not a component). The name in `out` is the
     * engine's copy and stays valid. */
    uint32_t (*component_field_count)(tynima_engine* engine, tynima_component_id id);
    bool (*component_field)(tynima_engine* engine, tynima_component_id id, uint32_t index, tynima_field* out);
    /* Describes a component the caller registered: the fields are copied,
     * names included, so a module's own strings need not outlive its reload.
     * A component already described keeps its description; false for a bad
     * id, more than TYNIMA_MAX_COMPONENT_FIELDS, or a field outside it. */
    bool (*describe_component)(tynima_engine* engine, tynima_component_id id, const tynima_field* fields,
                               uint32_t count);

    /* ---- version 6 ---- */

    /* What a new instance of a component starts out as: `size` bytes, the
     * engine's copy, valid as long as the engine; NULL when nobody said (a
     * tool then starts from zeros). set_component_defaults gives a component
     * the caller registered its defaults; the first ones given stand. */
    const void* (*component_defaults)(tynima_engine* engine, tynima_component_id id);
    bool (*set_component_defaults)(tynima_engine* engine, tynima_component_id id, const void* defaults);

    /* ---- version 9: the half a game needs and only a host had ---- */

    /* Ends the run: the host's next frame is its last. */
    void (*quit)(tynima_engine* engine);

    /* The camera and the light, read and written between frames. */
    void (*get_camera)(tynima_engine* engine, tynima_camera* out);
    void (*set_camera)(tynima_engine* engine, const tynima_camera* camera);
    void (*get_lighting)(tynima_engine* engine, tynima_lighting* out);
    void (*set_lighting)(tynima_engine* engine, const tynima_lighting* lighting);
    /* This frame's point lights, copied; at most TYNIMA_MAX_LIGHTS. Set them
     * again whenever they move — the engine keeps the last set. */
    void (*set_point_lights)(tynima_engine* engine, const tynima_point_light* lights, uint32_t count);

    /* Models. A file (a cooked .tymodel, or a .gltf/.glb cooked on the way
     * in), or a shape with one plain material — a box, a sphere, a capsule
     * to build a scene out of before there is art. Both answer with the
     * index a MeshRenderer's `model` refers to, TYNIMA_NO_MODEL on failure. */
    uint32_t (*load_model)(tynima_engine* engine, const char* path);
    uint32_t (*shape_model)(tynima_engine* engine, const tynima_shape* shape, tynima_vec4 color,
                            float roughness);

    /* Bodies. A body is the physics world's; an entity refers to one through
     * its RigidBody component, and its Transform follows the body each frame. */
    tynima_body (*create_body)(tynima_engine* engine, const tynima_body_desc* desc);
    bool (*destroy_body)(tynima_engine* engine, tynima_body body);
    /* Where the body is now, and how fast; false for a stale handle. */
    bool (*body_transform)(tynima_engine* engine, tynima_body body, tynima_vec3* position,
                           tynima_quat* rotation);
    bool (*body_velocity)(tynima_engine* engine, tynima_body body, tynima_vec3* linear,
                          tynima_vec3* angular);
    /* The nearest body along a ray (origin, unit direction), within
     * `max_distance` metres. False when it meets nothing. */
    bool (*cast_ray)(tynima_engine* engine, tynima_vec3 origin, tynima_vec3 direction, float max_distance,
                     tynima_ray_hit* out);

    /* The window, in points — the units the mouse is in. 0 x 0 headless. */
    void (*window_size)(tynima_engine* engine, uint32_t* width, uint32_t* height);
    /* Hides the cursor and reports unbounded motion: mouse look. */
    void (*set_relative_mouse)(tynima_engine* engine, bool enabled);
} tynima_api;

/* ---- what a game module exports ---- */

/* The module's entry points. Keep all state in the world: the module is
 * unloaded and reloaded while the engine — and the world — keep running. */
typedef struct tynima_game {
    uint32_t api_version; /* TYNIMA_API_VERSION the module was built against */
    /* After the module is loaded. `reloaded` is true after a hot reload. */
    void (*load)(const tynima_api* api, tynima_engine* engine, bool reloaded);
    /* Before the module is unloaded. `reloading` is true when a new build is about to replace it. */
    void (*unload)(const tynima_api* api, tynima_engine* engine, bool reloading);
    /* Once per frame. */
    void (*update)(const tynima_api* api, tynima_engine* engine, float dt);
} tynima_game;

#if defined(_WIN32)
#define TYNIMA_GAME_EXPORT __declspec(dllexport)
#else
#define TYNIMA_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* The one symbol a game module must export:
 *   extern "C" TYNIMA_GAME_EXPORT const tynima_game* tynima_game_entry(void);
 */
#define TYNIMA_GAME_ENTRY_NAME "tynima_game_entry"

/* ================================================================== hosts ==
 *
 * The rest is for the program that runs the engine — a game's runtime, or
 * the editor — never for a game module, which is run by one of these.
 */

/* ---- the engine ---- */

typedef struct tynima_engine_desc {
    const char* title;      /* the window's; NULL: "Tynima" */
    uint32_t width, height; /* the window, in points; 0: 1280 x 720 */
    bool headless;          /* no window and no GPU: a fixed sixtieth of a second a frame, for tests and CI */
    const char* game_module; /* a game module (a shared library) to load and hot-reload, or NULL */
    const char* physics;     /* "tynima" (also NULL) for the engine's own solver, "jolt" for the reference */
    const char*
        rhi; /* "auto" (also NULL): native Metal on Apple platforms, SDL GPU elsewhere; "sdl"; "metal" */
    uint32_t max_entities; /* 0: 65536 */
    uint32_t max_bodies;   /* 0: 4096 */
} tynima_engine_desc;

/* Brings up the platform, the window, the GPU, the world, the physics and
 * the game module. NULL on failure; tynima_last_error() says why. One at a
 * time, on the main thread. */
tynima_engine* tynima_engine_create(const tynima_engine_desc* desc);
void tynima_engine_destroy(tynima_engine* engine);
/* Text for the most recent failure here or in the platform layer. Never null. */
const char* tynima_last_error(void);

/* The same table a game module receives: a host reads and changes the world
 * through it too. */
const tynima_api* tynima_engine_api(tynima_engine* engine);

/* A frame. begin pumps the window's events and settles the frame's clock
 * and input; false once the window has been closed or the OS asked to quit
 * (the host then destroys the engine). Between the two the host draws its
 * UI and reads or changes the world. end runs the game module, steps the
 * simulation, renders and presents. */
bool tynima_engine_begin_frame(tynima_engine* engine);
void tynima_engine_end_frame(tynima_engine* engine);
/* This frame's length in seconds, the clock the game reads, and how many
 * frames have begun. */
float tynima_engine_dt(tynima_engine* engine);
double tynima_engine_time(tynima_engine* engine);
uint64_t tynima_engine_frame_index(tynima_engine* engine);

/* ---- models ---- */

/* Loads a model and returns the index a MeshRenderer's `model` refers to:
 * a cooked model (.tymodel, what tynima-cook writes) as it is, or a glTF
 * source (.glb, .gltf) cooked first into a .cooked directory beside it —
 * unless a blob newer than it is there already — and loaded from that.
 * TYNIMA_NO_MODEL on failure; the model is kept, at whatever index, for
 * the engine's lifetime. Its file is watched from then on: when it changes
 * (the blob, or the source, which is cooked again), the model is loaded
 * again into the same index and every entity drawing it shows the new one;
 * tynima_stats.model_reloads counts these. */
#define TYNIMA_NO_MODEL 0xFFFFFFFFu
uint32_t tynima_load_model(tynima_engine* engine, const char* path);
uint32_t tynima_model_count(tynima_engine* engine);
/* Cooks a glTF source into a .tymodel at `destination` (its directory made
 * if missing), as tynima-cook would: the mesh in GPU order, every image
 * with its mip chain. False with tynima_last_error(). */
bool tynima_cook_model(tynima_engine* engine, const char* source, const char* destination);
/* The box around a model's mesh, in the model's own space. False for an
 * index that is not a model, or one with nothing in it. */
bool tynima_model_bounds(tynima_engine* engine, uint32_t model, tynima_vec3* min, tynima_vec3* max);

/* ---- picking ---- */

/* The nearest drawable entity along a ray (origin and unit direction in
 * world space): the first whose model's bounds the ray enters, and how far
 * along the ray that is. False when it hits nothing. Bounds, not
 * triangles: enough for an editor's click, not for a bullet. */
bool tynima_pick(tynima_engine* engine, tynima_vec3 origin, tynima_vec3 direction, tynima_entity* out,
                 float* distance);

/* ---- scenes ----
 *
 * The world as a text file (TOML: an entity after another, a table per
 * component, a line per field — what version control diffs). Every
 * component in a file must be registered before it is loaded: the module
 * that defines one goes in first. false with tynima_last_error() naming
 * the line, and the world as it was. */
bool tynima_save_scene(tynima_engine* engine, const char* path);
/* `clear`: the world's entities go first; otherwise the file's are added. */
bool tynima_load_scene(tynima_engine* engine, const char* path, bool clear);
/* Every entity destroyed: an empty scene. */
void tynima_clear_scene(tynima_engine* engine);

/* ---- the view ---- */

/* The same camera and light a game sets through the table (tynima_camera,
 * tynima_lighting above), for the host that owns the window. */
void tynima_get_camera(tynima_engine* engine, tynima_camera* out);
void tynima_set_camera(tynima_engine* engine, const tynima_camera* camera);
void tynima_get_lighting(tynima_engine* engine, tynima_lighting* out);
void tynima_set_lighting(tynima_engine* engine, const tynima_lighting* lighting);

/* How the scene is drawn. The paths and views match the engine's
 * render::ShadingPath and render::DebugView; the names come from
 * tynima_render_name(). */
typedef enum tynima_shading_path {
    TYNIMA_SHADING_FORWARD,
    TYNIMA_SHADING_FUSED,
    TYNIMA_SHADING_SPLIT
} tynima_shading_path;
typedef enum tynima_shading_model {
    TYNIMA_MODEL_UNLIT,
    TYNIMA_MODEL_BLINN_PHONG,
    TYNIMA_MODEL_COOK_TORRANCE
} tynima_shading_model;
typedef enum tynima_tonemap { TYNIMA_TONEMAP_NONE, TYNIMA_TONEMAP_ACES, TYNIMA_TONEMAP_AGX } tynima_tonemap;
typedef enum tynima_anti_aliasing { TYNIMA_AA_NONE, TYNIMA_AA_FXAA, TYNIMA_AA_TAA } tynima_anti_aliasing;
#define TYNIMA_DEBUG_VIEW_COUNT 8u /* 0 is the lit picture; the rest show one quantity */
typedef struct tynima_render_settings {
    tynima_shading_path path;
    tynima_shading_model model;
    uint32_t debug_view;
    bool shadows;
    bool point_lights;
    bool bloom;
    tynima_tonemap tonemap;
    tynima_anti_aliasing anti_aliasing;
} tynima_render_settings;
void tynima_get_render_settings(tynima_engine* engine, tynima_render_settings* out);
void tynima_set_render_settings(tynima_engine* engine, const tynima_render_settings* settings);
/* The name of a debug view (0..TYNIMA_DEBUG_VIEW_COUNT-1); "?" past them. */
const char* tynima_debug_view_name(uint32_t view);

/* ---- the UI ----
 *
 * The engine owns a Dear ImGui context: it feeds it the window's input and
 * draws it over the frame. Between begin_frame and end_frame, a host calls
 * ImGui as usual (a module across a library boundary first hands ImGui the
 * context: ImGui::SetCurrentContext(tynima_ui_context(engine))). Without a
 * window the context exists and nothing is drawn. */
void* tynima_ui_context(tynima_engine* engine);

/* Asks for the scene to be drawn this frame into a texture of `width` by
 * `height` pixels instead of the window, and returns it as an ImTextureID
 * for ImGui::Image; the window is cleared behind the UI instead. Ask every
 * frame the picture is wanted. 0 without a GPU. */
uint64_t tynima_ui_scene_texture(tynima_engine* engine, uint32_t width, uint32_t height);
/* The scale from window points to pixels (2 on a Retina display). */
float tynima_ui_pixel_density(tynima_engine* engine);
/* Whether the UI wants the mouse (it is over a panel) or the keyboard (a
 * text field is active) this frame: then the scene behind should not act on them. */
bool tynima_ui_wants_mouse(tynima_engine* engine);
bool tynima_ui_wants_keyboard(tynima_engine* engine);
/* Hides the cursor and reports unbounded motion — for a fly camera. */
void tynima_set_relative_mouse(tynima_engine* engine, bool enabled);

/* ---- the log, as a console reads it ----
 *
 * The engine keeps the last TYNIMA_LOG_RING events of every level that got
 * through. `count` is how many were ever kept; the entries numbered
 * [count - kept, count) can still be read. Strings are copied out. */
#define TYNIMA_LOG_RING 2048u
#define TYNIMA_LOG_CATEGORY_MAX 24u
#define TYNIMA_LOG_MESSAGE_MAX 224u
typedef struct tynima_log_entry {
    uint64_t index;
    uint64_t time_ns; /* since the first log event of the process */
    tynima_log_level level;
    uint32_t thread; /* 0 is the main thread */
    char category[TYNIMA_LOG_CATEGORY_MAX];
    char message[TYNIMA_LOG_MESSAGE_MAX];
} tynima_log_entry;
uint64_t tynima_log_count(tynima_engine* engine);
bool tynima_log_get(tynima_engine* engine, uint64_t index, tynima_log_entry* out);
/* The engine's minimum level: what gets logged at all. */
tynima_log_level tynima_log_level_get(void);
void tynima_log_level_set(tynima_log_level level);

/* ---- statistics ---- */

typedef struct tynima_stats {
    float frame_ms;     /* the last frame, wall clock */
    float gpu_ms;       /* the GPU's time on the last completed frame; 0 where unknown */
    uint64_t gpu_bytes; /* what the device holds for the engine's resources; 0 where unknown */
    uint32_t entities;
    uint32_t archetypes;
    uint32_t bodies;
    uint32_t bodies_awake;
    uint32_t graph_passes; /* this frame's frame graph */
    uint32_t graph_culled;
    uint32_t graph_memoryless; /* transients that never left the tile */
    uint64_t graph_bytes;      /* what the graph's textures take in memory */
    uint64_t graph_bytes_memoryless;
    uint64_t heap_allocations; /* engine code's, in the last frame: the rule says zero */
    uint32_t game_reloads;     /* how many times the game module was swapped */
    uint32_t model_reloads;    /* how many times a model was loaded again after its file changed */
    uint32_t script_reloads;   /* how many times a Lua script was loaded again after its file changed */
} tynima_stats;
void tynima_get_stats(tynima_engine* engine, tynima_stats* out);

/* ---- the camera, inline: the same projection the engine draws with ----
 *
 * Normalized device coordinates run -1..1 across the view, x to the right
 * and y up; `aspect` is the view's width over its height. */

/* The direction, unit length, of the ray through (ndc_x, ndc_y). */
static inline tynima_vec3 tynima_camera_ray(const tynima_camera* camera, float aspect, float ndc_x,
                                            float ndc_y) {
    const float tan_half_y = tanf(0.5f * camera->fov_y);
    const tynima_vec3 view = tynima_vec3_make(ndc_x * tan_half_y * aspect, ndc_y * tan_half_y, -1.0f);
    return tynima_vec3_normalize(tynima_quat_rotate(camera->rotation, view));
}
/* Where a world point lands: (ndc_x, ndc_y) and its distance along the
 * view direction. False, with nothing written, for a point behind the
 * camera. */
static inline bool tynima_camera_project(const tynima_camera* camera, float aspect, tynima_vec3 point,
                                         float* ndc_x, float* ndc_y, float* depth) {
    const tynima_quat inverse = {-camera->rotation.x, -camera->rotation.y, -camera->rotation.z,
                                 camera->rotation.w};
    const tynima_vec3 view = tynima_quat_rotate(inverse, tynima_vec3_sub(point, camera->position));
    if (view.z >= -1e-6f) {
        return false;
    }
    const float tan_half_y = tanf(0.5f * camera->fov_y);
    if (ndc_x) *ndc_x = view.x / (-view.z * tan_half_y * aspect);
    if (ndc_y) *ndc_y = view.y / (-view.z * tan_half_y);
    if (depth) *depth = -view.z;
    return true;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TYNIMA_H */
