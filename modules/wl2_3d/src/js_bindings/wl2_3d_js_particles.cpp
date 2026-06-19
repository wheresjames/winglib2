// --- Effects: particle emitters (§13.2) -------------------------------------

bool read_particle_color(JSContext* ctx, JSValueConst opts, const char* key, td::ParticleColor& out) {
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    bool ok = false;
    if (!JS_IsUndefined(v)) {
        td::Color c{out.r, out.g, out.b, out.a};
        if (read_color(ctx, v, c)) {
            out = {c.r, c.g, c.b, c.a};
            ok = true;
        }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

JSValue scene_particles(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    td::Emitter emitter;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValueConst opts = argv[0];
        JSValue at = JS_GetPropertyStr(ctx, opts, "at");
        if (!JS_IsUndefined(at)) read_vec3(ctx, at, emitter.position);
        JS_FreeValue(ctx, at);
        JSValue vel = JS_GetPropertyStr(ctx, opts, "velocity");
        if (!JS_IsUndefined(vel)) read_vec3(ctx, vel, emitter.velocity);
        JS_FreeValue(ctx, vel);
        JSValue jit = JS_GetPropertyStr(ctx, opts, "velocityJitter");
        if (!JS_IsUndefined(jit)) read_vec3(ctx, jit, emitter.velocityJitter);
        JS_FreeValue(ctx, jit);
        JSValue grav = JS_GetPropertyStr(ctx, opts, "gravity");
        if (!JS_IsUndefined(grav)) read_vec3(ctx, grav, emitter.gravity);
        JS_FreeValue(ctx, grav);
        read_number_prop(ctx, opts, "rate", emitter.rate);
        read_number_prop(ctx, opts, "lifetime", emitter.lifetime);
        read_number_prop(ctx, opts, "sizeStart", emitter.sizeStart);
        read_number_prop(ctx, opts, "sizeEnd", emitter.sizeEnd);
        read_particle_color(ctx, opts, "colorStart", emitter.colorStart);
        read_particle_color(ctx, opts, "colorEnd", emitter.colorEnd);
        double maxD = emitter.maxParticles;
        if (read_number_prop(ctx, opts, "max", maxD)) emitter.maxParticles = static_cast<int>(maxD);
        double seed = 0.0;
        if (read_number_prop(ctx, opts, "seed", seed)) emitter.rng = td::Rng(static_cast<uint64_t>(seed));
        JSValue blend = JS_GetPropertyStr(ctx, opts, "blend");
        if (JS_IsString(blend)) {
            emitter.blend = js_string(ctx, blend) == "alpha" ? td::BlendMode::Alpha : td::BlendMode::Additive;
        }
        JS_FreeValue(ctx, blend);
        JSValue emitting = JS_GetPropertyStr(ctx, opts, "emitting");
        if (JS_IsBool(emitting)) emitter.emitting = JS_ToBool(ctx, emitting) != 0;
        JS_FreeValue(ctx, emitting);
    }
    const int64_t handle = scene->engine.addEmitter(std::move(emitter));
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "handle", JS_NewInt64(ctx, handle));
    return obj;
}

JSValue scene_particle_count(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    return JS_NewInt64(ctx, static_cast<int64_t>(scene->engine.particleCount()));
}

// Derived state of an emitter for inspection/tests: live count, blend, and the
// interpolated color/size/position of the oldest particle.
JSValue scene_emitter_state(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "scene.emitterState(handle) requires a handle");
    }
    int64_t handle = 0;
    JS_ToInt64(ctx, &handle, argv[0]);
    const td::Emitter* emitter = scene->engine.emitter(handle);
    if (!emitter) {
        return JS_NULL;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "count", JS_NewInt64(ctx, static_cast<int64_t>(emitter->particles.size())));
    JS_SetPropertyStr(ctx, obj, "blend",
        JS_NewString(ctx, emitter->blend == td::BlendMode::Alpha ? "alpha" : "additive"));
    if (!emitter->particles.empty()) {
        const td::Particle& p = emitter->particles.front();
        const double t = emitter->lifeFraction(p);
        const td::ParticleColor c = emitter->colorAt(t);
        JSValue sample = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, sample, "life", JS_NewFloat64(ctx, t));
        JS_SetPropertyStr(ctx, sample, "size", JS_NewFloat64(ctx, emitter->sizeAt(t)));
        JS_SetPropertyStr(ctx, sample, "position", vec3_to_js(ctx, p.position));
        JSValue color = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, color, 0, JS_NewFloat64(ctx, c.r));
        JS_SetPropertyUint32(ctx, color, 1, JS_NewFloat64(ctx, c.g));
        JS_SetPropertyUint32(ctx, color, 2, JS_NewFloat64(ctx, c.b));
        JS_SetPropertyUint32(ctx, color, 3, JS_NewFloat64(ctx, c.a));
        JS_SetPropertyStr(ctx, sample, "color", color);
        JS_SetPropertyStr(ctx, obj, "sample", sample);
    }
    return obj;
}

