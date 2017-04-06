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

#define CURRENT_WORKING_DIR "./"

#if 0
#define EPRINTF(...) ((void)0)
#else
#define EPRINTF(...) do{\
	fprintf(stderr, "[[ %d ]] ", __LINE__); \
	fprintf(stderr, __VA_ARGS__);\
	}while(0)
#endif

/*
 * Structure for client side file.
 */
struct client {
	int fd; // corresponding socket descriptor
	struct request req; // request from client side
	struct in_addr ipaddr; // ip address
	int status; // status for processing this client
	struct client *next; // the next client struct in the server linked list
	FILE* fp; // correponding server side file open for writing
};

int accept_connection(int fd);
char *extract_name(char *fname);
char *str_copy(char* src);
char *generate_path(char *fname, char *c_name);
// void client_write_str(int sock_fd, char *buf);
// char *server_generate_copy_root(int sock_fd);
char *compute_hash(char *fname);
void client_write_fields(int sock_fd, struct request *req_ptr);
void server_read_fields(int client_fd, struct request *rreq_ptr);
void sendfile(char *source, char *host, unsigned short port,
		struct request *info);
void load_hash_request(char *hash_val, struct request *req_ptr);
int rcopy_client_body(char *source, char* host, unsigned short port,
		int sock_fd);

int bindandlisten(void);
static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
int handleclient(struct client *p, struct client *top);
void check_REGFILE(struct client *p);
int need_send_file(struct client *p);
void check_REGDIR(struct client *p);
int process_data(struct client *p);

/*
 * Set up socket connection.
 * Note: reuse part of lecture codes
 */
int socket_connect(char *host, unsigned short port) {
	int sock_fd;
	struct hostent *hp;
	struct sockaddr_in peer;

	peer.sin_family = AF_INET;
	peer.sin_port = htons(port);

	/* fill in peer address */
	hp = gethostbyname(host);
	if (hp == NULL) {
		fprintf(stderr, "%s: unknown host\n", host);
		exit(1);
	}

	peer.sin_addr = *((struct in_addr *) hp->h_addr);

	/* create socket */
	if ((sock_fd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
	}
	/* request connection to server */
	if (connect(sock_fd, (struct sockaddr *) &peer, sizeof(peer)) == -1) {
		perror("client:connect");
		close(sock_fd);
		exit(1);
	}
	return sock_fd;
}

/* 
 * Client side for the rcopy program.
 */
int rcopy_client(char *source, char *host, unsigned short port) {

	int sock_fd = socket_connect(host, port);

	char *basename = extract_name(source);
	int r = rcopy_client_body(basename, host, port, sock_fd);
	close(sock_fd);
	return r;
}

/* 
 * The implementation of rcopy_client in order to use recursion.
 */
int rcopy_client_body(char *source, char* host, unsigned short port,
		int sock_fd) {

	//source information.
	struct stat src_info;
	int src_status = lstat(source, &src_info);

	//if not exist, should exit
	if (src_status < 0) {
		perror("lstat");
		exit(1);
	}

	struct request *info = malloc(sizeof(struct request));
	strcpy(info->path, source);
	info->mode = src_info.st_mode;

	// Recursive case:If source is a directory
	if (S_ISDIR(src_info.st_mode)) {
		info->size = 0;
		info->type = REGDIR;

		for (int i = 0; i < BLOCK_SIZE; i++) {
			info->hash[i] = '\0';
		}

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
				char* path = generate_path(info->path, dp->d_name);
				rcopy_client_body(path, host, port, sock_fd);
				free(path);
			}
			dp = readdir(dirp);
		}

		// Base case: If source is a file.
	} else if (S_ISREG(src_info.st_mode)) {

		info->size = src_info.st_size;
		info->type = REGFILE;
		memcpy(info->hash, compute_hash(source), BLOCK_SIZE);
		client_write_fields(sock_fd, info);

		int request_status;
		int num_read = read(sock_fd, &request_status, sizeof(int));
		if (num_read == 0 || num_read != sizeof(int)) {
			perror("client: read");
			exit(1);
		}

		switch (ntohl(request_status)) {
		case ERROR: {
			perror("request");
		}
			break;
		case OK:
			break;
		case SENDFILE: {
			info->type = TRANSFILE;
			sendfile(source, host, port, info);
		}
			break;
		}
	}
	free(info);
	return 0;

}

/*
 * Send the information about file at source to a pointer to request.
 */
