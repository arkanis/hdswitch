#include <stdlib.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "drawable.h"

static GLuint load_and_link_program(const char *vertex_shader_filename, const char *fragment_shader_filename);
static void   delete_program_and_shaders(GLuint program);
static GLuint create_and_compile_shader(GLenum shader_type, const char *filename);
static bool   gl_ext_present(const char *ext_name);


/**
 * The library requires some OpenGL extentions. This function checks if these are available. If not
 * an error message is printed to stderr for each missing extention.
 * 
 * Returns whether the requirements are met or not.
 */
bool check_required_gl_extentions(){
	const char* extentions[] = {
		// Both extentions are required for texture format and handling
		"GL_ARB_texture_rectangle",
		"GL_ARB_texture_storage",
		"GL_ARB_framebuffer_object",
		NULL
	};
	
	bool requirements_met = true;
	for(size_t i = 0; extentions[i] != NULL; i++){
		if ( !gl_ext_present(extentions[i]) ) {
			requirements_met = false;
			fprintf(stderr, "Required OpenGL extention not available: %s\n", extentions[i]);
		}
	}
	
	return requirements_met;
}


/**
 * Creates a new drawable object with the specified shaders loaded.
 * 
 * Returns the drawable on success or `NULL` on error (e.g. comiler error).
 */
drawable_p drawable_new(GLenum primitive_type, const char* vertex_shader, const char* fragment_shader){
	drawable_p drawable = malloc(sizeof(drawable_t));
	*drawable = (drawable_t){
		.primitive_type = primitive_type,
		.program = load_and_link_program(vertex_shader, fragment_shader),
		.vertex_buffer = 0,
		.texture = 0
	};
	
	if (drawable->program == 0){
		free(drawable);
		return NULL;
	}
	
	return drawable;
}

/**
 * Destroies the drawable object and all associated OpenGL objects. If you don't want to destroy
 * the vertex buffer or texture set them to 0 first!
 */
void drawable_destroy(drawable_p drawable){
	delete_program_and_shaders(drawable->program);
	if (drawable->vertex_buffer)
		buffer_destroy(drawable->vertex_buffer);
	if (drawable->texture)
		texture_destroy(drawable->texture);
	free(drawable);
}

/**
 * Displays all attributes and uniforms of the `program` on stderr. Each line is prefixed with
 * `prefix`.
 */
void drawable_program_inspect(GLuint program){
	char buffer[512];
	GLint size;
	GLenum type;
	
	GLint attrib_count = 0;
	glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &attrib_count);
	fprintf(stderr, "%d attributes:\n", attrib_count);
	
	for(ssize_t i = 0; i < attrib_count; i++){
		glGetActiveAttrib(program, i, sizeof(buffer), NULL, &size, &type, buffer);
		fprintf(stderr, "- \"%s\": size %d, type %d\n", buffer, size, type);
	}
	
	GLint uniform_count = 0;
	glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniform_count);
	fprintf(stderr, "%d uniforms:\n", uniform_count);
	
	for(ssize_t i = 0; i < uniform_count; i++){
		glGetActiveUniform(program, i, sizeof(buffer), NULL, &size, &type, buffer);
		fprintf(stderr, "- \"%s\": size %d, type %d\n", buffer, size, type);
	}
}

/**
 * Activates the program of the drawable so uniforms can be bound to it. Only necessary if you
 * actually want to setup uniforms before drawing.
 */
void drawable_begin_uniforms(drawable_p drawable){
	glUseProgram(drawable->program);
}

/**
 * Draws the vertecies in the vertex buffer with the shader program of the object. If a texture is
 * set it is used to. The vertex buffer is interpreted as 4 floats per vertex. These values are
 * passed to the shader via an attribute named `pos_and_tex`. The texture (if used) is passed via
 * a uniform named `tex`.
 * 
 * Returns `true` on success and `false` on error. In case the vertex attribute or texture uniform
 * is missing an error message is written to stderr. The texture uniform is only checked if a texture
 * is actually used.
 */
