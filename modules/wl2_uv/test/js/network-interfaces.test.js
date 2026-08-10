import { localNetworks, networkInterfaces, version } from "wl2:uv";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(typeof version === "string" && version.length > 0, "version is missing");

let interfaces;
try {
  interfaces = networkInterfaces({ includeInternal: true });
} catch (error) {
  // Sandboxed Linux test runners may block the netlink socket used internally
  // by getifaddrs(). The module must still surface the stable Winglib error.
  assert(
    error.code === "uv_interface_enumeration_failed",
    `unexpected enumeration error: ${error.code}`,
  );
  assert(error.operation === "networkInterfaces", "missing error operation");
  console.log("wl2:uv enumeration is unavailable in this sandbox");
}

if (!interfaces) {
  console.log("wl2:uv network interface error contract passed");
} else {
  assert(Array.isArray(interfaces), "networkInterfaces must return an array");
  assert(interfaces.length > 0, "expected at least one local interface address");

  for (const item of interfaces) {
    assert(typeof item.name === "string" && item.name.length > 0, "invalid name");
    assert(item.family === "IPv4" || item.family === "IPv6", "invalid family");
    assert(typeof item.address === "string" && item.address.length > 0, "invalid address");
    assert(typeof item.netmask === "string" && item.netmask.length > 0, "invalid netmask");
    assert(typeof item.internal === "boolean", "invalid internal flag");
    assert(typeof item.mac === "string", "invalid MAC address");
    assert(
      item.prefixLength === null || Number.isInteger(item.prefixLength),
      "invalid prefix length",
    );
  }

  const ipv4 = networkInterfaces({ family: "IPv4", includeInternal: true });
  assert(ipv4.every((item) => item.family === "IPv4"), "IPv4 filter was ignored");

  const networks = localNetworks({ includeInternal: true, maximumHosts: 2 ** 32 });
  assert(Array.isArray(networks), "localNetworks must return an array");
  assert(
    networks.every((network) => /^\d+\.\d+\.\d+\.\d+\/\d+$/.test(network)),
    "localNetworks returned an invalid IPv4 CIDR",
  );
  assert(new Set(networks).size === networks.length, "localNetworks returned duplicates");

  console.log("wl2:uv network interface tests passed");
}
