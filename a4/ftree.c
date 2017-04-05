#include <stdio.h>
// Add your system includes here.
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

#define BLOCKSIZE 8

// Client sturct
struct client {
	int fd;
	int status;
	FILE* fp;
	struct request req;
	struct client *next;
};

// General helpers.
int file_size(char* filename);
char* file_hash(char* filename);
char* build_path(char* path, char* name);
char* extract_path(char* fname);
int check_existence(char* path);
int is_same_file(char* name1, char* name2);
void file_overwrite(char* src, char* dest);

// Helpers for rcopy_client.
void handle_sendfile(struct request *info, char *source, char *host, unsigned short port);
void write_to_server(struct request *info, int sock_fd);
int rcopy_client_impl(char *source, char *host, unsigned short port, int sock_fd);

// Helpers for rcopy_server. Mostly based on professor's lecture code: simpleselect.c
int bindandlisten(int port);
static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
int handleclient(struct client *p, struct client *top);
void handleclient_dir(struct client* p);
void handleclient_file(struct client* p);
void handleclient_tfile(struct client* p);
int handleclient_data(struct client* p);
void build_relative_path(char *new_path, char *prefix, char *source);


void build_relative_path(char *new_path, char *prefix, char *source){
	 int index = strlen(prefix);
    strncpy(new_path, ".", 1);
    new_path[1] = '\0';
    int i = 1;
    while(source[index] != '\0') {
    	new_path[i] = source[index];
    	i++;
    	index++;
    }
    new_path[i]= '\0';
    printf("relative path: %s\n", new_path);
	
}
//==========Client======================================================================================================================

// Client main function, source should be a path.
int rcopy_client(char *source, char *host, unsigned short port){

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

	int result = rcopy_client_impl(source, host, port, sock_fd);
	close(sock_fd);
	return result;
}

// Client implement function, handle recursion cases.
int rcopy_client_impl(char *source, char *host, unsigned short port, int sock_fd) {
	// Get infomations form source.
	struct stat src_info;
	int src_status = lstat(source, &src_info);

	// Check existence.
	if (src_status < 0) {
		perror("lstat");
		exit(1);
	}
	
	static int has_prefix = 0;
   static char prefix_dir[MAXPATH];
   if(has_prefix == 0){
    	  char new_source[MAXPATH];
    	  strncpy(new_source, source, strlen(source)+1);
        char *dir_name = dirname(new_source);
        printf("dir name is %s\n", dir_name);
        strncpy(prefix_dir, dir_name, strlen(dir_name)+1);
        prefix_dir[strlen(dir_name)] = '\0';
        printf("prefix_dir is %s\n", prefix_dir);
        has_prefix = 1;
   }
   char relative_path[MAXPATH];
   build_relative_path(relative_path, prefix_dir, source);

	// Init info, which is request tpye to store all infomations we need
	// from a file or a directory.
	struct request *info = malloc(sizeof(struct request));

	// Init some general fields because both file and directory have them.
	//strcpy(info->path, source);
	strcpy(info->path, relative_path);
	info->mode = src_info.st_mode;

	// If source is a file.
	if (S_ISREG(src_info.st_mode)) {

		// Init some fields for file type only.
		info->type = REGFILE;
		strcpy(info->hash, file_hash(source));
		info->size = file_size(source);

		// Write infomations to server.
		write_to_server(info, sock_fd);

		// check request status
		int request_check;

		// get info from server
		int num_read = read(sock_fd, &request_check, sizeof(int));
		fprintf(stderr, "number of read from server: %d\n", num_read);
		fprintf(stderr, "request check from server: %d\n", request_check);

		// if nothing to read
		if (num_read == 0) {
			exit(1);
		}

		switch (request_check) {
		case ERROR:
			perror("rcopy_client : request");
			exit(1);
			break;
		case OK: break;
		case SENDFILE:
			info->type = TRANSFILE;
			handle_sendfile(info, source, host, port);
			break;
		}

	// If source is a directory. Need recursion.
	} else if (S_ISDIR(src_info.st_mode)) {

		// Init some fileds for directory type only.
		info->type = REGDIR;
		info->size = 0;

		// Write infomations to server.
		write_to_server(info, sock_fd);

		struct dirent *dp;
		DIR * dirp = opendir(source);

		// Check if src cannot open.
		if (dirp == NULL) {
			perror("opendir");
			exit(1);
		}

		// Go over all files in src.
		for (dp = readdir(dirp); dp != NULL; dp = readdir(dirp)) {
			if (dp->d_name[0] == '.') {
				continue;
			}
			char* path = build_path(source, dp->d_name);
			rcopy_client_impl(path, host, port, sock_fd);
			free(path);
		}
	}
	free(info);
	return 0;
}

