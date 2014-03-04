#version 130

/**

Y = Luma
U = Cb
V = Cr

YUYV = Luma Cb Luma Cr

See http://www.fourcc.org/yuv.php#YUYV

*/

uniform sampler2DRect tex;
varying vec2 tex_coords;

void main(){
	// Just show the luma channel
	//gl_FragColor = vec4( texture2DRect(tex, tex_coords).rrr, 1 );
	
	/*
	// Show luma and chroma samples
	vec2 this_pixel = texture2DRect(tex, tex_coords).rg;
	float y = this_pixel.r, cr = 0.0, cb = 0.0;
	
	if ( mod(tex_coords.x, 2.0) < 1.0 ) {
		// Even pixel, we have the Cb value, the next pixel the Cr value
		cb = this_pixel.g;
		cr = texture2DRect(tex, vec2( 1, 0) + tex_coords).g;
	} else {
		// Uneven pixel, we have the Cr value, the previous pixel the Cb value
		cb = texture2DRect(tex, vec2(-1, 0) + tex_coords).g;
		cr = this_pixel.g;
	}
	
	gl_FragColor = vec4(
		y                         + 1.402   * (cr - 0.5),
		y - 0.344144 * (cb - 0.5) - 0.71414 * (cr - 0.5),
		y + 1.772    * (cb - 0.5),
		1
	);
	*/
	
	
	float y = texture2DRect(tex, tex_coords).r;
	
	float x_mod_2 = mod(tex_coords.x, 2.0);
	float cb = texture2DRect(tex, tex_coords + vec2(-x_mod_2 + 0.5, 0.5)).g;
	float cr = texture2DRect(tex, tex_coords + vec2(-x_mod_2 + 1.5, 0.5)).g;
	
	gl_FragColor = vec4(
		y                         + 1.402   * (cr - 0.5),
		y - 0.344144 * (cb - 0.5) - 0.71414 * (cr - 0.5),
		y + 1.772    * (cb - 0.5),
		1
	);
	//gl_FragColor *= 0.0000001;
	//gl_FragColor += vec4(x_mod_2, x_mod_2, x_mod_2, 2) / 2;
	
	/*
	vec2 coords = tex_coords;
	coords.x -= mod(tex_coords.x, 2.0) - 1.0;
	gl_FragColor = vec4(texture2DRect(tex, coords).ggg, 1);
	*/
}