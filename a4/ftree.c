#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>    /* Internet domain header */
#include <errno.h>
#include <signal.h>
#include "ftree.h"
#include "hash.h"

#define MAX_BACKLOG 5
#define MAX_CONNECTIONS 12
#define BUF_SIZE 128
#define CURRENT_WORKING_DIR "./"

struct client {
	int fd;
	FILE* fp;
	struct request req;
	struct in_addr ipaddr;
	int status;
	struct client *next;
};

int accept_connection(int fd);
char *extract_name(char *fname);
char *str_copy(char* src);
char *generate_path(char *fname, char *c_name);
void client_write_str(int sock_fd, char *buf);
char *server_generate_copy_root(int sock_fd);
char *compute_hash(char *fname);
void client_write_fields(int sock_fd, struct request *req_ptr);
void server_read_fields(int client_fd, struct request *rreq_ptr);
void sendfile(char *source, char *host, unsigned short port, struct request *info);
void load_hash_request(char *hash_val, struct request *req_ptr);
int rcopy_client_body(char *source, char* host, unsigned short port, int sock_fd);

int bindandlisten(void);
static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
int handleclient(struct client *p, struct client *top);
void check_REGFILE(struct client *p);
int need_send_file(struct client *p);
void check_REGDIR(struct client *p);
int process_data(struct client *p);

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
	int r = rcopy_client_body(source, host, port, sock_fd);
	close(sock_fd);
	return r;
}

/* The body part of rcopy_client in order to use recursion.
 */
int rcopy_client_body(char *source, char* host, unsigned short port, int sock_fd) {
	
	//source information.
	struct stat src_info;
	int src_status = lstat(source, &src_info);
	
	//if not exist, should exit
	if (src_status < 0) {
		perror("lstat");
		exit(1);		
	}
	
	// flag to recognize the prefix.
	static int flag_prefix = 0;
	static char prefix_dir[MAXPATH];
	
	// the case we the prefix.
	if (flag_prefix) {
		// change the flag and 
		flag_prefix = 1;
		char new_source[MAXPATH];
		strncpy(new_source, source, strlen(source) + 1);
		char *directory_name = extract_name(new_source);
		prefix_dir[strlen(directory_name)] = '\0';				
	}
	
	//char *relative_path;
	//relative_path = generate_path(CURRENT_WORKING_DIR, basename);
	
	// allocate memory for request struct.
	char *basename = extract_name(source);
	char *relative_path;
	relative_path = generate_path(CURRENT_WORKING_DIR, basename);
	struct request *info = malloc(sizeof(struct request));
	strcpy(info->path, relative_path);
	
	// Recursive case:If source is a directory
	if (S_ISDIR(src_info.st_mode)) {
		info->size = 0;
		info->type = REGDIR;
		
		// write to server.
		client_write_fields(sock_fd, info);
		
		struct dirent *dp;
		DIR * dirp = opendir(source);
		
		//Check for open.
		if (dirp == NULL) {
			perror("opendir");
			exit(1);
		}
		
		dp = readdir(dirp);
		while (dp != NULL) {
			if (dp->d_name[0] != '.') {
				char* path = generate_path(source, dp->d_name);
				rcopy_client_body(path, host, port, sock_fd);
				free(path);
			}
			dp = readdir(dirp);
		}
	// Base case: If source is a file.
	} else if (S_ISREG(src_info.st_mode)) {
		struct stat buf;
		stat(source, &buf);
		info->size = buf.st_size;
		info->type = REGFILE;
		strcpy(info->hash, compute_hash(source));
		client_write_fields(sock_fd, info);
		
		int request_status;
		int num_read = read(sock_fd, &request_status, sizeof(int));
		if (num_read == 0){
			exit(1);
		}
		
		switch (request_status) {
		case ERROR:
			perror("request");
		case OK: break;
		case SENDFILE:
			info->type = TRANSFILE;
			sendfile(source, host, port, info);
		}
	}
	free(info);
	return 0;

/* 	
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
	if (MAXPATH - 1 > strlen(basename)) {
		n = strlen(basename);
	} else {
		n = MAXPATH - 1;
	}
	//strncpy(req_ptr->path, basename, n);
	//req_ptr->path[n] = '\0';
	//free(basename);
	//if (S_ISREG(src_stat.st_mode)) { // a regular file
		//req_ptr->type = REGFILE;
		//compute_hash(ori_path, req_ptr);
	//} else if (S_ISDIR(src_stat.st_mode)) { // a directory
		//req_ptr->type = REGDIR;
		//for (int i = 0; i < BLOCK_SIZE; i++) {
			//req_ptr->hash[i] = '\0';
		//}
	//} else {
		//exit(1);
	//}
	
	// Send the request
	client_write_fields(sock_fd, req_ptr);
	
	//while (1) {
	//}
	
	close(sock_fd);
	//return 0; */
}

