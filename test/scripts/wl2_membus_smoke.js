import {
  SharedBuffer,
  SharedQueue,
  VideoBuffer,
  AudioBuffer,
  CommandChannel,
  KeyValueStore,
  Selector,
  hasV12Surface
} from "wl2:membus";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function uniqueName(suffix) {
  return `/wl2_js_membus_${Date.now()}_${Math.floor(Math.random() * 1000000)}_${suffix}`;
}

function textOf(buffer) {
  return buffer.text();
}

{
  const name = uniqueName("buffer");
  const writer = SharedBuffer.create(name, 64);
  try {
    assert(writer.isOpen(), "SharedBuffer writer is not open");
    assert(writer.write("hello") === 5, "SharedBuffer write length mismatch");
    const reader = SharedBuffer.attach(name, 64);
    try {
      assert(reader.existing(), "SharedBuffer reader should attach to existing memory");
      assert(textOf(reader.read(5)) === "hello", "SharedBuffer read mismatch");
    } finally {
      reader.close();
    }
  } finally {
    writer.close();
  }
}

{
  const name = uniqueName("queue");
  const writer = SharedQueue.create(name, 512, true);
  const reader = SharedQueue.attach(name, 512, false);
  try {
    writer.write("message");
    assert(reader.poll(), "SharedQueue poll should report pending message");
    const result = reader.read({ timeoutMs: 100 });
    assert(!result.overrun, "SharedQueue should not overrun");
    assert(textOf(result.payload) === "message", "SharedQueue payload mismatch");
    if (hasV12Surface) {
      assert(reader.sessionId() !== 0, "SharedQueue session id missing");
    }
  } finally {
    writer.close();
    reader.close();
  }
}

// Byte-payload writes must accept strings, ArrayBuffer, TypedArray views, and
// wl2.Buffer without mangling the bytes.
{
  const name = uniqueName("payloads");
  const writer = SharedQueue.create(name, 512, true);
  const reader = SharedQueue.attach(name, 512, false);

  function roundTrip(label, payload, expected) {
    writer.write(payload);
    const bytes = reader.read({ timeoutMs: 100 }).payload.uint8Array();
    assert(bytes.length === expected.length, `${label} length mismatch: ${bytes.length} != ${expected.length}`);
    for (let i = 0; i < expected.length; ++i) {
      assert(bytes[i] === expected[i], `${label} byte ${i} mismatch: ${bytes[i]} != ${expected[i]}`);
    }
  }

  try {
    // "XYZ" must arrive as bytes 88,89,90 rather than the string "88,89,90".
    const xyz = [88, 89, 90];
    roundTrip("Uint8Array", new Uint8Array(xyz), xyz);
    roundTrip("ArrayBuffer", new Uint8Array(xyz).buffer, xyz);
    roundTrip("wl2.Buffer", wl2.buffer.fromText("XYZ"), xyz);

    // A view with a non-zero byteOffset must honor the offset and length.
    const backing = new Uint8Array([1, 2, 3, 4, 5]);
    roundTrip("Uint8Array.subarray", backing.subarray(1, 4), [2, 3, 4]);

    // Binary (non-UTF8) bytes must survive a wl2.Buffer round trip intact.
    const binary = [0x00, 0xff, 0x80, 0x01];
    roundTrip("binary Uint8Array", new Uint8Array(binary), binary);
  } finally {
    writer.close();
    reader.close();
  }
}

{
  const name = uniqueName("video");
  const video = VideoBuffer.create(name, 8, 4, 30, 2);
  try {
    const meta = video.metadata();
    assert(meta.width === 8 && meta.height === 4 && meta.bitsPerPixel === 24, "VideoBuffer metadata mismatch");
    video.fill(0, 0x33);
    const frame = video.frame(0);
    assert(frame.width === 8 && frame.height === 4, "VideoBuffer frame metadata mismatch");
    assert(frame.data.uint8Array()[0] === 0x33, "VideoBuffer frame payload mismatch");
    video.next(1);
    assert(video.metadata().sequence >= meta.sequence, "VideoBuffer sequence did not advance");
  } finally {
    video.close();
  }
}

{
  const name = uniqueName("audio");
  const audio = AudioBuffer.create(name, 2, 16, 48000, 30, 2);
  try {
    const meta = audio.metadata();
    assert(meta.channels === 2 && meta.bitsPerSample === 16 && meta.sampleRate === 48000, "AudioBuffer metadata mismatch");
    audio.fill(0, 0x22);
    const view = audio.buffer(0);
    assert(view.channels === 2 && view.bitsPerSample === 16, "AudioBuffer view metadata mismatch");
    assert(view.data.uint8Array()[0] === 0x22, "AudioBuffer payload mismatch");
  } finally {
    audio.close();
  }
}

if (hasV12Surface) {
  const name = uniqueName("command");
  const reader = CommandChannel.create(name, 512, true);
  const writer = CommandChannel.attach(name, 512, false);
  try {
    writer.write("pan_left");
    assert(reader.poll(), "CommandChannel poll should report pending command");
    const command = reader.read({ timeoutMs: 100 });
    assert(textOf(command.payload) === "pan_left", "CommandChannel payload mismatch");
  } finally {
    writer.close();
    reader.close();
  }

  const kvName = uniqueName("kv");
  const kv = KeyValueStore.create(kvName, 3, 15, 31);
  try {
    kv.setName(0, "pan");
    kv.setName(1, "tilt");
    kv.setName(2, "zoom");
    kv.setValue("pan", "10");
    kv.setValue(1, "-5");
    kv.setValue("zoom", "1.5");
    assert(kv.value("pan") === "10", "KeyValueStore named value mismatch");
    assert(kv.value(1) === "-5", "KeyValueStore indexed value mismatch");
    assert(kv.all().zoom === "1.5", "KeyValueStore snapshot mismatch");
  } finally {
    kv.close();
  }
}

{
  let ready = false;
  assert(Selector.wait([() => ready], { timeoutMs: 0 }) === -1, "Selector should time out");
  ready = true;
  assert(Selector.wait([() => false, () => ready], { timeoutMs: 10 }) === 1, "Selector should return ready index");
}

console.log("membus smoke ok");
