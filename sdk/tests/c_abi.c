/* tynima.h, compiled as plain C11 — the language a game may be written in,
 * and the one this header promises to be. It is included twice to prove the
 * guard holds, and nothing else is included before it: a header that needs
 * a companion file, or that leans on a C++ keyword, fails to build here.
 *
 * Every declaration is also used: the structs are made and read, the inline
 * math is called, the table is filled with functions of the right shape.
 * C's compiler checks the shapes; the cases in api_test.cpp check that the
 * answers are right. */
#include <tynima.h>
#include <tynima.h>

#include <string.h>

/* What the C side reports back to the tests. */
int tynima_c_abi_api_version(void) {
    return (int)TYNIMA_API_VERSION;
}

int tynima_c_abi_table_entries(void) {
    return (int)(sizeof(tynima_api) / sizeof(void*)) - 1;
}

/* The inline math, used as a game would: a camera looking down -z, a point
 * in front of it, the ray back out through the middle of the screen. */
int tynima_c_abi_math(void) {
    tynima_camera camera;
    tynima_vec3 forward, ahead, ray;
    tynima_quat turn;
    float ndc_x = 0.0f, ndc_y = 0.0f, depth = 0.0f;
    int ok = 1;

    camera.position = tynima_vec3_make(0.0f, 0.0f, 0.0f);
    camera.rotation = tynima_quat_identity();
    camera.fov_y = 1.0f;
    camera.near = 0.05f;

    forward = tynima_quat_rotate(camera.rotation, tynima_vec3_make(0.0f, 0.0f, -1.0f));
    ok = ok && forward.z < -0.99f;

    ahead = tynima_vec3_add(camera.position, tynima_vec3_scale(forward, 5.0f));
    ok = ok && tynima_camera_project(&camera, 1.5f, ahead, &ndc_x, &ndc_y, &depth);
    ok = ok && ndc_x > -0.01f && ndc_x < 0.01f && depth > 4.9f;

    ray = tynima_camera_ray(&camera, 1.5f, 0.0f, 0.0f);
    ok = ok && ray.z < -0.99f;
    ok = ok && tynima_vec3_length(ray) > 0.99f;

    /* A quaternion from Euler angles (yaw, then pitch, then roll) and back. */
    turn = tynima_quat_from_euler(0.6f, -0.2f, 0.0f);
    {
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        tynima_quat_to_euler(turn, &yaw, &pitch, &roll);
        ok = ok && yaw > 0.59f && yaw < 0.61f;
        ok = ok && pitch > -0.21f && pitch < -0.19f;
        ok = ok && roll > -0.01f && roll < 0.01f;
    }
    return ok;
}

/* The value types a game fills in, made the way C makes them. */
int tynima_c_abi_structs(void) {
    tynima_body_desc body = tynima_body_desc_make(tynima_shape_capsule(0.3f, 0.6f),
                                                  tynima_vec3_make(0.0f, 2.0f, 0.0f));
    tynima_shape box = tynima_shape_box(tynima_vec3_make(1.0f, 0.5f, 1.0f));
    tynima_lighting lighting;
    tynima_point_light light;
    tynima_transform transform;
    tynima_name name;
    int ok = 1;

    ok = ok && body.shape.type == TYNIMA_SHAPE_CAPSULE && body.motion == TYNIMA_MOTION_DYNAMIC;
    ok = ok && body.friction > 0.0f && body.start_active && body.user_data == 0;
    ok = ok && box.type == TYNIMA_SHAPE_BOX && box.half_extents.y == 0.5f;
    ok = ok && tynima_shape_sphere(2.0f).radius == 2.0f;

    lighting.sun_direction = tynima_vec3_normalize(tynima_vec3_make(0.0f, 1.0f, 0.5f));
    lighting.sun_intensity = 3.0f;
    lighting.sun_color = tynima_vec3_make(1.0f, 1.0f, 1.0f);
    lighting.ambient = 0.1f;
    lighting.sky = tynima_vec3_make(0.1f, 0.1f, 0.12f);
    ok = ok && lighting.sun_direction.y > 0.8f;

    light.position = tynima_vec3_make(1.0f, 2.0f, 3.0f);
    light.radius = 5.0f;
    light.color = tynima_vec3_make(1.0f, 0.5f, 0.2f);
    ok = ok && light.radius == 5.0f;

    transform.position = tynima_vec3_make(0.0f, 0.0f, 0.0f);
    transform.rotation = tynima_quat_identity();
    transform.scale = tynima_vec3_make(1.0f, 1.0f, 1.0f);
    ok = ok && transform.scale.x == 1.0f;

    memset(&name, 0, sizeof name);
    strcpy(name.text, "player");
    ok = ok && strcmp(name.text, "player") == 0;
    return ok;
}

/* A game module's entry point, as a game in C writes it: the engine's own
 * loader reads exactly this shape out of a shared library. */
static void c_abi_load(const tynima_api* api, tynima_engine* engine, bool reloaded) {
    (void)reloaded;
    if (api != NULL && api->log != NULL && engine != NULL) {
        api->log(engine, TYNIMA_LOG_DEBUG, "a game written in C loaded");
    }
}

static void c_abi_update(const tynima_api* api, tynima_engine* engine, float dt) {
    tynima_camera camera;
    if (api == NULL || engine == NULL || api->version < 9u) {
        return;
    }
    /* What every game does: read the camera, move it, put it back. */
    api->get_camera(engine, &camera);
    camera.position = tynima_vec3_add(camera.position, tynima_vec3_make(dt, 0.0f, 0.0f));
    api->set_camera(engine, &camera);
}

const tynima_game* tynima_c_abi_game(void) {
    static tynima_game game;
    game.api_version = TYNIMA_API_VERSION;
    game.load = c_abi_load;
    game.unload = NULL;
    game.update = c_abi_update;
    return &game;
}
