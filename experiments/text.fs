#version 120

uniform sampler2DRect tex;
varying vec2 tex_coords;

void main(){
	vec3 color = texture2DRect(tex, tex_coords).rgb;
	//gl_FragColor = vec4(color, 1);
	gl_FragColor = vec4( color, (color.r + color.g + color.b) / 3.0 );
}