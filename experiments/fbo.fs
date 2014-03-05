#version 120

uniform sampler2DRect tex;
varying vec2 tex_coords;

void main(){
	gl_FragColor = vec4( texture2DRect(tex, tex_coords).rgb, 1 );
}