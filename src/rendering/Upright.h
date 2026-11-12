#pragma once
#include <gl3w/gl3w.h>
#include <glm/glm.hpp>
#include "../helpers/ProgramUtilities.h"
#include <array>
#include <string>
#include <vector>

/// Upright piano mode: keys and action of an upright piano, from a .piano file exported from a 3D model.
/// Each note has its own action, driven by the key travel: the wippen follows the key, the jack throws the
/// hammer at the string, the hammer bounces back and is caught by the backcheck. Kinematics come from tables
/// computed on the model (contacts between the parts), the rest is a small per-note simulation.
/// Drawn in two passes: what is behind the visible part of the keys (the action) just above the background
/// image, and the visible part of the keys where the keyboard is drawn, so that the notes fall between them.
/// The spoon of each wippen lifts the damper off the string from the middle of the key travel.
class Upright {
public:

	struct Settings {
		float tilt = 40.0f; ///< Camera angle below the horizon, degrees (same camera as the 3D keys).
		float perspective = 91.0f; ///< Camera distance, white key widths.
		float keyboardHeight = 0.2f; ///< Fraction of the frame below the line where notes end.
		float minorWidth = 0.55f;
		int minKey = 21;
		int maxKey = 108;
		unsigned int minKeyMajor = 0;
		unsigned int keyCount = 52; ///< White keys on screen.
		// Lights of the 3D keys.
		float fillLight = 0.7f;
		float ambientLight = 0.7f;
		float backLight = 60.0f;
		float backLightRadius = 1.3f;
		float backLightPosition = 0.5f;
		float backLightHeight = 10.0f;
		glm::vec3 backLightColor{1.0f, 0.72f, 0.38f};
		float noteLight = 10.0f;
		float noteLightRadius = 2.0f;
		/// Played notes and the color of their light.
		std::vector<std::pair<int, glm::vec3>> litNotes;
		/// Debug: note tiles lying on the levers, coming from under the capstans towards the visible keys.
		struct Tile {
			int note = 0;
			float front = 0.0f; ///< Along the lever: 0 at the end of the visible key, 1 at the capstan.
			float back = 0.0f;
			glm::vec3 color{1.0f};
		};
		std::vector<Tile> tiles;
	};

	void setup();

	/// Load a .piano file, returns false and sets the error on failure. Empty path unloads.
	bool load(const std::string & path);

	bool loaded() const { return !_meshes.empty(); }

	const std::string & error() const { return _error; }

	/// Centre of each black note from the boundary between its two white keys, in white key widths,
	/// so that the notes fall on the keys of the model.
	std::array<float, 12> minorOffsets() const {
		std::array<float, 12> o{};
		for(int pc : {1, 3, 6, 8, 10}){ o[pc] = _tailOffset[pc] - 1.0f; }
		return o;
	}

	/// Advance the simulation of every note. travel: 0 rest, 1 key at the bottom. snap: jump to the steady state.
	void update(const std::array<float, 128> & travel, float dt, bool snap);

	/// Shadow map and action (front == false), or visible part of the keys (front == true).
	void draw(bool front, const glm::vec2 & invScreenSize, const Settings & settings);

	void clean();

private:

	struct Mesh {
		GLuint vao = 0;
		GLuint vbo = 0;
		GLsizei count = 0;
		int group = 0;
		bool front = false;
		unsigned int mask = 0xFFF;
		int material = 0;
	};

	struct Material {
		glm::vec3 color{1.0f};
		float specular = 0.0f;
		float gloss = 1.0f;
	};

	struct NoteState {
		float psi = 0.0f; ///< Wippen angle (negative: lifted).
		float psiVel = 0.0f;
		float prevTarget = 0.0f; ///< Wippen angle given by the key at the previous step.
		bool hasPrevTarget = false;
		float prevCatch = 0.0f; ///< Catch angle at the previous step, to know if the backcheck is moving.
		bool hasPrevCatch = false;
		float lastTravel = 0.0f; ///< Key travel at the previous step, to know if the key goes down.
		float chi = 0.0f; ///< Hammer angle (negative: towards the string).
		float omega = 0.0f; ///< Hammer angular speed.
		float zeta = 0.0f; ///< Jack tilt away from the butt.
		float zetaVel = 0.0f;
		bool escaped = false; ///< The jack has let the hammer go (set-off).
		bool armed = false; ///< The hammer went past the backcheck after set-off: it will be caught.
		float phi = 0.0f; ///< Key angle, for drawing.
		float travel = 0.0f; ///< Key travel 0..1, for the glow.
		float damper = 0.0f; ///< Damper lever angle (0: on the string).
	};

	enum Group { KEY = 0, CAPSTAN, WIPPEN, JACK, HAMMER, STATIC, RAIL, SPRING, STRING, DAMPER };

	float table(const std::vector<float> & t, float phi) const;
	/// Linear interpolation in a table sampled over [0, 1].
	static float lerpTable(const std::vector<float> & t, float x);
	float escape(float phi, float chi) const;
	float phiOfPsi(float psi) const;
	void step(NoteState & s, float travel, float h, bool black) const;

	/// Model matrix of each instance of a mesh, and the color tint of each one.
	void instances(const Mesh & mesh, const std::array<float, 128> & anchor, const std::vector<int> & notes, float railLeft, float railRight, std::vector<glm::mat4> & mats, std::vector<glm::vec3> & tints, std::vector<glm::vec3> & glows) const;
	glm::mat4 about(const glm::vec2 & pivot, float angle) const;

	/// Debug tiles on the levers (action pass).
	void drawTiles(const Settings & settings, const std::array<float, 128> & anchor, const std::vector<int> & notes);

	void drawMeshes(int frontFilter, bool shadowPass, const std::array<float, 128> & anchor, const std::vector<int> & notes, float railLeft, float railRight);

	ShaderProgram _program;
	bool _ready = false;
	std::vector<Mesh> _meshes;
	std::vector<Material> _materials;
	std::string _error;

	// Model constants (mod coordinates: x across in white key widths, y up, z towards the back).
	float _phiMax = 0.05f;
	float _chiString = -0.37f;
	float _chiSetoff = -0.35f;
	float _phiMaxBlack = 0.05f; ///< Black keys: their own pivot and key angle, same capstan lift.
	glm::vec2 _keyPivotBlack{0.0f}, _damperPivot{0.0f};
	glm::vec2 _keyPivot{0.0f}, _wippenPivot{0.0f}, _jackPivot{0.0f}, _hammerPivot{0.0f}, _springBase{0.0f}, _springTop{0.0f};
	float _stringZ = 0.0f, _stringBottom = 0.0f, _stringTop = 0.0f, _visibleEnd = 6.0f, _railMargin = 0.5f;
	std::array<float, 12> _tailOffset{};
	std::vector<float> _psi, _psiBlack, _push, _catch, _damper, _escape;
	int _escapeRows = 0, _escapeCols = 0;

	std::array<NoteState, 128> _notes{};
	/// Key travel at the previous frame of each note.
	std::array<float, 128> _prevTravel{};
	/// Color of the last note played on each key: the key keeps glowing in it while it comes back up.
	std::array<glm::vec3, 128> _noteColors{};

	GLuint _tileVao = 0; ///< Unit quad lying flat: x in [-0.5, 0.5], y 0, z in [0, 1].
	GLuint _tileVbo = 0;

	GLuint _shadowFramebuffer = 0;
	GLuint _shadowTexture = 0;
	int _shadowSize = 2048;
	glm::mat4 _lightViewProj{1.0f};
	glm::mat4 _viewProj{1.0f};
	glm::vec3 _eye{0.0f};
};
