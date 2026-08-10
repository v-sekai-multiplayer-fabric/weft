/*
 * Middleham / The Gyre MUD -- three.js 3D view.
 *
 * Two renderers share one WebGL2 context and one depth buffer:
 *
 *   1. three.js draws the room (floor, walls, exit posts) the usual way.
 *   2. Raw WebGL2 then draws the SlugHorn markers with the real ported
 *      Slug fragment shader (Lengyel 2017), straight onto the same
 *      default framebuffer three.js just rendered into.
 *
 * Because both write the same depth buffer, the markers get correctly
 * occluded by three.js's own geometry -- the whole reason SlugHorn was
 * picked over a DOM/CSS text overlay, which cannot depth-test by
 * construction. renderer.resetState() hands the context back to three.js
 * after each raw pass.
 *
 * The Slug shader sources, the interleaved attribute-per-instance vertex
 * layout, and the WASM accessor calls below are reused near-verbatim from
 * the batch prototype in weftspun/billboard-labels
 * (prototype/slughorn-wasm-batch-poc/index.html), which is itself ported
 * from SlugHorn's own GLFW example -- deliberately not rewritten.
 *
 * HONEST SCOPE, stated plainly: this runs SlugHorn's real WASM build and
 * its real GPU pipeline, but it does NOT render real glyph text. The
 * shapes are hand-authored star outlines (mud/web/slughorn/binding.cpp).
 * Font glyph loading needs a FreeType binding that does not exist
 * upstream in SlugHorn or here. Do not read these markers as "SlugHorn
 * text rendering works".
 *
 * No accounts, no login, same as mud.js: this browser mints one session
 * id per setting in localStorage, and this page shares those exact keys
 * with the text UI, so a session started in one view continues in the
 * other.
 */
import * as THREE from "./vendor/three.module.min.js";
import SlugHornModule from "./slughorn.js";

// ---------------------------------------------------------------- session

const MODE_KEY = "mud_mode";
const TITLES = { middleham: "Middleham", the_gyre: "The Gyre" };

function getOrCreateSessionId(mode) {
  const key = "mud_session_id_" + mode;
  let id = localStorage.getItem(key);
  if (!id) {
    id = "web-" + Math.random().toString(36).slice(2) + Date.now().toString(36);
    localStorage.setItem(key, id);
  }
  return id;
}

const statusEl = document.getElementById("status");
const roomNameEl = document.getElementById("roomName");
const logEl = document.getElementById("log");
const form = document.getElementById("commandForm");
const input = document.getElementById("commandInput");
const modeSelect = document.getElementById("modeSelect");

let mode = localStorage.getItem(MODE_KEY) || "middleham";
let sessionId = getOrCreateSessionId(mode);
modeSelect.value = mode;

function appendTurn(text, meta, invalid) {
  const div = document.createElement("div");
  div.className = "turn";
  const body = document.createElement("div");
  body.textContent = text;
  if (invalid) body.className = "invalid";
  div.appendChild(body);
  if (meta) {
    const metaEl = document.createElement("div");
    metaEl.className = "meta";
    metaEl.textContent = meta;
    div.appendChild(metaEl);
  }
  logEl.appendChild(div);
  logEl.scrollTop = logEl.scrollHeight;
}

// Same client-side split src/mud/mud_http.c's on_mud_command expects,
// copied from mud.js rather than a second, subtly different parser.
function parseCommandLine(raw) {
  const tokens = raw.trim().split(/\s+/).filter(Boolean);
  if (tokens.length === 0) return { command: "look", args: [], message: "" };
  const command = tokens[0].toLowerCase();
  if (command === "talk" && tokens.length > 2) {
    return { command, args: [tokens[1]], message: tokens.slice(2).join(" ") };
  }
  return { command, args: tokens.slice(1), message: "" };
}

