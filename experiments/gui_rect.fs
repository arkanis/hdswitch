#version 120

varying vec3 pixel_color;

void main(){
	gl_FragColor = vec4(pixel_color, 1);
}