void sendfile(char *source, char *host, unsigned short port, struct request *info) {
	int result = fork();
	if (result < 0) {
		perror("fork");
		exit(1);
	}
	
	// child process: we need to transfer the file.
	else if (result == 0) {
		int child_socket = socket(AF_INET, SOCK_STREAM, 0);
		
		// error check.
		if (child_socket < 0) {
			perror("socket");
			exit(1);
		}
		
		struct sockaddr_in server;
		server.sin_family = AF_INET;
		server.sin_port = htons(port);
		
		if (inet_pton(AF_INET, host, &server.sin_addr) < 1) {
			perror("client: inet_pton");
			close(child_socket);
		} 
		
		if (connect(child_socket, (struct sockaddr *)&server, sizeof(server)) < 0) {
			perror("client: connect");
			close(child_socket);
			exit(1);
		}
		
		client_write_fields(child_socket, info);
		
		FILE* fp = fopen(source, "r");
		char buf[MAXDATA];
		int num_read;
		while ((num_read = fread(buf, 1, MAXDATA, fp)) < 0) {
			if (write(child_socket, buf, num_read) != num_read) {
				perror("write");
				exit(1);
			}
		
		}
		close(child_socket);
	} else if (result > 0) {
		int status;
		if (wait(&status) == -1) {
			perror("wait");
			exit(1);
		}
	
	
	}
}

/*
 * Compute the hash for file at path fname with correponding request 
 * at req_ptr.
 */
char* compute_hash(char *fname) {
	
	// Open file
	FILE *f;
	if ((f = fopen(fname, "rb")) == NULL) {
		perror(fname);
		exit(1);
	}
	
	// Compute hash
	char *hash_val = hash(f);
	//for (int i = 0; i < BLOCK_SIZE; i++) {
	//	req_ptr->hash[i] = hash_val[i];
	//}
	//free(hash_val);
	
	// Close file
	if (fclose(f) != 0) {
		perror(fname);
		exit(1);
	}
	return hash_val;
}
/* 
 */

void load_hash_request(char *hash_val, struct request *req_ptr) {
	for (int i = 0; i < BLOCK_SIZE; i++) {
		req_ptr->hash[i] = hash_val[i];
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
	int num_written_path = write(sock_fd, req_ptr->path, MAXPATH);
	if (num_written_path != MAXPATH) {
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
	if (num_written_hash != MAXPATH) {
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

void rcopy_server(unsigned short port) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct client *head = NULL;
    socklen_t len;
    struct sockaddr_in q;
    struct timeval tv;
    fd_set allset;
    fd_set rset;

    int i;

    int listenfd = bindandlisten();
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        /* timeout in seconds (You may not need to use a timeout for
        * your assignment)*/
        tv.tv_sec = 10;
        tv.tv_usec = 0;  /* and microseconds */

        nready = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (nready == 0) {
            printf("No response from clients in %ld seconds\n", tv.tv_sec);
            continue;
        }
        printf("Time says %ld seconds\n", tv.tv_sec);

        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("a new client is connecting\n");
            len = sizeof(q);
            if ((clientfd = accept(listenfd, (struct sockaddr *)&q, &len)) < 0) {
                perror("accept");
                exit(1);
            }
            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("connection from %s\n", inet_ntoa(q.sin_addr));
            head = addclient(head, clientfd, q.sin_addr);
        }

        for(i = 0; i <= maxfd; i++) {
            if (FD_ISSET(i, &rset)) {
                for (p = head; p != NULL; p = p->next) {
                    if (p->fd == i) {
                        int result = handleclient(p, head);
                        if (result == -1) {
                            int tmp_fd = p->fd;
                            head = removeclient(head, p->fd);
                            FD_CLR(tmp_fd, &allset);
                            close(tmp_fd);
                        }
                        break;
                    }
                }
            }
        }
    }
}

 /* bind and listen, abort on error
  * returns FD of listening socket
  */
int bindandlisten(void) {
    struct sockaddr_in r;
    int listenfd;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
        perror("setsockopt");
    }
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(PORT);

    if (bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}

static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));
    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->next = top;
	p->fp = NULL;
    top = p;
    return top;
}

