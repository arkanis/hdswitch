#version 120

attribute vec4 pos_and_tex;
varying   vec2 tex_coords;

void main(){
	gl_Position.xy = pos_and_tex.xy;
	gl_Position.zw = vec2(0, 1);
	tex_coords.xy = pos_and_tex.zw;
}