async function sendCommand(raw) {
  const parsed = parseCommandLine(raw);
  appendTurn("> " + raw);
  try {
    const resp = await fetch("/api/mud/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        session_id: sessionId,
        domain: mode,
        command: parsed.command,
        args: parsed.args,
        message: parsed.message,
      }),
    });
    const data = await resp.json();
    if (!resp.ok) {
      appendTurn(data.error || "HTTP " + resp.status, null, true);
      return;
    }
    const meta = "turn " + data.turn + "  " + data.pre_room + " -> " + data.post_room +
      (data.objective_complete ? "  [objective complete]" : "") +
      (data.finished ? "  [session finished]" : "");
    appendTurn(data.narration, meta, data.valid === false);
    if (data.post_room) enterRoom(data.post_room);
  } catch (err) {
    appendTurn("connection error: " + err, null, true);
  }
}

// ------------------------------------------------------------- three.js

const canvas = document.getElementById("scene");
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
const gl = renderer.getContext();
if (!(gl instanceof WebGL2RenderingContext)) {
  statusEl.textContent = "WebGL2 is not available in this browser.";
  throw new Error("no webgl2");
}
renderer.setClearColor(0x0b0d12, 1);

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(50, 1, 0.1, 200);

scene.add(new THREE.AmbientLight(0xffffff, 0.55));
const keyLight = new THREE.DirectionalLight(0xffffff, 1.1);
keyLight.position.set(4, 8, 5);
scene.add(keyLight);

const ROOM_SIZE = 12;
const ROOM_HEIGHT = 5;

// The room shell: a box seen from the inside (BackSide), so the camera
// sits within it rather than looking at an opaque cube.
const wallMaterial = new THREE.MeshStandardMaterial({
  color: 0x2a2f3a, roughness: 0.95, metalness: 0.0, side: THREE.BackSide,
});
scene.add(new THREE.Mesh(
  new THREE.BoxGeometry(ROOM_SIZE, ROOM_HEIGHT, ROOM_SIZE), wallMaterial));

const floor = new THREE.Mesh(
  new THREE.BoxGeometry(ROOM_SIZE, 0.12, ROOM_SIZE),
  new THREE.MeshStandardMaterial({ color: 0x1b1f28, roughness: 1.0 }));
floor.position.y = -ROOM_HEIGHT / 2;
scene.add(floor);

scene.add(new THREE.GridHelper(ROOM_SIZE, 12, 0x3a4150, 0x232833)
  .translateY(-ROOM_HEIGHT / 2 + 0.07));

// Four exit posts, one per cardinal direction, so the room reads as a
// place with ways out rather than a sealed box. The MUD's own JSON
// (src/mud/mud_http.c's cbor_result_to_json) returns narration/pre_room/
// post_room and no structured exit list, so these are fixed cardinal
// markers, not a claim about which exits the current room really has.
const exitMaterial = new THREE.MeshStandardMaterial({ color: 0x8fd6c8, roughness: 0.6 });
const half = ROOM_SIZE / 2 - 0.4;
for (const [x, z] of [[0, -half], [0, half], [-half, 0], [half, 0]]) {
  const post = new THREE.Mesh(new THREE.BoxGeometry(0.5, 2.2, 0.5), exitMaterial);
  post.position.set(x, -ROOM_HEIGHT / 2 + 1.1, z);
  scene.add(post);
}

// A real occluder: a slab standing between the camera's default position
// and part of the SlugHorn marker ring. It exists so the shared depth
// buffer is actually exercised -- orbit around and the markers pass
// behind it and get clipped by three.js geometry.
const occluder = new THREE.Mesh(
  new THREE.BoxGeometry(3.2, 2.6, 0.25),
  new THREE.MeshStandardMaterial({ color: 0x8d93a0, roughness: 0.7 }));
occluder.position.set(0, 0.2, 2.6);
scene.add(occluder);

