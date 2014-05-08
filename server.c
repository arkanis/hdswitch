// For accept4() and open_memstream() (needs _POSIX_C_SOURCE 200809L which is also defined by _GNU_SOURCE)
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "list.h"
#include "ebml_writer.h"
#include "server.h"


typedef struct {
	int fd;
	pa_io_event* io_event;
	
	void* ptr;
	size_t size;
	
	list_node_p current_buffer_node;
	list_node_p disconnect_at_node;
} client_t, *client_p;

typedef struct {
	void*  ptr;
	size_t size;
	size_t refcount;
} buffer_t, *buffer_p;

pa_mainloop_api *server_mainloop = NULL;
const char* server_socket_path = NULL;
int server_fd = -1;
list_p clients = NULL;
list_p buffers = NULL;

buffer_t header;


static void on_accept(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void on_client_writable(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata);
static void on_client_disconnect(pa_mainloop_api *mainloop, list_node_p client_node);

static void mkv_build_header(uint16_t width, uint16_t height, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
static void buffer_node_unref(list_node_p buffer_node);



bool server_start(const char* socket_path, uint16_t width, uint16_t height, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample, pa_mainloop_api* mainloop) {
	server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server_fd == -1)
		return perror("[server] socket"), false;
	
	server_socket_path = socket_path;
	unlink(socket_path);
	
	struct sockaddr_un addr = { AF_UNIX, "" };
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	if ( bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1 )
		return perror("[server] bind"), false;
	
	if ( listen(server_fd, 3) == -1 )
		return perror("[server] listen"), false;
	
	clients = list_of(client_t);
	buffers = list_of(buffer_t);
	mkv_build_header(width, height, sample_rate, channels, bits_per_sample);
	
	server_mainloop = mainloop;
	server_mainloop->io_new(server_mainloop, server_fd, PA_IO_EVENT_INPUT, on_accept, NULL);
	
	return true;
}

void server_stop() {
	for(list_node_p n = clients->first; n != NULL; n = n->next) {
		client_p client = list_value_ptr(n);
		
		close(client->fd);
		server_mainloop->io_free(client->io_event);
	}
	list_destroy(clients);
	close(server_fd);
	
	list_destroy(buffers);
	free(header.ptr);
	
	unlink(server_socket_path);
}

void server_enqueue_frame(uint8_t track, uint64_t timecode_us, void* frame_data, size_t frame_size) {
	size_t connected_client_count = list_count(clients);
	// Throw the buffer away if no one is listening
	if (connected_client_count == 0)
		return;
	
	//printf("[server] queuing frame\n");
	buffer_p buffer = list_append_ptr(buffers);
	buffer->refcount = connected_client_count;
	
	FILE* f = open_memstream((char**)&buffer->ptr, &buffer->size);
	
	off_t o2, o3;
	o2 = ebml_element_start(f, MKV_Cluster);
		ebml_element_uint(f, MKV_Timecode, timecode_us);
		o3 = ebml_element_start(f, MKV_SimpleBlock);
			// Track number this frame belongs to
			ebml_write_data_size(f, track, 0);
			
			int16_t block_timecode = 0;
			fwrite(&block_timecode, sizeof(block_timecode), 1, f);
			
			uint8_t flags = 0x80; // keyframe (1), reserved (000), not invisible (0), no lacing (00), not discardable (0)
			fwrite(&flags, sizeof(flags), 1, f);
			
			fwrite(frame_data, frame_size, 1, f);
		ebml_element_end(f, o3);
	ebml_element_end(f, o2);
	
	fclose(f);
	
	// Check all clients and resume writing if necessary
	for(list_node_p n = clients->first; n != NULL; n = n->next) {
		client_p client = list_value_ptr(n);
		
		if (client->current_buffer_node == NULL) {
			// Switch client to the new buffer it it's stalled
			client->current_buffer_node = buffers->last;
			if (client->ptr == NULL) {
				client->ptr  = buffer->ptr;
				client->size = buffer->size;
			}
			
			// Enable this client in the mainloop
			//printf("[client %d] resuming\n", client->fd);
			server_mainloop->io_enable(client->io_event, PA_IO_EVENT_OUTPUT);
		}
	}
}

void server_flush_and_disconnect_clients() {
	for(list_node_p n = clients->first; n != NULL; n = n->next) {
		client_p client = list_value_ptr(n);
		client->disconnect_at_node = buffers->last;
	}
}



//
// Event handlers
//

static void on_accept(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	int client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK);
	if (client_fd == -1) {
		perror("accept4");
		return;
	}
	
	client_p client = list_append_ptr(clients);
	list_node_p client_node = clients->last;
	
	client->fd = client_fd;
	client->io_event = mainloop->io_new(mainloop, client->fd, PA_IO_EVENT_OUTPUT, on_client_writable, client_node);
	
	client->ptr = header.ptr;
	client->size = header.size;
	client->current_buffer_node = NULL;
	client->disconnect_at_node = NULL;
	
	printf("[client %d] connected\n", client->fd);
}

