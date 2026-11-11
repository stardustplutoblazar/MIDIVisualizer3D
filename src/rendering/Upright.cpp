#include "Upright.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

// Tuning of the per-note simulation (the reference is mechanism/anim/upright_ref.py, step_dyn).
// Every contact is a felt: a spring and a damper, given as natural frequency (rad/s) and damping ratio.
static const float kTau = 6.2831853f;
static const float kWippenGravity = 90.0f; ///< rad/s^2, wippen falling back onto the capstan.
static const float kWippenBounce = 0.3f; ///< Share of the speed the wippen keeps when it lands on the capstan.
static const float kHammerReturn = 30.0f; ///< rad/s^2, butt spring and gravity.
static const glm::vec2 kJackFelt(kTau * 60.0f, 0.3f);
static const glm::vec2 kStringFelt(kTau * 260.0f, 0.1f);
static const glm::vec2 kCushionFelt(kTau * 30.0f, 0.55f);
static const glm::vec2 kBackcheckFelt(kTau * 25.0f, 1.2f);
static const glm::vec2 kJackSpring(kTau * 18.0f, 0.35f);
static const float kKeyGlow = 0.9f; ///< Light inside a key pressed to the bottom.
// Spark running up a string when the hammer hits it.
static const float kSparkDuration = 0.4f; ///< Seconds to reach the top of the string.
static const float kSparkStart = 12.1f; ///< Height where the hammer hits the string.
static const float kSparkTail = 1.5f; ///< Length of the lit trail behind it, in white key widths.
static const float kSparkRadius = 0.14f;
static const float kSparkDim = 0.75f; ///< Kept discreet: it must not draw the eye from the notes.
static const int kMaxInstances = 128;

