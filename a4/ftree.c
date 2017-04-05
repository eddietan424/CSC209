#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ftree.h"

#define MAX_BACKLOG 5
#define MAX_CONNECTIONS 12
#define BUF_SIZE 128
#define CURRENT_WORKING_DIR "./"

struct request_received {
	int type;           // Request type is REGFILE, REGDIR, TRANSFILE
	char path[MAXPATH];
	mode_t mode;
	char hash[BLOCK_SIZE];
	int size;
	int state;
};

int accept_connection(int fd);
char *extract_name(char *fname);
char *str_copy(char* src);
char *generate_path(char *fname, char *c_name);
void client_write_str(int sock_fd, char *buf);
char *server_generate_copy_root(int sock_fd);

int rcopy_client(char *source, char *host, unsigned short port) {
	// Create the socket FD.
	int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("client: socket");
		exit(1);
	}
	
	// Set the IP and port of the server to connect to.
	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &server.sin_addr) < 1) {
		perror("client: inet_pton");
		close(sock_fd);
		exit(1);
	}
	
	// Connect to the server.
	if (connect(sock_fd, (struct sockaddr *)&server, sizeof(server)) == -1) {
		perror("client: connect");
		close(sock_fd);
		exit(1);
	}
	
	// Tell server the basename of input source file/ dir
	char *basename = extract_name(source);
	printf("basename: %s\n", basename);
	client_write_str(sock_fd, basename);
	
	// Maintain original path
	char *ori_path = source;
	
	// Identify rcopy_server itself as a main client
	// Initialize corresponding request
	struct stat src_stat;
	if (lstat(source, &src_stat) < 0) {
		perror("stat");
		exit(1);
	}
	struct request req = {.mode = src_stat.st_mode, .size = src_stat.st_size};
	struct request *req_ptr = &req;
	req_ptr->path[0] = '\0';
	int n = 0;
	if (MAX_PATH - 1 > strlen(basename)) {
		n = strlen(basename);
	} else {
		n = MAX_PATH - 1;
	}
	strncpy(req_ptr->path, basename, n);
	req_ptr->path[n] = '\0';
	free(basename);
	if (S_ISREG(src_stat.st_mode)) { // a regular file
		req_ptr->type = REGFILE;
		compute_hash(ori_path, req_ptr);
	} else if (S_ISDIR(src_stat.st_mode)) { // a directory
		req_ptr->type = REGDIR;
		for (int i = 0; i < BLOCK_SIZE; i++) {
			req_ptr->hash[i] = '\0';
		}
	} else {
		exit(1);
	}
	
	// Send the request
	client_write_fields(int sock_fd, struct request *req_ptr);
	
	while (1) {
	}
	
	close(sock_fd);
	return 0;
}

void rcopy_server(unsigned short port) {
	
	// Create the socket FD.
	int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		perror("server: socket");
		exit(1);
	}
	
	// Set information about the port (and IP) we want to be connected to.
	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	server.sin_addr.s_addr = INADDR_ANY;
	
	// This should always be zero. On some systems, it won't error if you
	// forget, but on others, you'll get mysterious errors. So zero it.
	memset(&server.sin_zero, 0, 8);
	
	// Bind the selected port to the socket.
	if (bind(sock_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
		perror("server: bind");
		close(sock_fd);
		exit(1);
	}
	
	// Announce willingness to accept connections on this socket.
	if (listen(sock_fd, MAX_BACKLOG) < 0) {
		perror("server: listen");
		close(sock_fd);
		exit(1);
	}
//	
//	// The client accept - message accept loop. First, we prepare to listen to multiple
//	// file descriptors by initializing a set of file descriptors.
//	int max_fd = sock_fd;
//	fd_set all_fds, listen_fds;
//	FD_ZERO(&all_fds);
//	FD_SET(sock_fd, &all_fds);
	
	while (1) {
//		// select updates the fd_set it receives, so we always use a copy and retain the original.
//		listen_fds = all_fds;
//		int nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
//		if (nready == -1) {
//			perror("server: select");
//			exit(1);
//		}
//		
//		// Is it the original socket? Create a new connection ...
//		if (FD_ISSET(sock_fd, &listen_fds)) {
			int client_fd = accept_connection(sock_fd);
//			if (client_fd > max_fd) {
//				max_fd = client_fd;
//			}
//			FD_SET(client_fd, &all_fds);
			printf("Accepted connection\n");
			fprintf(stderr, "path: %s\n", server_generate_copy_root(client_fd));
			
//		}
	}
}

/* Accept a connection. Note that a new file descriptor is created for
 * communication with the client. The initial socket descriptor is used
 * to accept connections, but the new socket is used to communicate.
 * Return the new client's file descriptor.
 */
int accept_connection(int fd) {
	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0) {
		perror("server: accept");
		close(fd);
		exit(1);
	}
	return client_fd;
}

/*
 * Extract the basename part in the path fname and return that name.
 * Note: reuse part of my A3 helper function extract_name
 *
 * Precondition: fname is a valid path
 */
char *extract_name(char *fname) {
	
	// If fname does not have slash, itself is already a basename
	char *first_occur = strchr(fname, '/');
	if (first_occur == NULL) {
		return str_copy(fname);
	}
	
	// Deal with the path that has slash(es)
	char *result = NULL;
	char *temp = str_copy(fname);
	
	// Get rid of trailing slash(es) except when it occurs also as the
	// first character in the path
	int j = strlen(fname) - 1;
	while (j > 0 && temp[j] == '/') {
		temp[j] = '\0';
		j--;
	}
	
	// Mark the start of the basename part in path temp
	char *name_start = strrchr(temp, '/');
	if (name_start == NULL) {
		name_start = temp;
	} else if (name_start[1] != '\0') {
		name_start += 1;
	}
	result = str_copy(name_start);
	
	// Free memory of no further use and return result
	free(temp);
	return result;
}

