// wl2-webrtc-client.js -- browser client for the wl2:webrtc SignalingHub.
//
// A framework-agnostic helper that hides the WebRTC answerer dance: it opens the
// signaling WebSocket, authenticates with a token, receives the server's SDP
// offer, answers it, trickles ICE both ways, and surfaces the remote media track
// plus connection state through callbacks. It is the browser counterpart to the
// `SignalingHub` exported by wl2:webrtc.
//
// Served as a static asset (e.g. GET /wl2-webrtc-client.js) and loaded with
// <script type="module">. Self-contained, no dependencies.
//
//   import { Wl2WebrtcClient } from "/wl2-webrtc-client.js";
//   const client = new Wl2WebrtcClient({
//     url: `ws://${location.host}/signal`,
//     token: myTicket,                       // optional; matches SignalingHub.authenticate
//     onTrack: (stream) => { videoEl.srcObject = stream; },
//     onState: (s) => { /* {connection, ice, sentPackets, trackOpen, ...} */ },
//     onLog: (line) => console.log(line),
//   });
//   await client.start({ source: "camera" });  // request object is app-defined
//   // ... later:
//   client.stop();

export class Wl2WebrtcClient {
  constructor(options = {}) {
    this.url = options.url || `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/signal`;
    this.token = options.token != null ? options.token : null;
    this.iceServers = options.iceServers || null;         // overrides the server-provided list
    this.pumpIntervalMs = options.pumpIntervalMs || 25;
    this._onTrack = options.onTrack || function () {};
    this._onState = options.onState || function () {};
    this._onLog = options.onLog || function () {};
    this._onClose = options.onClose || function () {};

    this.ws = null;
    this.pc = null;
    this._pumpTimer = 0;
    this._request = null;
    this._starting = null;
    this._stopped = false;
  }

  _log(text) { try { this._onLog(String(text)); } catch (e) {} }

  _send(message) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(message));
  }

  // Open the session and begin receiving media. `request` is passed verbatim to
  // the server's SignalingHub.onSession handler (source selection, etc.). Resolves
  // once the offer has been answered; media arrives asynchronously via onTrack.
  start(request = {}) {
    if (this._starting) return this._starting;
    this._stopped = false;
    this._request = request;
    this._starting = new Promise((resolve, reject) => {
      let settled = false;
      const done = (err) => { if (settled) return; settled = true; err ? reject(err) : resolve(); };

      let ws;
      try { ws = new WebSocket(this.url); } catch (e) { done(e); return; }
      this.ws = ws;

      ws.onopen = () => {
        this._log("Signaling connected; authenticating.");
        this._send({ type: "hello", token: this.token });
      };
      ws.onerror = () => { this._log("Signaling socket error."); done(new Error("Signaling socket error")); };
      ws.onclose = () => {
        this._stopPump();
        this._log("Signaling closed.");
        try { this._onClose(); } catch (e) {}
        done(new Error("Signaling closed before the session started"));
      };
      ws.onmessage = (event) => {
        this._handle(event.data, done).catch((err) => {
          this._log("Client error: " + (err && err.message || err));
          done(err);
        });
      };
    });
    return this._starting;
  }

  async _handle(raw, done) {
    let message;
    try { message = JSON.parse(raw); } catch (e) { return; }

    if (message.type === "welcome") {
      const ice = this.iceServers || message.iceServers || [];
      this._createPeer(ice);
      this._send({ type: "start", request: this._request });
      this._startPump();
      done();                      // authenticated + offer requested
      return;
    }

    if (message.type === "offer") {
      if (!this.pc) this._createPeer(this.iceServers || []);
      await this.pc.setRemoteDescription(message.description);
      const answer = await this.pc.createAnswer();
      await this.pc.setLocalDescription(answer);
      this._send({ type: "answer", description: this.pc.localDescription });
      return;
    }

    if (message.type === "candidate" && message.candidate && this.pc) {
      try { await this.pc.addIceCandidate(message.candidate); } catch (e) { this._log("addIceCandidate failed: " + e.message); }
      return;
    }

    if (message.type === "state") {
      try { this._onState(message); } catch (e) {}
      return;
    }

    if (message.type === "status") {
      this._log("Server: " + (message.message || ""));
      return;
    }

    if (message.type === "error") {
      this._log("Server error: " + (message.message || "unknown"));
      done(new Error(message.message || "Server rejected the session"));
      this.stop();
    }
  }

  _createPeer(iceServers) {
    if (this.pc) return;
    const pc = new RTCPeerConnection({ iceServers: iceServers || [] });
    this.pc = pc;
    // The server offers a sendonly track; setRemoteDescription creates the
    // matching recvonly transceiver, so no addTransceiver is needed here.
    pc.ontrack = (event) => {
      const stream = event.streams && event.streams[0];
      if (stream) { try { this._onTrack(stream, event); } catch (e) {} this._log("Remote media track received."); }
    };
    pc.onicecandidate = (event) => {
      if (event.candidate && event.candidate.candidate) this._send({ type: "candidate", candidate: event.candidate.toJSON() });
    };
    pc.onconnectionstatechange = () => this._log("Peer connection=" + pc.connectionState);
    pc.oniceconnectionstatechange = () => this._log("Peer ICE=" + pc.iceConnectionState);
  }

  _startPump() {
    this._stopPump();
    // The server pumps media on demand (no server-side timer), so tick it.
    this._pumpTimer = setInterval(() => this._send({ type: "pump" }), this.pumpIntervalMs);
  }

  _stopPump() {
    if (this._pumpTimer) { clearInterval(this._pumpTimer); this._pumpTimer = 0; }
  }

  async stats() {
    if (!this.pc) return null;
    try { return await this.pc.getStats(); } catch (e) { return null; }
  }

  stop() {
    if (this._stopped) return;
    this._stopped = true;
    this._stopPump();
    this._send({ type: "stop" });
    if (this.ws) { try { this.ws.close(); } catch (e) {} }
    if (this.pc) { try { this.pc.close(); } catch (e) {} }
    this.ws = null;
    this.pc = null;
    this._starting = null;
  }
}

export default Wl2WebrtcClient;
