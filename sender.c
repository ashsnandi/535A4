// sender.c


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "multicast.h"
// shared header for shared defs?
#include "shared_structs.h"

struct chunked_file {
    char *filename;
    int file_id;
    char **chunks; //chunk pointers array
    int *chunk_sizes; //chunk sizes
    int total_chunks;
    uint32_t *chunk_checksums;
    uint32_t file_checksum; //this file's checksum
};

// helper: open one file and split into chunks
chunk_a_file(char *filename, int file_id, int chunk_size) {

    //initialize file checksum
    uint32_t file_checksum = 0;

    // Open the file
    FILE *f = fopen(filename, "rb");

    // figure out file size


    // Figure out total chunks needed for memory allocation
        // Initialize total_chunks based on file size and chunk_size
        // likley need to mirror chunked_file struct and initialize chunks, chunk_sizes, and chunk_checksums arrays here as well

    // Read chunks into memory, get checksum, and store in arrays
    for (int i = 0; i < total_chunks; i++) {
        // read chunk into memory

        // Add chunk checksum to file checksum (simple aggregate)
        file_checksum += chunk_checksums[i]; // simple aggregate checksum
    }
    fclose(f);

    // store in files array

}

uint32_t compute_chunk_checksum(char *givenChunk, int size) {
    uint32_t sum = 0;
    for (int i = 0; i < size; i++) {
        sum += (unsigned char)givenChunk[i];
    }
    return sum;
}

// helper for sending a chunk

// helper for sending a metadata packet


// Need to implement statistics related stuff as well. Doing after core functionality is working



int main(int argc, char *argv[]) {

    int file_count = argc - 1; // assuming all args are files for now, adjust if adding options
    chunked_file *files = malloc(file_count * sizeof(chunked_file));

    // initialize multicast sender
    // This is example code right here
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000); 

    // load all files into memory as chunks

    while (1) {
        // Send all metadata

        // Send all chunks from all files cyclically for now (can optimize later)

        // Update stats
    }

    // free chunk memory
    // free files array
    return 0;
}


/*
Tasks

A: Sender Program (sender.c)

- Read files from command-line arguments
  (e.g., ./sender train.csv descr.txt)

- Chunking:
  * Split files into chunks.
    - Chunk size must be configurable via command-line argument
      (e.g., ./sender -c 1024 train.csv)
    - Default chunk size: 1024 bytes if not specified

  * Add headers with metadata:
    - sequence number
    - file ID
    - checksum
    - total chunks

- Transmission:
  * Send chunks to the multicast group

  * Implement reliability:
    - retransmit chunks cyclically OR
    - retransmit upon receiver requests

- Track statistics
*/