// Room identity -> a deterministic wall hue, so walking between rooms is
// visible in 3D even though the API gives us only the room's name.
function hashString(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

let currentRoom = null;
function enterRoom(name) {
  if (name === currentRoom) return;
  currentRoom = name;
  roomNameEl.textContent = "room: " + name;
  const hue = (hashString(name) % 360) / 360;
  wallMaterial.color.setHSL(hue, 0.22, 0.20);
  floor.material.color.setHSL(hue, 0.25, 0.11);
}

// Orbit camera, hand-rolled so no extra three.js example module has to be
// vendored alongside the core build.
const view = { theta: 0.6, phi: 1.25, radius: 11, target: new THREE.Vector3(0, 0.4, 0), autoRotate: true };
let dragging = false, lastX = 0, lastY = 0;
canvas.addEventListener("pointerdown", (e) => {
  dragging = true; view.autoRotate = false;
  lastX = e.clientX; lastY = e.clientY;
  canvas.classList.add("dragging"); canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener("pointerup", () => { dragging = false; canvas.classList.remove("dragging"); });
canvas.addEventListener("pointermove", (e) => {
  if (!dragging) return;
  view.theta -= (e.clientX - lastX) * 0.006;
  view.phi = Math.min(Math.PI - 0.15, Math.max(0.2, view.phi - (e.clientY - lastY) * 0.006));
  lastX = e.clientX; lastY = e.clientY;
});
canvas.addEventListener("wheel", (e) => {
  e.preventDefault();
  view.radius = Math.min(22, Math.max(3, view.radius + e.deltaY * 0.01));
}, { passive: false });

function resize() {
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== w * devicePixelRatio || canvas.height !== h * devicePixelRatio) {
    renderer.setPixelRatio(devicePixelRatio);
    renderer.setSize(w, h, false);
    camera.aspect = w / Math.max(h, 1);
    camera.updateProjectionMatrix();
  }
}
window.addEventListener("resize", resize);

// ------------------------------------------- SlugHorn: shaders (reused)

// Batched label vertex shader: every attribute varies per shape, the
// attribute-per-instance layout SlugHorn's own GLFW example uses.
// u_right/u_up stay uniforms since all shapes share one camera per frame.
const labelVertSrc = `#version 300 es
layout(location=0) in vec2 a_uv;
layout(location=1) in vec3 a_center;
layout(location=2) in float a_worldSize;
layout(location=3) in vec4 a_bbox;       // bboxMin.xy, bboxMax.xy
layout(location=4) in vec4 a_bandXform;  // bandScaleX/Y, bandOffsetX/Y
layout(location=5) in vec4 a_shapeData;  // bandTexX/Y, bandMaxX/Y
layout(location=6) in vec3 a_color;

uniform mat4 u_mvp;
uniform vec3 u_right;
uniform vec3 u_up;

out vec2 v_emCoord;
flat out vec4 v_bandXform;
flat out vec4 v_shapeData;
flat out vec3 v_color;

void main(){
  v_emCoord = mix(a_bbox.xy, a_bbox.zw, a_uv);
  v_bandXform = a_bandXform;
  v_shapeData = a_shapeData;
  v_color = a_color;
  vec3 worldPos = a_center + (a_uv.x - 0.5) * a_worldSize * u_right + (a_uv.y - 0.5) * a_worldSize * u_up;
  gl_Position = u_mvp * vec4(worldPos, 1.0);
}
`;

// Fragment shader: the Slug core (Lengyel 2017), ported near-verbatim
// from SlugHorn's own GLFW example (example/slughorn-example-glfw.cpp,
// k_FragSrc) to GLSL ES 300. SLUG_INDIRECTION_SIZE must match
// slughorn::Atlas::INDIRECTION_SIZE (32); u_texWidthLog2 replaces the
// example's hardcoded TEX_WIDTH so any atlas width works.
const labelFragSrc = `#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp usampler2D;

in vec2 v_emCoord;
flat in vec4 v_bandXform;
flat in vec4 v_shapeData;
flat in vec3 v_color;

uniform sampler2D u_curveTexture;
uniform usampler2D u_bandTexture;
uniform int u_texWidthLog2;

out vec4 fragColor;

#define SLUG_INDIRECTION_SIZE 32

uint slug_CalcRootCode(float y1, float y2, float y3) {
  uint i1 = floatBitsToUint(y1) >> 31u;
  uint i2 = floatBitsToUint(y2) >> 30u;
  uint i3 = floatBitsToUint(y3) >> 29u;
  uint shift = (i2 & 2u) | (i1 & ~2u);
  shift = (i3 & 4u) | (shift & ~4u);
  return ((0x2E74u >> shift) & 0x0101u);
}

vec2 slug_SolveHorizPoly(vec4 p12, vec2 p3) {
  vec2 a = p12.xy - p12.zw * 2.0 + p3;
  vec2 b = p12.xy - p12.zw;
  float ra = 1.0 / a.y;
  float rb = 0.5 / b.y;
  float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
  float t1 = (b.y - d) * ra;
  float t2 = (b.y + d) * ra;
  if(abs(a.y) < 1.0 / 65536.0) { t1 = p12.y * rb; t2 = t1; }
  return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 slug_SolveVertPoly(vec4 p12, vec2 p3) {
  vec2 a = p12.xy - p12.zw * 2.0 + p3;
  vec2 b = p12.xy - p12.zw;
  float ra = 1.0 / a.x;
  float rb = 0.5 / b.x;
  float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
  float t1 = (b.x - d) * ra;
  float t2 = (b.x + d) * ra;
  if(abs(a.x) < 1.0 / 65536.0) { t1 = p12.x * rb; t2 = t1; }
  return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 slug_CalcBandLoc(ivec2 glyphLoc, uint offset) {
  ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
  bandLoc.y += bandLoc.x >> u_texWidthLog2;
  bandLoc.x &= (1 << u_texWidthLog2) - 1;
  return bandLoc;
}

float slug_CalcCoverage(float xcov, float ycov, float xwgt, float ywgt) {
  float coverage = max(
    abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
    min(abs(xcov), abs(ycov))
  );
  return clamp(coverage, 0.0, 1.0);
}

float slug_Render(vec2 renderCoord, vec4 bandTransform, ivec2 glyphLoc, ivec2 bandMax) {
  vec2 emsPerPixel = fwidth(renderCoord);
  vec2 pixelsPerEm = 1.0 / emsPerPixel;

  int qY = clamp(int(renderCoord.y * bandTransform.y + bandTransform.w), 0, SLUG_INDIRECTION_SIZE - 1);
  int qX = clamp(int(renderCoord.x * bandTransform.x + bandTransform.z), 0, SLUG_INDIRECTION_SIZE - 1);
  int bandY = int(texelFetch(u_bandTexture, ivec2(glyphLoc.x + qY, glyphLoc.y), 0).r);
  int bandX = int(texelFetch(u_bandTexture, ivec2(glyphLoc.x + SLUG_INDIRECTION_SIZE + qX, glyphLoc.y), 0).r);

  float xcov = 0.0, xwgt = 0.0;
  uvec2 hbandData = texelFetch(u_bandTexture, ivec2(glyphLoc.x + 2 * SLUG_INDIRECTION_SIZE + bandY, glyphLoc.y), 0).xy;
  ivec2 hbandLoc = slug_CalcBandLoc(glyphLoc, hbandData.y);
  for(int ci = 0; ci < int(hbandData.x); ci++) {
    ivec2 curveLoc = ivec2(texelFetch(u_bandTexture, ivec2(hbandLoc.x + ci, hbandLoc.y), 0).xy);
    vec4 p12 = texelFetch(u_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
    vec2 p3 = texelFetch(u_curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;
    if(max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;
    uint code = slug_CalcRootCode(p12.y, p12.w, p3.y);
    if(code != 0u) {
      vec2 r = slug_SolveHorizPoly(p12, p3) * pixelsPerEm.x;
      if((code & 1u) != 0u) { xcov += clamp(r.x + 0.5, 0.0, 1.0); xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0)); }
      if(code > 1u) { xcov -= clamp(r.y + 0.5, 0.0, 1.0); xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0)); }
    }
  }

  float ycov = 0.0, ywgt = 0.0;
  uvec2 vbandData = texelFetch(u_bandTexture, ivec2(glyphLoc.x + 2 * SLUG_INDIRECTION_SIZE + bandMax.y + 1 + bandX, glyphLoc.y), 0).xy;
  ivec2 vbandLoc = slug_CalcBandLoc(glyphLoc, vbandData.y);
  for(int ci = 0; ci < int(vbandData.x); ci++) {
    ivec2 curveLoc = ivec2(texelFetch(u_bandTexture, ivec2(vbandLoc.x + ci, vbandLoc.y), 0).xy);
    vec4 p12 = texelFetch(u_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
    vec2 p3 = texelFetch(u_curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;
    if(max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;
    uint code = slug_CalcRootCode(p12.x, p12.z, p3.x);
    if(code != 0u) {
      vec2 r = slug_SolveVertPoly(p12, p3) * pixelsPerEm.y;
      if((code & 1u) != 0u) { ycov -= clamp(r.x + 0.5, 0.0, 1.0); ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0)); }
      if(code > 1u) { ycov += clamp(r.y + 0.5, 0.0, 1.0); ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0)); }
    }
  }

  return slug_CalcCoverage(xcov, ycov, xwgt, ywgt);
}

void main() {
  ivec2 glyphLoc = ivec2(v_shapeData.xy);
  ivec2 bandMax = ivec2(v_shapeData.zw);
  float fill = slug_Render(v_emCoord, v_bandXform, glyphLoc, bandMax);
  if(fill < 0.001) discard;
  fragColor = vec4(v_color, fill);
}
`;

function compile(type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
  return s;
}
function link(vertSrc, fragSrc) {
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl.VERTEX_SHADER, vertSrc));
  gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fragSrc));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  return p;
}

