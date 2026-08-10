import { networkInterfaces } from "wl2:uv";

let error;
try {
  networkInterfaces();
} catch (caught) {
  error = caught;
}

if (!error || error.code !== "uv_permission_denied") {
  throw new Error(`expected uv_permission_denied, received ${error && error.code}`);
}

console.log("wl2:uv permission denial test passed");