namespace {

bool isMinorNote(int n){
	const int pc = n % 12;
	return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

/// Little-endian reader over the whole file.
struct Reader {
	const std::vector<char> & data;
	size_t offset = 0;
	bool ok = true;
	explicit Reader(const std::vector<char> & d) : data(d) {}
	template<typename T> T get(){
		T v{};
		if(offset + sizeof(T) > data.size()){ ok = false; return v; }
		std::memcpy(&v, data.data() + offset, sizeof(T));
		offset += sizeof(T);
		return v;
	}
	glm::vec2 vec2(){ const float a = get<float>(); const float b = get<float>(); return {a, b}; }
	std::vector<float> floats(size_t count){
		std::vector<float> v(count, 0.0f);
		if(offset + count * sizeof(float) > data.size()){ ok = false; return v; }
		std::memcpy(v.data(), data.data() + offset, count * sizeof(float));
		offset += count * sizeof(float);
		return v;
	}
};

}

void Upright::setup(){
	_program.init("upright_vert", "upright_frag");

	glGenTextures(1, &_shadowTexture);
	glBindTexture(GL_TEXTURE_2D, _shadowTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, _shadowSize, _shadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &_shadowFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, _shadowFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _shadowTexture, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	// Unit sphere for the heads of the sparks (position = normal).
	{
		std::vector<float> v;
		const int n = 8;
		auto push = [&v](float th, float ph){
			const glm::vec3 p(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
			v.insert(v.end(), {p.x, p.y, p.z, p.x, p.y, p.z});
		};
		for(int i = 0; i < n; ++i){
			for(int j = 0; j < 2 * n; ++j){
				const float t0 = 3.14159265f * float(i) / n, t1 = 3.14159265f * float(i + 1) / n;
				const float p0 = 3.14159265f * float(j) / n, p1 = 3.14159265f * float(j + 1) / n;
				push(t0, p0); push(t1, p0); push(t1, p1);
				push(t0, p0); push(t1, p1); push(t0, p1);
			}
		}
		_sparkHead.count = GLsizei(v.size() / 6);
		glGenVertexArrays(1, &_sparkHead.vao);
		glBindVertexArray(_sparkHead.vao);
		glGenBuffers(1, &_sparkHead.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, _sparkHead.vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * v.size(), v.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
		glBindVertexArray(0);
	}
	checkGLError();
	_ready = true;
}

bool Upright::load(const std::string & path){
	// Free the previous model.
	for(Mesh & mesh : _meshes){
		glDeleteBuffers(1, &mesh.vbo);
		glDeleteVertexArrays(1, &mesh.vao);
	}
	_meshes.clear();
	_materials.clear();
	_error.clear();
	_notes = {};
	if(path.empty()){
		return true;
	}
	std::ifstream file(path, std::ios::binary);
	if(!file.is_open()){
		_error = "Can't open " + path;
		return false;
	}
	std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if(data.size() < 8 || std::memcmp(data.data(), "UPRT", 4) != 0){
		_error = "Not an upright piano file (.piano)";
		return false;
	}
	Reader r(data);
	r.offset = 4;
	const uint32_t version = r.get<uint32_t>();
	if(version != 1){
		_error = "Unsupported .piano version " + std::to_string(version);
		return false;
	}
	_phiMax = r.get<float>();
	_chiString = r.get<float>();
	_chiSetoff = r.get<float>();
	_keyPivot = r.vec2();
	_wippenPivot = r.vec2();
	_jackPivot = r.vec2();
	_hammerPivot = r.vec2();
	_springBase = r.vec2();
	_springTop = r.vec2();
	_stringZ = r.get<float>();
	_stringBottom = r.get<float>();
	_stringTop = r.get<float>();
	_visibleEnd = r.get<float>();
	_railMargin = r.get<float>();
	for(float & t : _tailOffset){
		t = r.get<float>();
	}
	const uint32_t phiCount = r.get<uint32_t>();
	_psi = r.floats(phiCount);
	_push = r.floats(phiCount);
	_catch = r.floats(phiCount);
	_escapeRows = int(r.get<uint32_t>());
	_escapeCols = int(r.get<uint32_t>());
	_escape = r.floats(size_t(_escapeRows) * size_t(_escapeCols));
	const uint32_t materialCount = r.get<uint32_t>();
	for(uint32_t i = 0; i < materialCount && r.ok; ++i){
		Material m;
		m.color.r = r.get<float>();
		m.color.g = r.get<float>();
		m.color.b = r.get<float>();
		m.specular = r.get<float>();
		m.gloss = r.get<float>();
		_materials.push_back(m);
	}
	const uint32_t meshCount = r.get<uint32_t>();
	for(uint32_t i = 0; i < meshCount && r.ok; ++i){
		Mesh mesh;
		mesh.group = int(r.get<uint32_t>());
		mesh.front = r.get<uint32_t>() != 0;
		mesh.mask = r.get<uint32_t>();
		mesh.material = int(r.get<uint32_t>());
		const uint32_t vertexCount = r.get<uint32_t>();
		const std::vector<float> vertices = r.floats(size_t(vertexCount) * 6);
		if(!r.ok || mesh.material >= int(_materials.size())){
			break;
		}
		mesh.count = GLsizei(vertexCount);
		glGenVertexArrays(1, &mesh.vao);
		glBindVertexArray(mesh.vao);
		glGenBuffers(1, &mesh.vbo);
		glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
		glBindVertexArray(0);
		_meshes.push_back(mesh);
	}
	if(!r.ok || phiCount < 2 || _escapeRows < 2 || _escapeCols < 2 || _meshes.size() != meshCount){
		_error = "Truncated or damaged .piano file";
		load("");
		_error = "Truncated or damaged .piano file";
		return false;
	}
	checkGLError();
	return true;
}

float Upright::table(const std::vector<float> & t, float phi) const {
	const float x = glm::clamp(phi / _phiMax, 0.0f, 1.0f) * float(t.size() - 1);
	const int i = std::min(int(x), int(t.size()) - 2);
	const float f = x - float(i);
	return t[i] * (1.0f - f) + t[i + 1] * f;
}

float Upright::escape(float phi, float chi) const {
	const float x = glm::clamp(phi / _phiMax, 0.0f, 1.0f) * float(_escapeRows - 1);
	const int i = std::min(int(x), _escapeRows - 2);
	const float fx = x - float(i);
	const float y = glm::clamp((chi - _chiString) / (0.0f - _chiString), 0.0f, 1.0f) * float(_escapeCols - 1);
	const int j = std::min(int(y), _escapeCols - 2);
	const float fy = y - float(j);
	auto e = [this](int a, int b){ return _escape[a * _escapeCols + b]; };
	const float lo = e(i, j) * (1.0f - fx) + e(i + 1, j) * fx;
	const float hi = e(i, j + 1) * (1.0f - fx) + e(i + 1, j + 1) * fx;
	return lo * (1.0f - fy) + hi * fy;
}

float Upright::phiOfPsi(float psi) const {
	// Key angle that puts the wippen at psi: the tables are indexed by the key angle.
	const int n = int(_psi.size());
	if(psi >= _psi[0]){
		return 0.0f;
	}
	for(int i = 0; i < n - 1; ++i){
		const float a = _psi[i];
		const float b = _psi[i + 1];
		if((a - psi) * (b - psi) <= 0.0f && a != b){
			return (float(i) + (psi - a) / (b - a)) / float(n - 1) * _phiMax;
		}
	}
	return _phiMax;
}

/// Felt contact at 'rest': pushes towards side (+1 or -1) when the part goes past it, as an angular acceleration.
static float felt(float x, float v, float rest, const glm::vec2 & wz, float side){
	const float pen = (rest - x) * side;
	if(pen <= 0.0f){
		return 0.0f;
	}
	const float w = wz.x;
	const float damping = (v * side < 0.0f) ? 1.0f : 0.3f; // Softer while the felt pushes back.
	return side * (w * w * pen) - 2.0f * wz.y * w * v * damping;
}

void Upright::step(NoteState & s, float travel, float h) const {
	const float phiKey = travel * _phiMax;
	const float target = table(_psi, phiKey);
	// Wippen: carried by the capstan while it pushes; when the key drops faster than the wippen falls,
	// it falls under its weight, lands on the capstan felt and bounces a little.
	const float targetVel = s.hasPrevTarget ? (target - s.prevTarget) / h : 0.0f;
	if(s.psi >= target - 1e-7f){
		s.psiVel = targetVel;
		s.psi = target;
	} else {
		s.psiVel += kWippenGravity * h;
		s.psi += s.psiVel * h;
		if(s.psi >= target){
			const float landing = s.psiVel - targetVel;
			s.psi = target;
			s.psiVel = targetVel - landing * kWippenBounce;
			if(s.psiVel < targetVel - 1e-6f){
				s.psi = target + s.psiVel * h;
			}
		}
	}
	s.prevTarget = target;
	s.hasPrevTarget = true;
	if(s.psi > 0.0f){
		s.psi = 0.0f;
		s.psiVel = std::min(s.psiVel, 0.0f);
	}
	const float phi = s.psi < -1e-6f ? std::max(0.0f, phiOfPsi(s.psi)) : 0.0f;
	const float push = table(_push, phi);
	const float catchAngle = table(_catch, phi);
	// Set-off: the jack lets the hammer go.
	if(!s.escaped && push <= _chiSetoff + 1e-4f && travel > 0.3f){
		s.escaped = true;
	}
	// Hammer: thrown by the jack, bounces on the string, caught by the backcheck, back on the rest rail.
	float acc = kHammerReturn;
	if(!s.escaped){
		acc += felt(s.chi, s.omega, push, kJackFelt, -1.0f);
	}
	acc += felt(s.chi, s.omega, _chiString, kStringFelt, 1.0f);
	acc += felt(s.chi, s.omega, 0.0f, kCushionFelt, -1.0f);
	if(s.escaped && catchAngle < 0.0f){
		if(s.chi < catchAngle - 0.01f){
			s.armed = true;
		}
		if(s.armed){
			acc += felt(s.chi, s.omega, catchAngle, kBackcheckFelt, -1.0f);
		}
	}
	const float before = s.chi;
	s.omega += acc * h;
	s.chi += s.omega * h;
	if(before > _chiString && s.chi <= _chiString){
		s.sparkAge = 0.0f; // Level set by update(), which knows the velocity.
	}
	// Jack: its spring brings it back under the butt, never into it.
	const float need = s.escaped ? escape(phi, s.chi) : 0.0f;
	s.zetaVel += (kJackSpring.x * kJackSpring.x * (need - s.zeta) - 2.0f * kJackSpring.y * kJackSpring.x * s.zetaVel) * h;
	s.zeta += s.zetaVel * h;
	if(s.zeta < need){
		s.zeta = need;
		s.zetaVel = std::max(s.zetaVel, 0.0f);
	}
	if(s.zeta < 0.0f){
		s.zeta = 0.0f;
		s.zetaVel = std::max(s.zetaVel, 0.0f);
	}
	if(s.escaped && need <= 1e-4f && s.chi >= push - 0.02f && travel < 0.6f){
		s.escaped = false;
		s.armed = false;
	}
	s.phi = phiKey;
}

void Upright::update(const std::array<float, 128> & travel, const std::array<float, 128> & velocities, float dt, bool snap){
	if(!loaded()){
		return;
	}
	if(snap){
		// Paused or seeking: the steady state of each key, pressed (hammer caught) or at rest.
		for(size_t k = 0; k < _notes.size(); ++k){
			NoteState & s = _notes[k];
			s = NoteState();
			const float phi = glm::clamp(travel[k], 0.0f, 1.0f) * _phiMax;
			s.phi = phi;
			s.psi = table(_psi, phi);
			s.prevTarget = s.psi;
			s.hasPrevTarget = true;
			const float push = table(_push, phi);
			if(travel[k] > 0.5f){
				const float c = table(_catch, phi);
				s.escaped = true;
				s.armed = c < 0.0f;
				s.chi = s.armed ? c : 0.0f;
				s.zeta = escape(phi, s.chi);
			} else {
				s.chi = std::min(0.0f, push);
			}
		}
		return;
	}
	const int substeps = std::max(1, int(std::ceil(dt / 0.001f)));
	const float h = dt / float(substeps);
	for(size_t k = 0; k < _notes.size(); ++k){
		const float t = glm::clamp(travel[k], -0.2f, 1.2f);
		// Keys at rest with nothing moving: skip the substeps.
		NoteState & s = _notes[k];
		if(std::abs(t) < 1e-4f && std::abs(s.chi) < 1e-5f && std::abs(s.omega) < 1e-3f && s.zeta < 1e-5f && std::abs(s.psi) < 1e-6f && !s.escaped && s.sparkAge < 0.0f){
			s = NoteState();
			s.hasPrevTarget = true;
			continue;
		}
		const bool sparkBefore = s.sparkAge >= 0.0f;
		const float ageBefore = s.sparkAge;
		for(int i = 0; i < substeps; ++i){
			const float previous = s.sparkAge;
			step(s, t, h);
			if(s.sparkAge >= 0.0f && previous == s.sparkAge){
				s.sparkAge += h;
			}
		}
		if(s.sparkAge >= 0.0f && (!sparkBefore || s.sparkAge < ageBefore)){
			// New hit: brightness from the velocity of the note, in steps of 10%.
			s.sparkLevel = std::round(glm::clamp(velocities[k], 0.0f, 1.0f) * 10.0f) / 10.0f * kSparkDim;
		}
		if(s.sparkAge > kSparkDuration){
			s.sparkAge = -1.0f;
		}
	}
}

glm::vec4 Upright::spark(const NoteState & s) const {
	if(s.sparkAge < 0.0f){
		return glm::vec4(0.0f);
	}
	const float f = glm::clamp(s.sparkAge / kSparkDuration, 0.0f, 1.0f);
	const float rise = 1.0f - std::pow(1.0f - f, 3.0f); // Fast first, slowing down.
	const float height = kSparkStart + (_stringTop - kSparkStart) * rise;
	const float brightness = s.sparkLevel * std::pow(1.0f - f, 1.5f);
	return glm::vec4(height, brightness, kSparkTail, 0.0f);
}

glm::mat4 Upright::about(const glm::vec2 & pivot, float angle) const {
	// Pivots are (y, z); model angles turn +z (back) towards +y (up), i.e. -angle around +x.
	glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, pivot.x, pivot.y));
	m = glm::rotate(m, -angle, glm::vec3(1.0f, 0.0f, 0.0f));
	return glm::translate(m, glm::vec3(0.0f, -pivot.x, -pivot.y));
}

void Upright::instances(const Mesh & mesh, const std::array<float, 128> & anchor, const std::vector<int> & notes, float railLeft, float railRight, std::vector<glm::mat4> & mats, std::vector<glm::vec3> & tints, std::vector<glm::vec3> & glows) const {
	mats.clear();
	tints.clear();
	glows.clear();
	if(mesh.group == RAIL){
		glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(railLeft, 0.0f, 0.0f));
		mats.push_back(glm::scale(m, glm::vec3(railRight - railLeft, 1.0f, 1.0f)));
		tints.emplace_back(1.0f);
		glows.emplace_back(0.0f);
		return;
	}
	// Bass strings are copper wound: tint of the steel material.
	const glm::vec3 copper = glm::vec3(0.72f, 0.42f, 0.24f) / glm::vec3(0.62f, 0.63f, 0.66f);
	for(int n : notes){
		if(mesh.group == KEY && ((mesh.mask >> (n % 12)) & 1u) == 0){
			continue;
		}
		const NoteState & s = _notes[n];
		const glm::mat4 a = glm::translate(glm::mat4(1.0f), glm::vec3(anchor[n], 0.0f, 0.0f));
		glm::vec3 tint(1.0f);
		switch(mesh.group){
			case KEY:
			case CAPSTAN:
				mats.push_back(a * about(_keyPivot, s.phi));
				break;
			case WIPPEN:
				mats.push_back(a * about(_wippenPivot, s.psi));
				break;
			case JACK:
				mats.push_back(a * about(_wippenPivot, s.psi) * about(_jackPivot, s.zeta));
				break;
			case HAMMER:
				mats.push_back(a * about(_hammerPivot, s.chi));
				break;
			case SPRING: {
				// Unit helix along +y, stretched between its seat on the wippen and the toe of the jack.
				const glm::mat4 w = about(_wippenPivot, s.psi);
				const glm::vec4 b = w * glm::vec4(0.0f, _springBase.x, _springBase.y, 1.0f);
				const glm::vec4 t = w * about(_jackPivot, s.zeta) * glm::vec4(0.0f, _springTop.x, _springTop.y, 1.0f);
				const glm::vec2 d(t.y - b.y, t.z - b.z);
				const float len = glm::length(d);
				glm::mat4 m = glm::translate(a, glm::vec3(0.0f, b.y, b.z));
				m = glm::rotate(m, std::atan2(d.y, d.x), glm::vec3(1.0f, 0.0f, 0.0f));
				mats.push_back(glm::scale(m, glm::vec3(1.0f, len, 1.0f)));
				break;
			}
			case STRING: {
				const float f = float(n - 21) / 87.0f;
				const float radius = 3.2f * std::pow(0.6f / 3.2f, f) / 35.0f;
				glm::mat4 m = glm::translate(a, glm::vec3(0.0f, _stringBottom, _stringZ));
				mats.push_back(glm::scale(m, glm::vec3(radius, _stringTop - _stringBottom, radius)));
				if(n < 51){
					tint = copper;
				}
				break;
			}
			default:
				mats.push_back(a);
				break;
		}
		tints.push_back(tint);
		// The visible part of the keys is lit from the inside by the note, as deep as the key is pressed.
		const float pressed = (mesh.group == KEY && mesh.front) ? glm::clamp(s.phi / _phiMax, 0.0f, 1.0f) : 0.0f;
		glows.push_back(_noteColors[n] * (kKeyGlow * pressed));
	}
}

void Upright::drawMeshes(int frontFilter, bool shadowPass, const std::array<float, 128> & anchor, const std::vector<int> & notes, float railLeft, float railRight){
	std::vector<glm::mat4> mats;
	std::vector<glm::vec3> tints;
	std::vector<glm::vec3> glows;
	std::vector<glm::vec4> sparks(kMaxInstances, glm::vec4(0.0f));
	_program.uniform("shadowPass", shadowPass);
	_program.uniform("sparkHeads", false);
	// The keys glow from the inside; the light of the notes only touches the keys, it must not leak between them.
	_program.uniform("translucent", true);
	_program.uniform("noteLightsOn", frontFilter == 1);
	for(const Mesh & mesh : _meshes){
		if(!shadowPass && int(mesh.front) != frontFilter){
			continue;
		}
		instances(mesh, anchor, notes, railLeft, railRight, mats, tints, glows);
		if(mats.empty()){
			continue;
		}
		const size_t count = std::min(mats.size(), size_t(kMaxInstances));
		_program.uniforms("instances", count, mats.data());
		_program.uniforms("instanceColors", count, tints.data());
		// Strings: the spark of each note runs up its string, in the color of the note.
		const bool strings = mesh.group == STRING && !shadowPass;
		_program.uniform("sparks", strings);
		if(strings){
			for(size_t i = 0; i < count; ++i){
				sparks[i] = spark(_notes[notes[i]]);
				glows[i] = _noteColors[notes[i]];
			}
			_program.uniforms("instanceSpark", count, sparks.data());
		}
		_program.uniforms("instanceGlow", count, glows.data());
		const Material & mat = _materials[mesh.material];
		_program.uniform("albedo", mat.color);
		_program.uniform("specular", mat.specular);
		_program.uniform("gloss", mat.gloss);
		glBindVertexArray(mesh.vao);
		glDrawArraysInstanced(GL_TRIANGLES, 0, mesh.count, GLsizei(count));
	}
	_program.uniform("sparks", false);
	// Heads of the flying sparks, with the action (behind the notes).
	if(!shadowPass && frontFilter == 0){
		mats.clear();
		glows.clear();
		std::vector<glm::vec4> heads;
		for(int n : notes){
			const glm::vec4 sp = spark(_notes[n]);
			if(sp.y <= 0.0f || mats.size() >= size_t(kMaxInstances)){
				continue;
			}
			glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(anchor[n], sp.x, _stringZ));
			mats.push_back(glm::scale(m, glm::vec3(kSparkRadius)));
			glows.push_back(_noteColors[n]);
			heads.push_back(sp);
		}
		if(!mats.empty()){
			const std::vector<glm::vec3> black(mats.size(), glm::vec3(0.0f));
			_program.uniform("sparkHeads", true);
			_program.uniforms("instances", mats.size(), mats.data());
			_program.uniforms("instanceColors", black.size(), black.data());
			_program.uniforms("instanceGlow", glows.size(), glows.data());
			_program.uniforms("instanceSpark", heads.size(), heads.data());
			_program.uniform("albedo", glm::vec3(0.0f));
			_program.uniform("specular", 0.0f);
			glBindVertexArray(_sparkHead.vao);
			glDrawArraysInstanced(GL_TRIANGLES, 0, _sparkHead.count, GLsizei(mats.size()));
			_program.uniform("sparkHeads", false);
		}
	}
	glBindVertexArray(0);
}

