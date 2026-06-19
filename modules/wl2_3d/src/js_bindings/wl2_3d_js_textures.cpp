// --- Content textures -------------------------------------------------------

bool bytes_from_js(JSContext* ctx, JSValueConst value, std::vector<uint8_t>& out) {
    size_t byteOffset = 0, byteLength = 0, bytesPerElement = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buffer)) {
        size_t size = 0;
        uint8_t* data = JS_GetArrayBuffer(ctx, &size, buffer);
        const bool ok = data && byteOffset + byteLength <= size;
        if (ok) {
            out.assign(data + byteOffset, data + byteOffset + byteLength);
        }
        JS_FreeValue(ctx, buffer);
        return ok;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t size = 0;
    uint8_t* data = JS_GetArrayBuffer(ctx, &size, value);
    if (data) {
        out.assign(data, data + size);
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
}

JSValue texture_object(JSContext* ctx, std::shared_ptr<TextureState> texture) {
    JSValue obj = JS_NewObjectClass(ctx, texture_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new TextureBox{std::move(texture)});
    return obj;
}

JSValue texture_metadata(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    TextureBox* box = get_texture_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt64(ctx, box->texture->width));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt64(ctx, box->texture->height));
    JS_SetPropertyStr(ctx, obj, "stride", JS_NewInt64(ctx, box->texture->stride));
    JS_SetPropertyStr(ctx, obj, "format", JS_NewString(ctx, "rgba8"));
    JS_SetPropertyStr(ctx, obj, "colorSpace", JS_NewString(ctx, "srgb"));
    JS_SetPropertyStr(ctx, obj, "alpha", JS_NewString(ctx, "premultiplied"));
    JS_SetPropertyStr(ctx, obj, "origin", JS_NewString(ctx, "top-left"));
    JS_SetPropertyStr(ctx, obj, "byteLength", JS_NewInt64(ctx, static_cast<int64_t>(box->texture->rgba.size())));
    return obj;
}

JSValue texture_map(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    TextureBox* box = get_texture_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    return JS_NewArrayBufferCopy(ctx, box->texture->rgba.data(), box->texture->rgba.size());
}

JSValue texture_unmap(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    TextureBox* box = get_texture_box(ctx, thisVal);
    if (!box) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "texture.unmap(bytes) requires an ArrayBuffer or typed array");
    }
    std::vector<uint8_t> bytes;
    if (!bytes_from_js(ctx, argv[0], bytes)) {
        return JS_ThrowTypeError(ctx, "texture.unmap(bytes) requires an ArrayBuffer or typed array");
    }
    if (bytes.size() != box->texture->rgba.size()) {
        return throw_error(ctx, make_error("3d_invalid_texture", "texture.unmap() byte length must match the mapped texture size"));
    }
    box->texture->rgba = std::move(bytes);
    return texture_metadata(ctx, thisVal, 0, nullptr);
}

JSValue scene_texture(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto scene = get_scene(ctx, thisVal);
    if (!scene) {
        return JS_EXCEPTION;
    }
    (void)scene;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "scene.texture({ size: [width, height], data? }) requires an options object");
    }
    int64_t width = 0;
    int64_t height = 0;
    JSValue size = JS_GetPropertyStr(ctx, argv[0], "size");
    if (JS_IsArray(ctx, size) > 0) {
        JSValue w = JS_GetPropertyUint32(ctx, size, 0);
        JSValue h = JS_GetPropertyUint32(ctx, size, 1);
        JS_ToInt64(ctx, &width, w);
        JS_ToInt64(ctx, &height, h);
        JS_FreeValue(ctx, w);
        JS_FreeValue(ctx, h);
    }
    JS_FreeValue(ctx, size);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return throw_error(ctx, make_error("3d_invalid_texture", "texture size must be positive and within bounds"));
    }
    const int64_t stride = width * 4;
    const size_t byteLength = static_cast<size_t>(stride * height);
    auto texture = std::make_shared<TextureState>();
    texture->width = width;
    texture->height = height;
    texture->stride = stride;
    texture->rgba.assign(byteLength, 0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            texture->rgba[static_cast<size_t>(y * stride + x * 4 + 3)] = 255;
        }
    }
    JSValue data = JS_GetPropertyStr(ctx, argv[0], "data");
    if (!JS_IsUndefined(data) && !JS_IsNull(data)) {
        std::vector<uint8_t> bytes;
        if (!bytes_from_js(ctx, data, bytes)) {
            JS_FreeValue(ctx, data);
            return JS_ThrowTypeError(ctx, "texture data must be an ArrayBuffer or typed array");
        }
        if (bytes.size() != byteLength) {
            JS_FreeValue(ctx, data);
            return throw_error(ctx, make_error("3d_invalid_texture", "texture data byte length must equal width * height * 4"));
        }
        texture->rgba = std::move(bytes);
    }
    JS_FreeValue(ctx, data);
    return texture_object(ctx, std::move(texture));
}