bool drawable_draw(drawable_p drawable){
	const char* vertex_attrib_name = "pos_and_tex";
	const char* tex_uniform_name = "tex";
	
	glUseProgram(drawable->program);
	
	// Setup vertex data association for the draw call
	GLint vertex_attrib = glGetAttribLocation(drawable->program, vertex_attrib_name);
	if (vertex_attrib == -1){
		fprintf(stderr, "Can't draw, program doesn't have the \"%s\" attribute!\n", vertex_attrib_name);
		goto vertex_attr_failed;
	}
	
	glBindBuffer(GL_ARRAY_BUFFER, drawable->vertex_buffer);
	
	size_t vertex_size = sizeof(float) * 4;
	GLint vertex_buffer_size = 0;
	glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vertex_buffer_size);
	
	glEnableVertexAttribArray(vertex_attrib);
	glVertexAttribPointer(vertex_attrib, 4, GL_FLOAT, GL_FALSE, vertex_size, 0);
	GLenum error = glGetError();
	
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	
	if (error != GL_NO_ERROR)
		goto vertex_setup_failed;
	
	// Bind texture to slot 0
	if (drawable->texture) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_RECTANGLE, drawable->texture);
		
		GLint tex_attrib = glGetUniformLocation(drawable->program, tex_uniform_name);
		if (tex_attrib == -1) {
			fprintf(stderr, "Can't draw, program doesn't have the \"%s\" uniform (sampler)!\n", tex_uniform_name);
			goto tex_attr_failed;
		}
		
		glUniform1i(tex_attrib, 0);
	}
	
	// Draw the vertecies
	glDrawArrays(drawable->primitive_type, 0, vertex_buffer_size / vertex_size);
	if (glGetError() != GL_NO_ERROR)
		goto draw_failed;
	
	// Cleanup time
	if (drawable->texture)
		glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	
	glDisableVertexAttribArray(vertex_attrib);
	glUseProgram(0);
	
	return true;
	
	draw_failed:
	tex_attr_failed:
		if (drawable->texture)
			glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	vertex_setup_failed:
		glDisableVertexAttribArray(vertex_attrib);
	vertex_attr_failed:
		glUseProgram(0);
	
	return false;
}


/**
 * Creates a new vertex buffer with the specified size and initial data uploaded. The initial data
 * is uploaded with the GL_STATIC_DRAW usage, meant to be used for model data that does not change.
 * 
 * If `data` is `NULL` but a size is given the buffer will be allocated but no data is uploaded.
 * If `size` is `0` only the OpenGL object is created but nothing is allocated.
 * 
 * Returns the vertex buffer on success or `0` on error.
 */
GLuint buffer_new(size_t size, const void* data){
	// Create vertex buffer
	GLuint buffer = 0;
	glGenBuffers(1, &buffer);
	if (buffer == 0)
		return 0;
	
	if (size > 0)
		buffer_update(buffer, size, data, GL_STATIC_DRAW);
	
	return buffer;
}

void buffer_destroy(GLuint buffer){
	glDeleteBuffers(1, (const GLuint[]){ buffer });
}

/**
 * Updates the vertex buffer with new data. The `usage` parameter is the same as of the
 * `glBufferData()` function.
 */
void buffer_update(GLuint buffer, size_t size, const void* data, GLenum usage){
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, size, data, usage);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}


/**
 * Creates a rectangular texture of the specified dimensions and format. The format has to be a sized
 * format, e.g. GL_R8 or GL_RGBA8. See `glTexStorage2D()` for a list of all sized formats.
 * 
 * Returns the texture object on success or `0` on error.
 */
GLuint texture_new(size_t width, size_t height, GLenum format){
	GLuint texture = 0;
	glGenTextures(1, &texture);
	if (texture == 0)
		return 0;
	
	glBindTexture(GL_TEXTURE_RECTANGLE, texture);
	glTexStorage2D(GL_TEXTURE_RECTANGLE, 1, format, width, height);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	
	return texture;
}

void texture_destroy(GLuint texture){
	glDeleteTextures(1, (const GLuint[]){ texture });
}

/**
 * Uploads new data for the specified texture. The data is expected to be as large as the entire
 * texture. A pixel type of GL_UNSIGNED_BYTE is assumed. The `format` parameter specifies the number
 * of components per pixel, e.g. GL_RED or GL_RGBA. See `glTexSubImage2D()` for full format list.
 */
void texture_update(GLuint texture, GLenum format, const void* data){
	glBindTexture(GL_TEXTURE_RECTANGLE, texture);
	
	GLint width = 0, height = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_HEIGHT, &height);
	
	glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
}

void texture_update_part(GLuint texture, GLenum format, const void* data, GLint x, GLint y, GLsizei width, GLsizei height) {
	glBindTexture(GL_TEXTURE_RECTANGLE, texture);
	glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, x, y, width, height, format, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);	
}


//
// Utility functions
//

/**
 * Creates a program out of the specified shader files.
 *
 * If something goes wrong an error message is printed on stderr and 0 is returned.
 */
