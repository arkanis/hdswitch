#version 120

attribute vec2 pos;
attribute vec3 color;
varying   vec3 pixel_color;
uniform   mat3 screen_to_normal;

void main(){
	pixel_color = color;
	gl_Position.xy = (screen_to_normal * vec3(pos, 1)).xy;
	gl_Position.zw = vec2(0, 1);
}