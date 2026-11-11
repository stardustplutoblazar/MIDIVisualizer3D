#version 330

in INTERFACE {
	vec3 pos;
	vec3 nor;
	vec4 lightPos;
	flat int instance;
} In;

#define MAX_INSTANCES 128
uniform vec3 instanceColors[MAX_INSTANCES]; ///< Per-instance tint (strings: steel or copper), (1,1,1) otherwise.
uniform vec3 instanceGlow[MAX_INSTANCES]; ///< Light inside each key (color of its note, times how far it is pressed).
uniform bool translucent = false; ///< Keys made of a milky material, lit from the inside when played.
uniform bool noteLightsOn = true; ///< Lights of the notes (only on the keys: they must not leak between them).
uniform vec3 albedo;
uniform float specular;
uniform float gloss;
uniform vec3 eye;
uniform sampler2D shadowMap;
uniform bool shadowPass = false;

// Same lights as the 3D keys (keys3d.frag), on the same settings.
uniform float fillLight = 0.7;
uniform float ambientLight = 0.7;
uniform float backLight = 60.0; ///< Warm bulb.
uniform vec3 backLightPos; ///< Its position, computed on the CPU from its settings.
uniform float backLightRadius = 1.3;
uniform vec3 backLightColor = vec3(1.0, 0.72, 0.38);
#define MAX_NOTE_LIGHTS 24
uniform vec3 noteLights[MAX_NOTE_LIGHTS]; ///< Where each played note touches its key.
uniform vec3 noteLightColors[MAX_NOTE_LIGHTS];
uniform int noteLightCount = 0;
uniform float noteLight = 10.0;
uniform float noteLightRadius = 2.0;

out vec4 fragColor;

float shadow(){
	vec3 p = In.lightPos.xyz / In.lightPos.w * 0.5 + 0.5;
	if(any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0)))){
		return 1.0;
	}
	vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
	float lit = 0.0;
	for(int i = -2; i <= 2; ++i){
		for(int j = -2; j <= 2; ++j){
			float d = texture(shadowMap, p.xy + vec2(i, j) * texel).r;
			lit += (p.z - 0.0015 > d) ? 0.0 : 1.0;
		}
	}
	return lit / 25.0;
}

vec3 shade(vec3 n, vec3 v, vec3 l, vec3 base, float g, float sp, vec3 color, float amount){
	float ndl = max(dot(n, l), 0.0);
	float s = pow(max(dot(n, normalize(l + v)), 0.0), g) * sp;
	return (base * ndl + s) * color * amount;
}

void main(){
	if(shadowPass){
		fragColor = vec4(1.0);
		return;
	}
	vec3 n = normalize(In.nor);
	vec3 v = normalize(eye - In.pos);
	if(dot(n, v) < 0.0){
		n = -n;
	}
	vec3 base = albedo * instanceColors[In.instance];
	vec3 p = In.pos;

	// Ambient light takes the color of the bulb: the whole scene is bathed in its warmth.
	vec3 color = base * 0.085 * backLightColor * ambientLight;
	vec3 fillDir = normalize(vec3(0.0, 0.25, -1.0));
	color += shade(n, v, fillDir, base, gloss, specular * 0.2, vec3(0.30, 0.31, 0.34), fillLight);

	// The bulb, with the shadows of the action.
	if(backLight > 0.0){
		vec3 l = backLightPos - p;
		float dl = length(l);
		l /= dl;
		float fall = backLight / (1.0 + pow(dl / max(backLightRadius, 0.01), 2.0));
		color += shade(n, v, l, base, gloss, specular, backLightColor, fall * shadow());
	}

	// Light from the point where each played note touches its key.
	float reach = 4.0 * noteLightRadius;
	for(int i = 0; i < (noteLightsOn ? noteLightCount : 0); ++i){
		vec3 lp = noteLights[i];
		if(abs(lp.x - p.x) > reach){
			continue;
		}
		for(int s = 0; s < 2; ++s){
			vec3 l = lp + vec3(0.0, float(s) * 3.0 * 0.142, 0.0) - p;
			float dl = length(l);
			l /= dl;
			float fall = noteLight * (s == 0 ? 0.7 : 0.3) / (1.0 + pow(dl / noteLightRadius, 2.0));
			fall *= clamp((reach - dl) / (2.0 * noteLightRadius), 0.0, 1.0);
			color += shade(n, v, l, base, gloss, specular * 1.3, noteLightColors[i], fall);
		}
	}

	// Lit from the inside: the light comes out more where the material is seen at a grazing angle
	// (edges, thin parts), like milky plastic.
	if(translucent){
		float edge = pow(1.0 - abs(dot(n, v)), 1.5);
		color += instanceGlow[In.instance] * (0.55 + 1.2 * edge);
	}

	// Soft tone mapping, so that light does not clip.
	color = (1.0 - exp(-color * 1.15)) / (1.0 - exp(-1.15));
	fragColor = vec4(color, 1.0);
}
