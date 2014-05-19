#version 120

uniform sampler2DRect tex;
varying vec2 tex_coords;

void main(){
	float intensity = texture2DRect(tex, tex_coords).r;
	vec3 color = vec3(0, 0, 0);
	gl_FragColor = vec4(color, intensity);
}