// 21 floats/vertex: uv(2) center(3) worldSize(1) bbox(4) bandXform(4) shapeData(4) color(3)
const FLOATS_PER_VERTEX = 21;

// ------------------------------------------------------------ main loop

// Slug markers get their own tiny state block. Everything here is raw
// WebGL2 on three.js's context; renderer.resetState() below is what makes
// that safe to interleave.
let slug = null;

async function initSlugHorn() {
  statusEl.textContent = "building SlugHorn atlas (wasm)...";
  const Module = await SlugHornModule();
  Module._slughorn_buildBatchAtlas();

  const shapeCount = Module._slughorn_shapeCount();
  const curveW = Module._slughorn_curveTexWidth(), curveH = Module._slughorn_curveTexHeight();
  const bandW = Module._slughorn_bandTexWidth(), bandH = Module._slughorn_bandTexHeight();
  const curvePtr = Module._slughorn_curveTexPtr(), curveLen = Module._slughorn_curveTexLen();
  const bandPtr = Module._slughorn_bandTexPtr(), bandLen = Module._slughorn_bandTexLen();
  const curveData = new Float32Array(Module.HEAPU8.buffer, curvePtr, curveLen / 4);
  const bandData = new Uint16Array(Module.HEAPU8.buffer, bandPtr, bandLen / 2);
  const texWidthLog2 = Module._slughorn_atlasTexWidthLog2();

  // One interleaved buffer, every shape, one draw call.
  const verts = new Float32Array(shapeCount * 4 * FLOATS_PER_VERTEX);
  const indices = new Uint16Array(shapeCount * 6);

  function hueToRGB(h) {
    const c = 0.65, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = 0.35;
    let r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; } else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    return [r + m, g + m, b + m];
  }

  // Markers ring the room's centre, high enough to read against the walls
  // and low enough that the occluder slab really crosses in front of some
  // of them as the camera orbits.
  const ringRadius = 3.4;
  for (let i = 0; i < shapeCount; i++) {
    const f = (idx) => Module._slughorn_shapeField(i, idx);
    const bandTexX = f(0), bandTexY = f(1), bandMaxX = f(2), bandMaxY = f(3);
    const bandScaleX = f(4), bandScaleY = f(5), bandOffsetX = f(6), bandOffsetY = f(7);
    const bearingX = f(8), bearingY = f(9), width = f(10), height = f(11);

    const ang = (i / shapeCount) * Math.PI * 2;
    const center = [Math.cos(ang) * ringRadius, 0.9, Math.sin(ang) * ringRadius];
    const bbox = [bearingX, bearingY - height, bearingX + width, bearingY];
    const bandXform = [bandScaleX, bandScaleY, bandOffsetX, bandOffsetY];
    const shapeData = [bandTexX, bandTexY, bandMaxX, bandMaxY];
    const color = hueToRGB((i * 360) / shapeCount);

    const uvs = [[0, 0], [1, 0], [1, 1], [0, 1]];
    const base = i * 4;
    for (let v = 0; v < 4; v++) {
      const o = (base + v) * FLOATS_PER_VERTEX;
      verts.set(uvs[v], o);
      verts.set(center, o + 2);
      verts[o + 5] = 1.2; // worldSize
      verts.set(bbox, o + 6);
      verts.set(bandXform, o + 10);
      verts.set(shapeData, o + 14);
      verts.set(color, o + 18);
    }
    indices.set([base + 0, base + 1, base + 2, base + 0, base + 2, base + 3], i * 6);
  }

  const mkTex = (internal, w, h, fmt, type, data) => {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, internal, w, h, 0, fmt, type, data);
    return t;
  };
  const curveTex = mkTex(gl.RGBA32F, curveW, curveH, gl.RGBA, gl.FLOAT, curveData);
  const bandTex = mkTex(gl.RGBA16UI, bandW, bandH, gl.RGBA_INTEGER, gl.UNSIGNED_SHORT, bandData);

  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);
  const vbo = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);
  const stride = FLOATS_PER_VERTEX * 4;
  const attr = (loc, size, offsetFloats) => {
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, size, gl.FLOAT, false, stride, offsetFloats * 4);
  };
  attr(0, 2, 0); attr(1, 3, 2); attr(2, 1, 5); attr(3, 4, 6);
  attr(4, 4, 10); attr(5, 4, 14); attr(6, 3, 18);
  const ebo = gl.createBuffer();
  gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ebo);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);
  gl.bindVertexArray(null);

  const prog = link(labelVertSrc, labelFragSrc);
  slug = {
    prog, vao, curveTex, bandTex, texWidthLog2, shapeCount,
    indexCount: indices.length,
    u: {
      mvp: gl.getUniformLocation(prog, "u_mvp"),
      right: gl.getUniformLocation(prog, "u_right"),
      up: gl.getUniformLocation(prog, "u_up"),
      curveTexture: gl.getUniformLocation(prog, "u_curveTexture"),
      bandTexture: gl.getUniformLocation(prog, "u_bandTexture"),
      texWidthLog2: gl.getUniformLocation(prog, "u_texWidthLog2"),
    },
  };

  statusEl.textContent =
    `SlugHorn WASM live: ${shapeCount} shapes, one draw call, ` +
    `curveTex ${curveW}x${curveH}, bandTex ${bandW}x${bandH}. ` +
    `Real glyphs not yet wired.`;
  // A stable hook for the e2e test to assert the real pipeline came up,
  // rather than the test scraping HUD prose that may be reworded later.
  window.__slughorn = { shapeCount, curveW, curveH, bandW, bandH, texWidthLog2 };
}