void sendfile(char *source, char *host, unsigned short port,
		struct request *info) {
	int result = fork();
	if (result < 0) {
		perror("fork");
		exit(1);
	}

	// child process: we need to transfer the file.
	else if (result == 0) {
		int child_socket = socket_connect(host, port);
		EPRINTF("%s\n", source);
		client_write_fields(child_socket, info);

		FILE* fp = fopen(source, "r");

		// read the fp to buf.
		char buf[MAXDATA];
		int num_read;
		while ((num_read = fread(buf, 1, MAXDATA, fp)) > 0) {
			if (write(child_socket, buf, num_read) != num_read) {
				perror("write");
				exit(1);
			}

		}
		close(child_socket);

		// Parent process: wait for child
	} else if (result > 0) {
		int status;
		if (wait(&status) == -1) {
			perror("wait");
			exit(1);
		}
	}
}

/*
 * Compute the hash for file at path fname.
 * Precondition: fname is a valid path for a file.
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

	// Close file
	if (fclose(f) != 0) {
		perror(fname);
		exit(1);
	}
	return hash_val;
}

/* 
 * Load the hash_val to struct req_ptr.
 */
void load_hash_request(char *hash_val, struct request *req_ptr) {
	memcpy(req_ptr->hash, hash_val, BLOCK_SIZE);
}

/*
 * Client writes all fields of request at req_ptr to a socket descriptor.
 */