// A helper for writing to server.
void write_to_server(struct request *info, int sock_fd){
	int tmp = htonl(info->type);
	if (write(sock_fd, &tmp, sizeof(int)) != sizeof(int)) {
		perror("write: type");
		exit(1);
	}

	if (write(sock_fd, info->path, MAXPATH) != MAXPATH) {
		perror("write: path");
		exit(1);
	}

	if (write(sock_fd, &info->mode, sizeof(mode_t)) != sizeof(mode_t)) {
		perror("write: mode");
		exit(1);
	}

	if (write(sock_fd, info->hash, BLOCKSIZE) != BLOCKSIZE) {
		perror("write: hash");
		exit(1);
	}

	tmp = htonl(info->size);
	if (write(sock_fd, &tmp, sizeof(int)) != sizeof(int)) {
		perror("write: size");
		exit(1);
	}
}

// A helper for SENDFILE case.
void handle_sendfile(struct request *info, char *source, char *host, unsigned short port) {
	int result = fork();
	if (result < 0) {
		perror("fork");
		exit(1);
	}
	// parent process, wait
	else if (result > 0){
		int status;
		// check error, wait child process
		if (wait(&status) == -1) {
			perror("wait");
			exit(1);
		}
	}

	// child process, transfer file.
	else if(result == 0){
		// new connection as another client with the server to transfer file
		int sock_fd_child = socket(AF_INET, SOCK_STREAM, 0);
		if (sock_fd_child < 0) {
			perror("client: socket");
			exit(1);
		}
		// Set the IP and port of the server to connect to.
		struct sockaddr_in server;
		server.sin_family = AF_INET;
		server.sin_port = htons(port);

		if (inet_pton(AF_INET, host, &server.sin_addr) < 1) {
			perror("client: inet_pton");
			close(sock_fd_child);
			exit(1);
		}
		// Connect to the server.
		if (connect(sock_fd_child, (struct sockaddr *)&server, sizeof(server)) == -1) {
			perror("client: connect");
			close(sock_fd_child);
			exit(1);
		}

		write_to_server(info, sock_fd_child);

		FILE* fp = fopen(source, "r");
		char buf[MAXDATA];
		
		// MAXDATA error exists
		int num_read;
		while((num_read = fread(buf,1,MAXDATA,fp)) > 0) {
			if (write(sock_fd_child, buf, num_read) != num_read) {
				perror("write");
				exit(1);
			}
		}
		close(sock_fd_child);
	}
}

//==========Server======================================================================================================================

// Server main function.
void rcopy_server(unsigned short port) {
	int clientfd, maxfd, nready;
	struct client *p;
	struct client *head = NULL;
	socklen_t len;
	struct sockaddr_in q;
	fd_set allset;
	fd_set rset;

	int i;


	int listenfd = bindandlisten(port);
	// initialize allset and add listenfd to the
	// set of file descriptors passed into select
	FD_ZERO(&allset);
	FD_SET(listenfd, &allset);
	// maxfd identifies how far into the set to search
	maxfd = listenfd;

	while (1) {
		// make a copy of the set before we pass it into select
		rset = allset;

		nready = select(maxfd + 1, &rset, NULL, NULL, NULL);

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

// A helper to handle infomations from client.
int handleclient(struct client *p, struct client *top) {
	switch (p->status) {
	case AWAITING_TYPE: {
		int request_read = read(p->fd, &p->req.type, sizeof(int));
		p->req.type = ntohl(p->req.type);
		if (request_read == 0) {
			return -1;
		}
		if (request_read != sizeof(int)){
			perror("rcopy_server: read client type");
			return -1;
		}
		p->status = AWAITING_PATH;
	} break;
	case AWAITING_PATH: {
		if (read(p->fd, p->req.path, MAXPATH) != MAXPATH){
			perror("rcopy_server: read client path");
			return -1;
		}
		p->status = AWAITING_PERM;
	} break;
	case AWAITING_PERM: {
		if (read(p->fd, &p->req.mode, sizeof(mode_t)) != sizeof(mode_t)){
			perror("rcopy_server: read client mode");
			return -1;
		}
		p->status = AWAITING_HASH;
	} break;
	case AWAITING_HASH: {
		if (read(p->fd, p->req.hash, BLOCKSIZE) != BLOCKSIZE){
			perror("rcopy_server: read client hash");
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
			handleclient_file(p);
			p->status = AWAITING_TYPE;
		} break;
		case REGDIR: {
			handleclient_dir(p);
			p->status = AWAITING_TYPE;
		} break;
		case TRANSFILE: {
			handleclient_tfile(p);
			p->status = AWAITING_DATA;
		} break;
		}
	} break;
	case AWAITING_DATA: {
		return handleclient_data(p);
	} break;
	}
	return 0;
}

// A helper to handle REGFILE request from server to client.
void handleclient_file(struct client* p) {
	int check_point1 = check_existence(p->req.path);
	int check_point2 = 1;
	if (check_point1 != 0){
		FILE *fp = fopen(p->req.path,"r");
		char* local_hash = hash(fp);
		int local_size = file_size(p->req.path);
		//int check_hash_point = strcmp(p->req.hash, local_hash);
		int check_hash_point = check_hash(p->req.hash, local_hash);
		if ((check_hash_point != 0) || (local_size != p->req.size)){
			check_point2 = 0;
		}
	}

	if (check_point1 == 0 || check_point2 == 0){
		int request_to_client = SENDFILE;
		if (write(p->fd, &request_to_client, sizeof(int)) != sizeof(int)) {
			perror("rcopy_server: write request_to_client");
			exit(1);
		}
	} else {
		int request_to_client = OK;
		if (write(p->fd, &request_to_client, sizeof(int)) != sizeof(int)) {
			perror("rcopy_server: write request_to_client");
			exit(1);
		}
	}
}

// A helper to handle REGDIR request from server to client.
void handleclient_dir(struct client* p) {
	char* dest_dir = p->req.path;

	struct stat src_info;
	int src_existence = lstat(dest_dir, &src_info);
	DIR * dirp;

	// Check existence.
	if (src_existence < 0) {
		int permissions = (p->req.mode)&(0777);
		mkdir(dest_dir, permissions);
		dirp = opendir(dest_dir);
	} else {
		dirp = opendir(dest_dir);
	}

	// Check if src cannot open.
	if (dirp == NULL) {
		perror("opendir");
		exit(1);
	}
	closedir(dirp);
}

// A helper to handle TRANSFILE request from server to client.
void handleclient_tfile(struct client* p) {
	p->fp = fopen(p->req.path, "w");
}

// A helper to handle date from client.
int handleclient_data(struct client* p) {
	char buf[MAXDATA];
	int read_count = read(p->fd,buf,MAXDATA);

	//first time read
	if (read_count < 0){
		perror("rcopy_server: read data");
		fclose(p->fp);
		exit(1);
	}

	if (read_count == 0) {
		fclose(p->fp);
		return -1;
	}

	if (fwrite(buf, 1, read_count, p->fp) != read_count) {
		perror("fwrite");
		exit(-1);
	}
	return 0;
}

// Bind and listen, abort on errorreturns FD of listening socket.
int bindandlisten(int port) {
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
	r.sin_port = htons(port);

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

// A helper to a new client to server.
static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
	struct client *p = malloc(sizeof(struct client));
	if (!p) {
		perror("malloc");
		exit(1);
	}

	printf("Adding client %s\n", inet_ntoa(addr));

	p->fd = fd;
	p->status = AWAITING_TYPE;
	p->fp = NULL;
	p->next = top;
	top = p;
	return top;
}

// A helper to remove exist client from server.
static struct client *removeclient(struct client *top, int fd) {
	struct client **p;

	for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next) {}
	// Now, p points to (1) top, or (2) a pointer to another client
	// This avoids a special case for removing the head of the list
	if (*p) {
		struct client *t = (*p)->next;
		free(*p);
		*p = t;
	} else {
		fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
				fd);
	}
	return top;
}

