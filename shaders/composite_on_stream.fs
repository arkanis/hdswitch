#version 130

uniform sampler2DRect tex;
varying vec2 tex_coords;

void main(){
	vec3 rgb = texture2DRect(tex, tex_coords).rgb;
	
	// ITU-R BT.601 conversation using limited YCbCr color space (seems like webcams are using this colorspace)
	// http://en.wikipedia.org/wiki/YCbCr
	float y  = ( 16 / 256.0) + ( ( 65.481 / 256.0) * rgb.r + (128.553 / 256.0) * rgb.g + ( 24.966 / 256.0) * rgb.b );
	float cb = (128 / 256.0) + ( (-37.797 / 256.0) * rgb.r - ( 74.203 / 256.0) * rgb.g + (112.0   / 256.0) * rgb.b );
	float cr = (128 / 256.0) + ( (112.0   / 256.0) * rgb.r - ( 93.786 / 256.0) * rgb.g - ( 18.214 / 256.0) * rgb.b );
	
	/*
	// JPEG color conversion with full YCbCr spectrum from Wikipedia (http://en.wikipedia.org/wiki/YCbCr)
	float y  =       0.299    * rgb.r + 0.587    * rgb.g + 0.114    * rgb.b;
	float cb = 0.5 - 0.168736 * rgb.r - 0.331264 * rgb.g + 0.5      * rgb.b;
	float cr = 0.5 + 0.5      * rgb.r - 0.418688 * rgb.g - 0.081312 * rgb.b;
	*/
	
	float x_mod_2 = mod(tex_coords.x, 2.0);
	
	gl_FragColor = vec4(
		y,
		(x_mod_2 <= 1.0) ? cb : cr,
		0,
		1
	);
}