void Upright::draw(bool front, const glm::vec2 & invScreenSize, const Settings & settings){
	if(!_ready || !loaded()){
		return;
	}
	const glm::vec2 screen = 1.0f / invScreenSize;

	// Layout: the same columns as the notes and the 3D keys. Anchor of a note: the axis of its key lever.
	std::array<float, 128> anchor{};
	std::vector<int> notes;
	{
		int major = 0;
		for(int n = 0; n < 128; ++n){
			bool visible = false;
			if(!isMinorNote(n)){
				const int i = major - int(settings.minKeyMajor);
				visible = i >= 0 && i < int(settings.keyCount);
				anchor[n] = float(i) + _tailOffset[n % 12];
				++major;
			} else {
				// Same layout as the model (even steps of the levers): from the white key on the left.
				const int i = major - 1 - int(settings.minKeyMajor);
				visible = i >= 0 && i < int(settings.keyCount) - 1;
				anchor[n] = float(i) + _tailOffset[n % 12];
			}
			if(visible && n >= std::max(21, settings.minKey) && n <= std::min(108, settings.maxKey)){
				notes.push_back(n);
			}
		}
	}
	if(notes.empty()){
		return;
	}
	const float railLeft = anchor[notes.front()] - _railMargin;
	const float railRight = anchor[notes.back()] + _railMargin;

	// Camera of the 3D keys: scale 1:1 with the flat layers at the end of the visible part of the keys.
	const float count = float(settings.keyCount);
	const float phi = glm::radians(settings.tilt);
	const glm::vec3 forward(0.0f, -std::sin(phi), std::cos(phi));
	const glm::vec3 up(0.0f, std::cos(phi), std::sin(phi));
	const glm::vec3 right(1.0f, 0.0f, 0.0f);
	_eye = glm::vec3(0.5f * count, 0.0f, _visibleEnd) - forward * settings.perspective;
	glm::mat4 view(1.0f);
	for(int c = 0; c < 3; ++c){
		view[c][0] = right[c];
		view[c][1] = up[c];
		view[c][2] = -forward[c];
	}
	view[3] = glm::vec4(-glm::dot(right, _eye), -glm::dot(up, _eye), glm::dot(forward, _eye), 1.0f);
	const float focal = (screen.x / count) * settings.perspective;
	const float lineY = settings.keyboardHeight * screen.y;
	const float zNear = 1.0f;
	const float zFar = 400.0f;
	glm::mat4 proj(0.0f);
	proj[0][0] = 2.0f * focal / screen.x;
	proj[1][1] = 2.0f * focal / screen.y;
	proj[2][1] = -(2.0f * lineY / screen.y - 1.0f);
	proj[2][2] = -(zFar + zNear) / (zFar - zNear);
	proj[3][2] = -2.0f * zFar * zNear / (zFar - zNear);
	proj[2][3] = -1.0f;
	_viewProj = proj * view;

	_program.use();
	_program.uniform("eye", _eye);
	_program.uniform("fillLight", settings.fillLight);
	_program.uniform("ambientLight", settings.ambientLight);
	// The bulb stands where it does on the 3D keys: on the line behind the visible part of the keys.
	const glm::vec3 bulb(settings.backLightPosition * count, settings.backLightHeight, _visibleEnd - 0.244f);
	_program.uniform("backLight", settings.backLight);
	_program.uniform("backLightPos", bulb);
	_program.uniform("backLightRadius", settings.backLightRadius);
	_program.uniform("backLightColor", settings.backLightColor);
	_program.uniform("noteLight", settings.noteLight);
	_program.uniform("noteLightRadius", settings.noteLightRadius);
	{
		// Where each played note touches its key: just above the key top, at the end of its visible part.
		constexpr int maxLights = 24;
		std::array<glm::vec3, maxLights> positions;
		std::array<glm::vec3, maxLights> colors;
		int lightCount = 0;
		const float blackTop = 0.514f;
		for(const auto & lit : settings.litNotes){
			_noteColors[lit.first] = lit.second;
		}
		for(const auto & lit : settings.litNotes){
			if(lightCount >= maxLights){
				break;
			}
			const int n = lit.first;
			if(std::find(notes.begin(), notes.end(), n) == notes.end()){
				continue;
			}
			positions[lightCount] = isMinorNote(n) ? glm::vec3(anchor[n], 0.142f + blackTop, _visibleEnd - 0.03f)
												   : glm::vec3(anchor[n], 0.142f, _visibleEnd - 0.244f);
			colors[lightCount] = lit.second;
			++lightCount;
		}
		_program.uniform("noteLightCount", lightCount);
		if(lightCount > 0){
			_program.uniforms("noteLights", size_t(lightCount), positions.data());
			_program.uniforms("noteLightColors", size_t(lightCount), colors.data());
		}
	}

	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);

	if(!front){
		// Shadow map of the bulb: orthographic, from the action towards the bulb.
		const glm::vec3 center(0.5f * (railLeft + railRight), 5.0f, 12.0f);
		const glm::vec3 toBulb = bulb - center;
		const glm::vec3 lightDir = glm::length(toBulb) > 1e-3f ? glm::normalize(toBulb) : glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 lightEye = center + lightDir * 100.0f;
		const glm::vec3 lightUp = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::mat4 lightView = glm::lookAt(lightEye, center, lightUp);
		const float halfWidth = 0.5f * (railRight - railLeft) + 6.0f;
		const glm::mat4 lightProj = glm::ortho(-halfWidth, halfWidth, -halfWidth, halfWidth, 0.0f, 200.0f);
		_lightViewProj = lightProj * lightView;
		_program.uniform("lightViewProj", _lightViewProj);

		GLint previousFramebuffer = 0;
		GLint viewport[4];
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
		// Not sampled while it is the render target.
		_program.texture("shadowMap", 0, GL_TEXTURE_2D);
		glBindFramebuffer(GL_FRAMEBUFFER, _shadowFramebuffer);
		glViewport(0, 0, _shadowSize, _shadowSize);
		glClear(GL_DEPTH_BUFFER_BIT);
		drawMeshes(-1, true, anchor, notes, railLeft, railRight);
		glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

		// The action, in the back half of the depth buffer; the keys will be drawn in the front half later.
		glClear(GL_DEPTH_BUFFER_BIT);
	}
	_program.uniform("viewProj", _viewProj);
	_program.uniform("lightViewProj", _lightViewProj);
	_program.uniform("depthRange", front ? glm::vec2(0.0f, 0.5f) : glm::vec2(0.5f, 1.0f));
	_program.texture("shadowMap", _shadowTexture, GL_TEXTURE_2D);
	drawMeshes(front ? 1 : 0, false, anchor, notes, railLeft, railRight);

	glDisable(GL_DEPTH_TEST);
	glUseProgram(0);
	checkGLError();
}

void Upright::clean(){
	load("");
	if(_sparkHead.vao){
		glDeleteBuffers(1, &_sparkHead.vbo);
		glDeleteVertexArrays(1, &_sparkHead.vao);
		_sparkHead = Mesh();
	}
	_program.clean();
	if(_shadowFramebuffer){
		glDeleteFramebuffers(1, &_shadowFramebuffer);
		_shadowFramebuffer = 0;
	}
	if(_shadowTexture){
		glDeleteTextures(1, &_shadowTexture);
		_shadowTexture = 0;
	}
	_ready = false;
}
