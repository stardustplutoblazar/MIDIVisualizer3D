#version 330

in INTERFACE {
	vec3 pos;
	vec3 nor;
	vec4 lightPos;
	flat int instance;
} In;

#define MAX_INSTANCES 128
uniform vec3 instanceColors[MAX_INSTANCES]; ///< Per-instance tint (strings: steel or copper), (1,1,1) otherwise.
uniform vec3 albedo;
uniform float specular;
uniform float gloss;
uniform vec3 eye;
uniform vec3 lightDir; ///< Towards the light.
uniform vec3 lightColor = vec3(1.0, 0.94, 0.84);
uniform float keyLight = 0.85;
uniform float fillLight = 0.8;
uniform float ambientLight = 0.55;
uniform sampler2D shadowMap;
uniform bool shadowPass = false;

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
	vec3 l = normalize(lightDir);
	float sh = shadow();
	float ndl = max(dot(n, l), 0.0);
	vec3 h = normalize(l + v);
	float sp = pow(max(dot(n, h), 0.0), gloss) * specular;
	vec3 sky = mix(vec3(0.30, 0.28, 0.26), vec3(0.62, 0.64, 0.70), n.y * 0.5 + 0.5);
	vec3 fill = vec3(0.25, 0.22, 0.18) * max(dot(n, normalize(vec3(0.6, 0.3, -0.5))), 0.0);
	vec3 base = albedo * instanceColors[In.instance];
	vec3 c = base * (sky * ambientLight + fill * fillLight + lightColor * ndl * keyLight * sh) + vec3(1.0, 0.97, 0.9) * sp * sh * 0.8;
	fragColor = vec4(pow(c, vec3(1.0 / 1.05)), 1.0);
}