//=======General helpers===============================================================================================================

// A helper for getting a file name from a path. Like A2.
char* extract_path(char* fname) {
	char* last_slash = strrchr(fname, '/');
	return last_slash ? last_slash + 1 : fname;
}

// A helper to get the size of a file.
int file_size(char* filename) {
	struct stat buf;
	stat(filename, &buf);
	int size = buf.st_size;

	return size;
}

// A helper to get the hash value of a file.
char* file_hash(char* filename) {

	FILE * file = fopen(filename, "r");

	// Check if file cannot open.
	if (file == NULL) {
		printf("cannot open");
		perror("openfile");
		exit(1);
	}

	char* result = hash(file);
	fclose(file);

	return result;
}

// A helper to build a path for the name. Like A2.
char* build_path(char* path, char* name) {

	char* path_name = malloc(strlen(path) + strlen(name) + 2);
	strcpy(path_name, path);
	strcat(path_name, "/");
	strcat(path_name, name);
	strcat(path_name, "\0");

	return path_name;
}

// A helper to check if this file exist.
int check_existence(char* path) {

	// Get infomations form input.
	struct stat info;
	int status = lstat(path, &info);

	// Check existence.
	if (status < 0) {
		return 0;
	}
	return 1;
}

// A helper to check if two files are the same.
int is_same_file(char* name1, char* name2) {

	// If one of these 2 is not exist, they are definitly not same.
	if (!check_existence(name1) || !check_existence(name2)) {
		return 0;
	}

	int same_size_flag = (file_size(name1) == file_size(name2));
	int same_hash_flag = (file_hash(name1) == file_hash(name2));

	// They are same iff their sizes and hash values are same.
	return same_size_flag && same_hash_flag;
}

// A helper to overwrite one file to another.
void file_overwrite(char* src, char* dest) {

	FILE * src_file = fopen(src, "r");
	FILE * dest_file = fopen(dest, "w");

	// Check if file cannot open.
	if (src_file == NULL || dest_file == NULL) {
		perror("openfile");
		exit(1);
	}

	char temp;
	int length = file_size(src);
	int i = 0;

	// Read from src and write to dest, one by one.
	for (; i < length; i++) {
		fread(&temp, 1, 1, src_file);
		fwrite(&temp, 1, 1, dest_file);
	}

	fclose(src_file);
	fclose(dest_file);
}
