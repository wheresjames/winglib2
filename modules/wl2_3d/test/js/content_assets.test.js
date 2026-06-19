import { Scene } from "wl2:3d";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

const scene = await Scene.create({ size: [64, 32], backend: "auto" });

const asset = await scene.load("wl2:/3d-fixtures/tiny.gltf", {
  id: "site-root",
  at: [1, 2, 3],
  scale: 2,
});
assert(asset.id === "site-root", "load should return a node wrapper with the id");
const assetState = asset.get();
assert(assetState.kind === "asset", "loaded content should be an asset node");
assert(assetState.assetUrl === "wl2:/3d-fixtures/tiny.gltf", "asset URL should round-trip");
assert(assetState.model === assetState.assetUrl, "model should default to the asset URL");
assert(assetState.resourceSize > 0, "loaded resource should record its size");
assert(assetState.position[0] === 1 && assetState.scale[0] === 2, "load options should apply to the node");

let rejected = false;
try {
  await scene.load("file:///tmp/not-allowed.gltf");
} catch (error) {
  rejected = error.code === "3d_invalid_argument";
}
assert(rejected, "scene.load should reject non-wl2 URLs");

rejected = false;
try {
  await scene.load("wl2:/3d-fixtures/missing.gltf");
} catch (error) {
  rejected = error.code === "resource_not_found";
}
assert(rejected, "scene.load should reject missing resources");

rejected = false;
try {
  await scene.load("wl2:/3d-fixtures/tiny.txt");
} catch (error) {
  rejected = error.code === "3d_unsupported_asset";
}
assert(rejected, "scene.load should reject unsupported extensions before opening");

const primitive = scene.primitive("grid", {
  id: "floor-grid",
  size: 10,
  divisions: 5,
  at: [0, 0, 0],
  color: "#808080",
});
const primitiveState = primitive.get();
assert(primitive.id === "floor-grid", "primitive should return a node wrapper with the id");
assert(primitiveState.kind === "primitive", "primitive node kind");
assert(primitiveState.primitive === "grid", "primitive kind should round-trip");
assert(primitiveState.primitiveOptions.size === 10, "primitive size option");
assert(primitiveState.primitiveOptions.divisions === 5, "primitive divisions option");

let primitiveRejected = false;
try {
  scene.primitive("unsupported-shape");
} catch (error) {
  primitiveRejected = error.code === "3d_unsupported_primitive";
}
assert(primitiveRejected, "unsupported primitive kind should throw");

const texture = scene.texture({ size: [2, 2] });
let meta = texture.metadata();
assert(meta.width === 2 && meta.height === 2, "texture metadata size");
assert(meta.stride === 8, "texture stride should be width * 4");
assert(meta.format === "rgba8" && meta.alpha === "premultiplied", "texture pixel contract");

const mapped = new Uint8Array(texture.map());
assert(mapped.length === 16, "mapped texture byte length");
assert(mapped[3] === 255 && mapped[7] === 255 && mapped[11] === 255 && mapped[15] === 255,
  "default texture pixels should be opaque");
mapped[0] = 12;
mapped[1] = 34;
mapped[2] = 56;
meta = texture.unmap(mapped);
assert(meta.byteLength === 16, "unmap should return texture metadata");
const mappedAgain = new Uint8Array(texture.map());
assert(mappedAgain[0] === 12 && mappedAgain[1] === 34 && mappedAgain[2] === 56,
  "unmap should copy bytes back into the texture");

let textureRejected = false;
try {
  texture.unmap(new Uint8Array(4));
} catch (error) {
  textureRejected = error.code === "3d_invalid_texture";
}
assert(textureRejected, "unmap should reject wrong-sized buffers");

scene.close();
console.log("wl2:3d content assets ok");
