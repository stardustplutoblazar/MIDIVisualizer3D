#version 330

// Upright piano mode: one template mesh drawn once per note (instanced), or once for rails and keybed.
layout(location = 0) in vec3 v;
layout(location = 1) in vec3 n;

#define MAX_INSTANCES 128
uniform mat4 instances[MAX_INSTANCES]; ///< Model matrix of each note (or of the stretched rails).
uniform mat4 viewProj;
uniform mat4 lightViewProj;
uniform bool shadowPass = false;
uniform vec2 depthRange = vec2(0.0, 1.0); ///< Fraction of the depth buffer used by this pass.

out INTERFACE {
	vec3 pos;
	vec3 nor;
	vec4 lightPos;
	flat int instance;
} Out;

void main(){
	mat4 model = instances[gl_InstanceID];
	vec4 world = model * vec4(v, 1.0);
	Out.pos = world.xyz;
	Out.nor = normalize(mat3(model) * n);
	Out.lightPos = lightViewProj * world;
	Out.instance = gl_InstanceID;
	if(shadowPass){
		gl_Position = Out.lightPos;
		return;
	}
	gl_Position = viewProj * world;
	// Each pass (behind the notes / in front of them) gets its own slice of the depth buffer.
	float z = gl_Position.z / gl_Position.w * 0.5 + 0.5;
	gl_Position.z = (mix(depthRange.x, depthRange.y, clamp(z, 0.0, 1.0)) * 2.0 - 1.0) * gl_Position.w;
}