static GLuint load_and_link_program(const char *vertex_shader_filename, const char *fragment_shader_filename){
	GLuint vertex_shader = create_and_compile_shader(GL_VERTEX_SHADER, vertex_shader_filename);
	GLuint fragment_shader = create_and_compile_shader(GL_FRAGMENT_SHADER, fragment_shader_filename);
	if (vertex_shader == 0 || fragment_shader == 0)
		goto shaders_failed;
	
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vertex_shader);
	glAttachShader(prog, fragment_shader);
	glLinkProgram(prog);
	
	GLint result = GL_TRUE;
	glGetProgramiv(prog, GL_LINK_STATUS, &result);
	if (result == GL_FALSE){
		char buffer[1024];
		glGetProgramInfoLog(prog, sizeof(buffer), NULL, buffer);
		fprintf(stderr, "vertex and pixel shader linking faild:\n%s\n", buffer);
		drawable_program_inspect(prog);
		goto program_failed;
	}
	
	return prog;
	
	program_failed:
		if (prog)
			glDeleteProgram(prog);
		
	shaders_failed:
		if (vertex_shader)
			glDeleteShader(vertex_shader);
		if (fragment_shader)
			glDeleteShader(fragment_shader);
	
	return 0;
}


/**
 * Destorys the specified program and all shaders attached to it.
 */
static void delete_program_and_shaders(GLuint program){
	GLint shader_count = 0;
	glGetProgramiv(program, GL_ATTACHED_SHADERS, &shader_count);
	
	GLuint shaders[shader_count];
	glGetAttachedShaders(program, shader_count, NULL, shaders);
	
	glDeleteProgram(program);
	for(ssize_t i = 0; i < shader_count; i++)
		glDeleteShader(shaders[i]);
}


/**
 * Loads and compiles a source code file as a shader.
 * 
 * Returns the shaders GL object id on success or 0 on error. Compiler errors in the shader are
 * printed on stderr.
 */
static GLuint create_and_compile_shader(GLenum shader_type, const char *filename){
	int fd = open(filename, O_RDONLY, 0);
	struct stat file_stat;
	fstat(fd, &file_stat);
	char *code = mmap(NULL, file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	
	GLuint shader = glCreateShader(shader_type);
	glShaderSource(shader, 1, (const char*[]){ code }, (const int[]){ file_stat.st_size });
	
	munmap(code, file_stat.st_size);
	close(fd);
	
	glCompileShader(shader);
	
	GLint result = GL_TRUE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);
	if (result == GL_FALSE){
		char buffer[1024];
		glGetShaderInfoLog(shader, 1024, NULL, buffer);
		fprintf(stderr, "shader compilation of %s failed:\n%s\n", filename, buffer);
		return 0;
	}
	
	return shader;
}


/**
 * Checks if an OpenGL extention is avaialbe.
 */
static bool gl_ext_present(const char *ext_name){
	GLint ext_count;
	glGetIntegerv(GL_NUM_EXTENSIONS, &ext_count);
	for(ssize_t i = 0; i < ext_count; i++){
		if ( strcmp((const char*)glGetStringi(GL_EXTENSIONS, i), ext_name) == 0 )
			return true;
	}
	return false;
}



//
// Frame buffer object functions
//

fbo_p fbo_new(GLuint target_texture) {
	fbo_p fbo = malloc(sizeof(fbo_t));
	if (fbo == NULL)
		return fbo;
	
	glGenFramebuffers(1, &fbo->fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->fbo);
	
	glBindTexture(GL_TEXTURE_RECTANGLE, target_texture);
		glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_WIDTH, &fbo->width);
		glGetTexLevelParameteriv(GL_TEXTURE_RECTANGLE, 0, GL_TEXTURE_HEIGHT, &fbo->height);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);
	
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, target_texture, 0);
	GLenum error = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	if (error != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "Framebuffer setup failed, error: %4X\n", error);
		
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &fbo->fbo);
		
		free(fbo);
		return NULL;
	}
	
	
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	return fbo;
}

void fbo_destroy(fbo_p fbo) {
	glDeleteFramebuffers(1, &fbo->fbo);
	free(fbo);
}

void fbo_bind(fbo_p fbo) {
	if (fbo != NULL) {
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo->fbo);
		glViewport(0, 0, fbo->width, fbo->height);
	} else {
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	}
}

void fbo_read(fbo_p fbo, GLenum format, GLenum type, void* data) {
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo->fbo);
	glReadPixels(0, 0, fbo->width, fbo->height, format, type, data);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}