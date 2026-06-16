import { get, post, CurlClient } from "wl2:curl";

const configuredUrl = globalThis.WL2_CURL_TEST_URL || "";
const localFixture = configuredUrl.startsWith("http://127.0.0.1:");
const baseUrl = configuredUrl || "https://example.com/";
const textUrl = localFixture ? `${baseUrl}/text` : baseUrl;
const headersUrl = localFixture ? `${baseUrl}/headers` : baseUrl;
const redirectUrl = localFixture ? `${baseUrl}/redirect` : baseUrl;
const slowUrl = localFixture ? `${baseUrl}/slow` : "";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function bodyText(response) {
  if (response.body && typeof response.body.text === "function") {
    return response.body.text();
  }
  return String(response.body || "");
}

console.log("wl2_curl smoke test:", textUrl);

const getResponse = await get(textUrl, {
  timeoutMs: 5000,
  headers: {
    "User-Agent": "winglib2-wl2-curl-smoke/0.1"
  }
});

assert(getResponse.status >= 200 && getResponse.status < 400, `GET failed: ${getResponse.status}`);
assert(wl2.buffer.isBuffer(getResponse.body), "GET response body is not wl2.Buffer");
assert(bodyText(getResponse).length > 0, "GET returned an empty body");
if (localFixture) {
  assert(bodyText(getResponse).includes("winglib2 wl2_curl smoke response"), "GET body did not match fixture text");
}
console.log("GET", getResponse.status, getResponse.url || textUrl);

if (localFixture) {
  const headerResponse = await get(headersUrl, { timeoutMs: 5000 });
  assert(headerResponse.status === 200, `headers endpoint failed: ${headerResponse.status}`);
  assert(headerResponse.headers["x-wl2-fixture"] === "headers", "fixture response header missing");

  const redirectResponse = await get(redirectUrl, {
    timeoutMs: 5000,
    followRedirects: true
  });
  assert(redirectResponse.status === 200, `redirect endpoint failed: ${redirectResponse.status}`);
  assert(bodyText(redirectResponse).includes("winglib2 wl2_curl smoke response"), "redirect body did not match fixture text");
}

const client = new CurlClient({
  timeoutMs: 5000,
  followRedirects: true,
  headers: {
    "User-Agent": "winglib2-wl2-curl-smoke/0.1"
  }
});

const clientResponse = await client.request({
  method: "GET",
  url: textUrl
});

assert(clientResponse.status >= 200 && clientResponse.status < 400, `client GET failed: ${clientResponse.status}`);
assert(wl2.buffer.isBuffer(clientResponse.body), "client GET response body is not wl2.Buffer");
console.log("client GET", clientResponse.status);

if (globalThis.WL2_CURL_TEST_POST_URL) {
  const posted = await post(globalThis.WL2_CURL_TEST_POST_URL, JSON.stringify({ ok: true }), {
    timeoutMs: 5000,
    headers: {
      "Content-Type": "application/json",
      "User-Agent": "winglib2-wl2-curl-smoke/0.1"
    }
  });

  assert(posted.status >= 200 && posted.status < 400, `POST failed: ${posted.status}`);
  assert(wl2.buffer.isBuffer(posted.body), "POST response body is not wl2.Buffer");
  assert(bodyText(posted).includes("posted:"), "POST response did not echo payload");
  console.log("POST", posted.status);
}

if (slowUrl) {
  let timedOut = false;
  try {
    await get(slowUrl, { timeoutMs: 50 });
  } catch (error) {
    timedOut = true;
  }
  assert(timedOut, "slow endpoint did not trigger timeout");
}

console.log("wl2_curl smoke test passed");