static struct client *removeclient(struct client *top, int fd) {
    struct client **p;

    for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
    return top;
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
 * Server loads all fields.
 */
int handleclient(struct client *p, struct client *top) {
	switch (p->status) {
	case AWAITING_TYPE: {
		if (read(p->fd, &(p->req.type), sizeof(int)) != sizeof(int)){
			perror("server: load type");
			return -1;
		}
		p->req.type = ntohl(p->req.type);
		p->status = AWAITING_PATH;
	} break;
	case AWAITING_PATH: {
		if (read(p->fd, p->req.path, MAXPATH) != MAXPATH){
			perror("server: load path");
			return -1;
		}
		p->status = AWAITING_PERM;
	} break;
	case AWAITING_PERM: {
		if (read(p->fd, &p->req.mode, sizeof(mode_t)) != sizeof(mode_t)){
			perror("server: load mode");
			return -1;
		}
		p->status = AWAITING_HASH;
	} break;
	case AWAITING_HASH: {
		if (read(p->fd, p->req.hash, BLOCK_SIZE) != BLOCK_SIZE){
			perror("server: load hash");
			return -1;
		}
		p->status = AWAITING_SIZE;
	} break;
	case AWAITING_SIZE: {
		if (read(p->fd, &p->req.size, sizeof(int)) != sizeof(int)){
			perror("rcopy_server: read client SIZE");
			return -1;
		}
		p->req.size = ntohl(p->req.size);
		switch (p->req.type) {
		case REGFILE: {
			check_REGFILE(p);
			p->status = AWAITING_TYPE;
		} break;
		case REGDIR: {
			check_REGDIR(p);
			p->status = AWAITING_TYPE;
		} break;
		case TRANSFILE: {
			p->status = AWAITING_DATA;
		} break;
		}
	} break;
	case AWAITING_DATA: {
		return process_data(p);
	} default: // should not get here
		break;
	}
	return 0;
}

void check_REGFILE(struct client *p) {
	int response;
	int result;
	if ((result = need_send_file(p)) == 0) { // need
		response = SENDFILE;
	} else if (result == 1){ // OK
		response = OK;
	} else {
		response = ERROR;
	}
	
	// Send response to client
	uint32_t net_uint = htonl(response);
	int num_written = write(p->fd, &net_uint, sizeof(uint32_t));
	if (num_written != sizeof(uint32_t)) {
		perror("server: write response");
		close(p->fd);
		exit(1);
	}
}

/*
 * 0 for need, 1 for not, -1 for error
 */
int need_send_file(struct client *p) {
	
	// Check existence
	struct stat local_stat;
	int exist = lstat(p->req.path, &local_stat);
	if (exist < 0) {
		return 0;
	}
	
	// Change permission
	if (chmod(p->req.path, p->req.mode) != 0) {
		perror("chmod");
		exit(1);
	}
	
	// Check whether incompatible type
	if ((S_ISREG(local_stat.st_mode) && p->req.type == REGDIR)
		|| (S_ISDIR(local_stat.st_mode) && p->req.type == REGFILE)) {
		fprintf(stderr, "server: find incompatible type");
		return -1;
	}
	
	// Already exists, check whether same
	// compare size
	if (local_stat.st_size != p->req.size) {
		return 0;
	}
	
	// compare hash
	char *local_hash = compute_hash(p->req.path);
	if ((check_hash(local_hash, p->req.hash)) == 1) {
		return 0;
	}
	
	// Exist and same, no need to copy
	return 1;
}

void check_REGDIR(struct client *p) {
	
	// Check existence
	struct stat local_stat;
	int exist = lstat(p->req.path, &local_stat);
	if (exist >= 0) {
		if (chmod(p->req.path, p->req.mode) != 0) {
			perror("chmod");
			exit(1);
		}
		return;
	}
	
	if ((mkdir(p->req.path, p->req.mode)) != 0) {
		perror("server: mkdir");
		exit(1);
	}
}

int process_data(struct client *p) {
	if (p->req.size == 0) {
		return 0;
	}
	
	char buf[MAXDATA];
	int num_read;
	if ((num_read = read(p->fd, buf, MAXDATA)) <= 0) {
		perror("server: process data");
		return -1;
	}

	if ((fwrite(buf, sizeof(char), num_read, p->fp)) != num_read) {
		perror("fwrite");
		return -1;
	}
	
	return 0;
}