const vpMatrix = new THREE.Matrix4();

function drawSlugHorn() {
  if (!slug) return;

  // Same view-projection three.js just used, so the markers land in the
  // same world space as the room, not an independently-guessed camera.
  vpMatrix.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
  // camera.matrixWorld's first two basis columns are the camera's own
  // world-space right and up -- what billboards the marker quads.
  const m = camera.matrixWorld.elements;

  gl.useProgram(slug.prog);
  gl.enable(gl.DEPTH_TEST);
  gl.depthFunc(gl.LEQUAL);
  gl.depthMask(true);
  gl.disable(gl.BLEND);
  gl.disable(gl.CULL_FACE);

  gl.uniformMatrix4fv(slug.u.mvp, false, vpMatrix.elements);
  gl.uniform3f(slug.u.right, m[0], m[1], m[2]);
  gl.uniform3f(slug.u.up, m[4], m[5], m[6]);
  gl.uniform1i(slug.u.texWidthLog2, slug.texWidthLog2);

  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, slug.curveTex);
  gl.uniform1i(slug.u.curveTexture, 0);
  gl.activeTexture(gl.TEXTURE1);
  gl.bindTexture(gl.TEXTURE_2D, slug.bandTex);
  gl.uniform1i(slug.u.bandTexture, 1);

  gl.bindVertexArray(slug.vao);
  gl.drawElements(gl.TRIANGLES, slug.indexCount, gl.UNSIGNED_SHORT, 0);
  gl.bindVertexArray(null);

  // Test hook: sample the framebuffer *inside* the frame that just drew.
  // A WebGL canvas without preserveDrawingBuffer is cleared once the
  // browser composites it, so drawImage()-ing it from a test afterwards
  // reliably reads back nothing -- readPixels here is the only honest way
  // for an e2e test to assert the Slug shader actually put pixels down.
  if (window.__slughornWantCapture) {
    window.__slughornWantCapture = false;
    const w = gl.drawingBufferWidth, h = gl.drawingBufferHeight;
    const buf = new Uint8Array(w * h * 4);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, buf);
    let saturated = 0;
    for (let i = 0; i < buf.length; i += 4) {
      const r = buf[i], g = buf[i + 1], b = buf[i + 2];
      // Markers are strongly hued (hueToRGB); the room is near-grey, so a
      // wide per-pixel channel spread means a marker, not a wall.
      if (Math.max(r, g, b) - Math.min(r, g, b) > 60) saturated++;
    }
    window.__slughornPixels = { width: w, height: h, saturated };
  }

  // Hand the context back: three.js caches GL state aggressively, and the
  // raw calls above changed program/VAO/texture bindings behind its back.
  renderer.resetState();
}