static void on_client_writable(pa_mainloop_api *mainloop, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
	client_p client = list_value_ptr(userdata);
	//printf("[client %d] writing data\n", client->fd);
	
	while (true) {
		ssize_t bytes_written = 0;
		while (client->size > 0) {
			bytes_written = write(client->fd, client->ptr, client->size);
			if (bytes_written < 0) {
				if (errno == EWOULDBLOCK) {
					// Very common case, do nothing
				} else if (errno == EPIPE) {
					on_client_disconnect(mainloop, userdata);
				} else {
					perror("write");
				}
				break;
			}
			
			client->ptr  += bytes_written;
			client->size -= bytes_written;
		}
		
		if (bytes_written >= 0) {
			// Buffer finished, switch to next one
			client->ptr = NULL;
			
			if (client->current_buffer_node != NULL) {
				list_node_p finished_buffer_node = client->current_buffer_node;
				client->current_buffer_node = client->current_buffer_node->next;
				buffer_node_unref(finished_buffer_node);
			}
			
			if (client->disconnect_at_node != NULL && client->disconnect_at_node == client->current_buffer_node) {
				// Client is marked to be disconnected as soon as it encounters the
				// current buffer. So do it.
				//printf("[client %d] disconnecting at buffer %p\n", client->fd, client->disconnect_at_node);
				on_client_disconnect(mainloop, userdata);
				break;
			}
			
			if (client->current_buffer_node != NULL) {
				// There is a next buffer ready. Switch this client to it.
				//printf("[client %d] switching to next buffer\n", client->fd);
				buffer_p next_buffer = list_value_ptr(client->current_buffer_node);
				client->ptr  = next_buffer->ptr;
				client->size = next_buffer->size;
			} else {
				// No next buffer, the client is stalled now. Disable it from the
				// mainloop since we don't have any data to write. We'll resume when
				// the next buffer comes around.
				//printf("[client %d] stalled\n", client->fd);
				mainloop->io_enable(client->io_event, PA_IO_EVENT_NULL);
				break;
			}
		} else {
			// Didn't finish buffer, continue next time we can write to client
			break;
		}
	}
}

static void on_client_disconnect(pa_mainloop_api *mainloop, list_node_p client_node) {
	client_p client = list_value_ptr(client_node);
	
	printf("[client %d] disconnected\n", client->fd);
	
	// Cleanup this client
	close(client->fd);
	mainloop->io_free(client->io_event);
	
	// unref all buffers this client would've read
	if (client->current_buffer_node) {
		for (list_node_p node = client->current_buffer_node, next = NULL; node != NULL; node = next) {
			next = node->next;
			buffer_node_unref(node);
		}
	}
	
	list_remove(clients, client_node);
}



//
// Utility functions
//

static void mkv_build_header(uint16_t width, uint16_t height, uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
	FILE* f = open_memstream((char**)&header.ptr, &header.size);
	
	off_t o1, o2, o3, o4;
	
	o1 = ebml_element_start(f, MKV_EBML);
		ebml_element_string(f, MKV_DocType, "matroska");
	ebml_element_end(f, o1);
	
	ebml_element_start_unkown_data_size(f, MKV_Segment);
	
	o2 = ebml_element_start(f, MKV_Info);
		// specify timestamps in microseconds (usec), matches the timestamp format of Pulse Audio
		ebml_element_uint(f, MKV_TimecodeScale, 1000);
		ebml_element_string(f, MKV_MuxingApp, "ebml_writer v0.1");
		ebml_element_string(f, MKV_WritingApp, "HDswitch v0.1");
	ebml_element_end(f, o2);
	
	o2 = ebml_element_start(f, MKV_Tracks);
		// Video track
		o3 = ebml_element_start(f, MKV_TrackEntry);
			ebml_element_uint(f, MKV_TrackNumber, 1);
			ebml_element_uint(f, MKV_TrackUID, 1);
			ebml_element_uint(f, MKV_TrackType, MKV_TrackType_Video);
			
			ebml_element_string(f, MKV_CodecID, "V_UNCOMPRESSED");
			// These were not included in files generated by mkclean
			//ebml_element_uint(f, MKV_FlagEnabled, 1);
			//ebml_element_uint(f, MKV_FlagDefault, 1);
			//ebml_element_uint(f, MKV_FlagForced, 1);
			ebml_element_uint(f, MKV_FlagLacing, 1);
			ebml_element_string(f, MKV_Language, "und");
			
			o4 = ebml_element_start(f, MKV_Video);
				ebml_element_uint(f, MKV_PixelWidth, width);
				ebml_element_uint(f, MKV_PixelHeight, height);
				ebml_element_string(f, MKV_ColourSpace, "YUY2");
			ebml_element_end(f, o4);
			
		ebml_element_end(f, o3);
		
		// Audio track
		o3 = ebml_element_start(f, MKV_TrackEntry);
			ebml_element_uint(f, MKV_TrackNumber, 2);
			ebml_element_uint(f, MKV_TrackUID, 2);
			ebml_element_uint(f, MKV_TrackType, MKV_TrackType_Audio);
			
			ebml_element_string(f, MKV_CodecID, "A_PCM/INT/LIT");
			ebml_element_uint(f, MKV_FlagLacing, 1);
			ebml_element_string(f, MKV_Language, "ger");
			
			o4 = ebml_element_start(f, MKV_Audio);
				ebml_element_float(f, MKV_SamplingFrequency, sample_rate);
				ebml_element_uint(f, MKV_Channels, channels);
				ebml_element_uint(f, MKV_BitDepth, bits_per_sample);
			ebml_element_end(f, o4);
			
		ebml_element_end(f, o3);
		
	ebml_element_end(f, o2);
	
	fclose(f);
}

static void buffer_node_unref(list_node_p buffer_node) {
	buffer_p buffer = list_value_ptr(buffer_node);
	buffer->refcount--;
	
	if (buffer->refcount == 0) {
		free(buffer->ptr);
		list_remove(buffers, buffer_node);
		//printf("[server] freeing buffer\n");
	}
}