/*
 * Return a copy of given source string src.
 * Note: reuse part of my A3 helper function str_copy
 */
char *str_copy(char* src) {
	int length = strlen(src);
	char *result = malloc(sizeof(char) * (length + 1));
	memcpy(result, src, length);
	result[length] = '\0';
	return result;
}

/*
 * Return the path for a child directory or file using the path of its parent
 * directory and the name of that child directory/ file.
 *
 * Precondition: fname is a valid path representing a directory
 * Note: reuse A3 generate_path function
 */
char *generate_path(char *fname, char *c_name) {
	
	// Allocate memory for path
	int p_size = sizeof(char) * (strlen(fname) + strlen(c_name) + 2);
	char *path = malloc(p_size);
	
	// Make path a string so that we can use string operations
	path[0] = '\0';
	
	// We have allocated enough memory, so we don't need
	// to use strncpy and strncat here
	// And we also deal with the trailing slashes for path fname
	strcpy(path, fname);
	int i = strlen(fname) - 1;
	while (i > 0 && path[i] == '/') {
		path[i] = '\0';
		i--;
	}
	
	// If parent dir is root directory for the OS, i.e. now what is already
	// concatenated to the path is a single /,
	// then we don't need to concatenate slash any more
	if (path[strlen(path) - 1] != '/') {
		strcat(path, "/");
	}
	
	// strcat ensures null terminator and we allocated enough memory
	strcat(path, c_name);
	
	// Return the path
	return path;
}


/*
 * Client writes a single string buf to correponding socket descriptor.
 * Note: this write includes writing the null terminator for a string
 */
void client_write_str(int sock_fd, char *buf) {
	int should_write = strlen(buf) + 1;
	int num_written = write(sock_fd, buf, should_write);
	if (num_written != should_write) {
		perror("client: write");
		close(sock_fd);
		exit(1);
	}
}

/*
 * Return a path for the root file/ or dir to start copy in the server side, 
 * based on the basename read from client.
 */
char *server_generate_copy_root(int sock_fd) {
	char basename[BUF_SIZE + 1];
	fprintf(stderr, "read basename not start");
	int num_read = read(sock_fd, &basename, BUF_SIZE);
	fprintf(stderr, "read basename: %s\n", basename);
	if (num_read == 0) {
		perror("server: read");
		close(sock_fd);
		exit(1);
	}
	char *path = generate_path(CURRENT_WORKING_DIR, basename);
	fprintf(stderr, "path in sgcr: %s\n", path);
	return path;
}

/*
 * Compute the hash for file at path fname with correponding request 
 * at req_ptr.
 */
void compute_hash(char *fname, struct request *req_ptr) {
	
	// Open file
	FILE *f;
	if ((f = fopen(fname, "rb")) == NULL) {
		perror(fname);
		exit(1);
	}
	
	// Compute hash
	char *hash_val = hash(f);
	for (int i = 0; i < BLOCK_SIZE; i++) {
		req_ptr->hash[i] = hash_val[i];
	}
	free(hash_val);
	
	// Close file
	if (fclose(f) != 0) {
		perror(fname);
		exit(1));
	}
}

/*
 * Client writes all fields of request at req_ptr to a socket descriptor.
 */
void client_write_fields(int sock_fd, struct request *req_ptr) {
	
	// Write type
	uint32_t neto_type = htonl(req_ptr->type);
	int num_written_type = write(sock_fd, &neto_type, sizeof(uint32_t));
	if (num_written_type != sizeof(uint32_t)) {
		perror("client: write type");
		close(sock_fd);
		exit(1);
	}
	
	// Write path
	int num_written_path = write(sock_fd, req_ptr->path, MAX_PATH);
	if (num_written_path != MAX_PATH) {
		perror("client: write path");
		close(sock_fd);
		exit(1);
	}
	
	// Write mode
	int num_written_mode = write(sock_fd, &(req_ptr->mode), sizeof(mode_t));
	if (num_written_mode != sizeof(mode_t)) {
		perror("client: write mode");
		close(sock_fd);
		exit(1);
	}
	
	// Write hash
	int num_written_hash = write(sock_fd, req_ptr->hash, BLOCK_SIZE);
	if (num_written_hash != MAX_PATH) {
		perror("client: write path");
		close(sock_fd);
		exit(1);
	}
	
	// Write size
	uint32_t neto_size = htonl(req_ptr->size);
	int num_written_size = write(sock_fd, &neto_size, sizeof(uint32_t));
	if (num_written_size != sizeof(uint32_t)) {
		perror("client: write size");
		close(sock_fd);
		exit(1);
	}
}

/*
 * Server reads all fields of request to req_ptr from a socket descriptor.
 */
void server_read_fields(int client_fd, struct request_received *rreq_ptr) {
	int bytes_read = sizeof(uint32_t) * 2 + MAX_PATH + sizeof(mode_t) + BLOCK_SIZE;
	void buf[bytes_read];
	fprintf(stderr, "read fields not start");
	int num_read = read(client_fd, &buf, bytes_read);
	fprintf(stderr, "read fields");
	if (num_read == 0) {
		perror("server: read");
		close(sock_fd);
		exit(1);
	}
	
	struct request_received rreq = {.state =  AWAITING_TYPE};
	struct request_received *rreq_ptr = &rreq;
	switch (rreq_ptr->state) {
  		case AWAITING_TYPE:
			
			break;
			
		case AWAITING_PATH:
			
			break;
			
		case AWAITING_PERM:
			
			break;
			
		case AWAITING_HASH:
			
			break;
			
		case AWAITING_SIZE:
			
			break;
			
		case
			
		default: // should not get here
			break;
	}
	
}