function frame() {
  requestAnimationFrame(frame);
  resize();
  if (view.autoRotate) view.theta += 0.0022;

  camera.position.set(
    view.target.x + view.radius * Math.sin(view.phi) * Math.sin(view.theta),
    view.target.y + view.radius * Math.cos(view.phi),
    view.target.z + view.radius * Math.sin(view.phi) * Math.cos(view.theta));
  camera.lookAt(view.target);
  camera.updateMatrixWorld();

  renderer.render(scene, camera);
  drawSlugHorn();
}

// ------------------------------------------------------------- wiring up

form.addEventListener("submit", (ev) => {
  ev.preventDefault();
  const raw = input.value;
  input.value = "";
  if (raw.trim()) sendCommand(raw);
});

modeSelect.addEventListener("change", () => {
  mode = modeSelect.value;
  localStorage.setItem(MODE_KEY, mode);
  sessionId = getOrCreateSessionId(mode);
  document.title = (TITLES[mode] || mode) + " -- 3D view";
  logEl.innerHTML = "";
  currentRoom = null;
  appendTurn("Connected. Type a command below and press Send.", "session " + sessionId);
  sendCommand("look");
});

resize();
requestAnimationFrame(frame);

initSlugHorn().catch((e) => {
  statusEl.textContent = "SlugHorn error: " + e.message;
  console.error(e);
});

appendTurn("Connected. Type a command below and press Send.", "session " + sessionId);
// An opening "look" so the 3D room reflects real server state on load,
// rather than sitting on a default until the player types something.
sendCommand("look");
