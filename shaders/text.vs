#version 120

attribute vec4 pos_and_tex;
varying   vec2 tex_coords;
uniform   mat3 screen_to_normal;

void main(){
	gl_Position.xy = (screen_to_normal * vec3(pos_and_tex.xy, 1)).xy;
	gl_Position.zw = vec2(0, 1);
	tex_coords.xy = pos_and_tex.zw;
}