void client_write_fields(int sock_fd, struct request *req_ptr) {
	EPRINTF("\n");
	// Write type
	int neto_type = htonl(req_ptr->type);
	int num_written_type = write(sock_fd, &neto_type, sizeof(int));
	if (num_written_type != sizeof(int)) {
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
	if (num_written_hash != BLOCK_SIZE) {
		perror("client: write hash");
		close(sock_fd);
		exit(1);
	}

	// Write size
	int neto_size = htonl(req_ptr->size);
	int num_written_size = write(sock_fd, &neto_size, sizeof(int));
	if (num_written_size != sizeof(int)) {
		perror("client: write size");
		close(sock_fd);
		exit(1);
	}
}

/*
 * Server side at port to do the copy.
 * Note: we reuse some of lecture codes for simpleselect.c
 */
void rcopy_server(unsigned short port) {

	// Basic declaration for settings of further use
	int clientfd, maxfd, nready;
	struct client *p;
	struct client *head = NULL;
	socklen_t len;
	struct sockaddr_in q;
	fd_set allset;
	fd_set rset;
	int listenfd = bindandlisten();
	FD_ZERO(&allset);
	FD_SET(listenfd, &allset);

	// maxfd identifies how far into the set to search
	maxfd = listenfd;

	while (1) {

		// make a copy of the set before we pass it into select
		rset = allset;

		// Do the select and corresponding connection
		nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
		if (nready == -1) {
			perror("select");
			continue;
		}
		if (FD_ISSET(listenfd, &rset)) {
			printf("a new client is connecting\n");
			len = sizeof(q);
			if ((clientfd = accept(listenfd, (struct sockaddr *) &q, &len))
					< 0) {
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

		// Update the linked list of clients and do the copy
		for (p = head; p != NULL; p = p->next) {
			if (FD_ISSET(p->fd, &rset)) {
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

/* Bind and listen, abort on error.
 * returns FD of listening socket
 * Note: we reuse some of lecture codes for simpleselect.c
 */
int bindandlisten(void) {
	struct sockaddr_in r;
	int listenfd;

	if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		exit(1);
	}
	int yes = 1;
	if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)))
			== -1) {
		perror("setsockopt");
	}
	memset(&r, '\0', sizeof(r));
	r.sin_family = AF_INET;
	r.sin_addr.s_addr = INADDR_ANY;
	r.sin_port = htons(PORT);

	if (bind(listenfd, (struct sockaddr *) &r, sizeof r)) {
		perror("bind");
		exit(1);
	}

	if (listen(listenfd, 5)) {
		perror("listen");
		exit(1);
	}
	return listenfd;
}

/*
 * Add a struct client to a linked list of clients, whose head is top.
 */
static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
	struct client *p = malloc(sizeof(struct client));
	if (!p) {
		perror("malloc");
		exit(1);
	}
	printf("Adding client %s\n", inet_ntoa(addr));

	// Initialize fields
	p->fd = fd;
	p->ipaddr = addr;
	p->next = top;
	p->status = AWAITING_TYPE;
	top = p;
	return top;
}

/*
 * Remove a struct client from a linked list of clients, whose head is top.
 */
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
 * A FSM to load info from client for a corresponding file or directory based
 * on how much fields or data we have already read in teh server side.
 * (Our order here to load fields is inconsistent of the order mentioned in 
 * the hhandout)
 */
int handleclient(struct client *p, struct client *top) {
	EPRINTF("%d %d\n", p->fd, p->status);

	// Do the transition based on current loading status
	switch (p->status) {

	// Wait to load type field
	case AWAITING_TYPE: {
		int nbytes = read(p->fd, &(p->req.type), sizeof(int));
		if (nbytes == 0) {
			return -1;
		}
		if (nbytes != sizeof(int)) {
			perror("server: load type");
			return -1;
		}
		p->req.type = ntohl(p->req.type);
		p->status = AWAITING_PATH;
	}
		break;

		// Wait to load path field
	case AWAITING_PATH: {
		if (read(p->fd, p->req.path, MAXPATH) != MAXPATH) {
			perror("server: load path");
			return -1;
		}
		EPRINTF("%.*s\n", MAXPATH, p->req.path);
		p->status = AWAITING_PERM;
	}
		break;

		// wait to load mode field
	case AWAITING_PERM: {
		if (read(p->fd, &p->req.mode, sizeof(mode_t)) != sizeof(mode_t)) {
			perror("server: load mode");
			return -1;
		}
		p->status = AWAITING_HASH;
	}
		break;

		// Wait to load hash field
	case AWAITING_HASH: {
		if (read(p->fd, p->req.hash, BLOCK_SIZE) != BLOCK_SIZE) {
			perror("server: load hash");
			return -1;
		}
		p->status = AWAITING_SIZE;
	}
		break;

		// Wait to load size
	case AWAITING_SIZE: {
		if (read(p->fd, &p->req.size, sizeof(int)) != sizeof(int)) {
			perror("rcopy_server: read client SIZE");
			return -1;
		}
		p->req.size = ntohl(p->req.size);

		switch (p->req.type) {
		case REGFILE: {
			check_REGFILE(p);
			p->status = AWAITING_TYPE;
		}
			break;

		case REGDIR: {
			check_REGDIR(p);
			p->status = AWAITING_TYPE;
		}
			break;

		case TRANSFILE: {
			p->status = AWAITING_DATA;
			if ((p->fp = fopen(p->req.path, "w")) == NULL) {
				perror("server: fopen");
				return -1;
			}
		}
			break;
		}
	}
		break;

		// Wait to do the real copy
	case AWAITING_DATA: {
		return process_data(p);
	}
	default: // should not get here
		break;
	}
	return 0;
}

/*
 * Server side sends response of the request from a regular file.
 */
void check_REGFILE(struct client *p) {

	// Check whether here is a same local copy for the given reg file
	int response;
	int result;
	if ((result = need_send_file(p)) == 0) { // need
		response = SENDFILE;
	} else if (result == 1) { // OK
		response = OK;
	} else {
		response = ERROR;
	}

	// Send response to client
	int net_uint = htonl(response);
	int num_written = write(p->fd, &net_uint, sizeof(int));
	if (num_written != sizeof(int)) {
		perror("server: write response");
		close(p->fd);
		exit(1);
	}
}

/*
 * Return whether we need to send a file, i.e. do the real copy.
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
	if (chmod(p->req.path, p->req.mode & 0777) != 0) {
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

/*
 * Server side handles the request from a regular directory.
 */
void check_REGDIR(struct client *p) {
	EPRINTF("%s %04o\n", p->req.path, p->req.mode);

	//Check existence
	struct stat local_stat;
	printf("before lstat\n");
	int exist = lstat(p->req.path, &local_stat);
	printf("after lstat\n");
	printf("%s\n", p->req.path);

	// Dir does not exist, make a new dir in the local
	if (exist < 0) {
		if (mkdir(p->req.path, p->req.mode) != 0) {
			perror("server: mkdir");
			exit(1);
		}
	}

	// change the mode of existing local copy, regardless whether match or 
	// mismatch with the one at client side
	if (chmod(p->req.path, p->req.mode) != 0) {
		perror("chmod");
		exit(1);
	}
}

/*
 * Transmit data of the client side file to server side.
 */
int process_data(struct client *p) {

	// Read data from client
	char buf[MAXDATA];
	int num_read = read(p->fd, buf, MAXDATA);
	if (num_read == 0) {
		fclose(p->fp);
		return -1;
	}
	if (num_read < 0) {
		perror("server: process data");
		return -1;
	}

	// Write to local copy
	if ((fwrite(buf, sizeof(char), num_read, p->fp)) != num_read) {
		perror("fwrite");
		return -1;
	}

	return 0;
}
