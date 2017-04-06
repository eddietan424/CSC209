#include <stdio.h>
#include <stdlib.h>
#include "hash.h"

#define BLOCK_SIZE 8

/*
 * Return the hash of a regular file or a link pointed by the FILE pointer f.
 * Precondition: f is a valid FILE pointer to some file
 */
char *hash(FILE *f) {
	char *hash_val = malloc(sizeof(char) * BLOCK_SIZE);

	// Initialization for hash_val
	for (int i = 0; i < BLOCK_SIZE; i++) {
		hash_val[i] = '\0';
	}

	// Compute the hash of data provided by given file
	char data = '\0';
	int count = 0;
	while (fread(&data, sizeof(char), 1, f) != 0) {
		if (count >= BLOCK_SIZE) {
			count = 0;
		}
		hash_val[count] = hash_val[count] ^ data;
		count++;
	}

	// Return the result
	return hash_val;
}

int check_hash(const char *hash1, const char *hash2) {
	for (long i = 0; i < BLOCK_SIZE; i++) {
		if (hash1[i] != hash2[i]) {
			printf("Index %ld: %c\n", i, hash1[i]);
			return 1;
		}
	}
	return 0;
}
