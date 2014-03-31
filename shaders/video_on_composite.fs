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
	
	// Show luma and chroma samples
	float y = texture2DRect(tex, tex_coords).r;
	
	float x_mod_2 = mod(tex_coords.x, 2.0);
	float cb = texture2DRect(tex, tex_coords + vec2(-x_mod_2 + 0.5, 0.5)).g;
	float cr = texture2DRect(tex, tex_coords + vec2(-x_mod_2 + 1.5, 0.5)).g;
	
	// ITU-R BT.601 conversation using limited YCbCr color space (seems like webcams are using this colorspace)
	// http://en.wikipedia.org/wiki/YCbCr
	gl_FragColor = vec4(
		(298.082 / 256.0) * y                          + (408.583 / 256.0) * cr - (222.921 / 256.0),
		(298.082 / 256.0) * y - (100.291 / 256.0) * cb - (208.120 / 256.0) * cr + (135.576 / 256.0),
		(298.082 / 256.0) * y + (516.412 / 256.0) * cb                          - (276.836 / 256.0),
		1
	);
	
	/*
	// JPEG color conversion with full Cb and Cr spectrum from Wikipedia (http://en.wikipedia.org/wiki/YCbCr)
	gl_FragColor = vec4(
		y                         + 1.402   * (cr - 0.5),
		y - 0.344144 * (cb - 0.5) - 0.71414 * (cr - 0.5),
		y + 1.772    * (cb - 0.5),
		1
	);
	*/
}