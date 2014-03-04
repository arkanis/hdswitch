#pragma once

#include <stdbool.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>


typedef struct {
	GLenum primitive_type;
	GLuint program;
	GLuint vertex_buffer;
	GLuint texture;
} drawable_t, *drawable_p;


/**

Basic API usage:

drawable_p d = drawable_new("object.vs", "object.fs");
if (!d) {
	// shader compiler error or something else happend, abort here
	exit(1);
}

d->vertex_buffer = buffer_new(...);
d->texture = texture_new(...);
texture_update(d->texture, ...);

drawable_begin_uniforms(d);
// do uniforms...
drawable_draw(d);


drawable_destroy(d);

*/

bool        check_required_gl_extentions();

drawable_p  drawable_new(GLenum primitive_type, const char* vertex_shader, const char* fragment_shader);
void        drawable_destroy(drawable_p drawable);
void        drawable_begin_uniforms(drawable_p drawable);
bool        drawable_draw(drawable_p drawable);

void        drawable_program_inspect(GLuint program);

GLuint      buffer_new(size_t size, const void* data);
void        buffer_destroy(GLuint buffer);
void        buffer_update(GLuint buffer, size_t size, const void* data, GLenum usage);

GLuint      texture_new(size_t width, size_t height, GLenum format);
void        texture_destroy(GLuint texture);
void        texture_update(GLuint texture, GLenum format, const void* data);