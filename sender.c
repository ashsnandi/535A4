// sender.c


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "multicast.h"
// shared header for shared defs?
#include "shared_structs.h"

// helper: open one file and split into chunks
// Note that this includes no error checks as of now
struct chunked_file chunk_a_file(char *filename, int file_id, int chunk_size) {

    //initialize file checksum
    uint32_t file_checksum = 0;

    // Open the file
    FILE *file = fopen(filename, "rb");

    // Check if file opened successfully
    if (file == NULL) {
        fprintf(stderr, "File open failed!\n");
        exit(1);
    }

    // figure out file size 
    // Navigate to end of file
    fseek(file, 0, SEEK_END);
    // ftell get's current position which is size/end of file in our case
    long file_size = ftell(file);
    //convert file size to uint32_t to be consistent with our structs
    file_size = (uint32_t)file_size;
    // Return to beginning of file for reading
    rewind(file);

    // Check if file size is zero
    if (file_size == 0) {
        fprintf(stderr, "File is empty!\n");
        fclose(file);
        exit(1);
    }

    // Figure out total chunks needed for memory allocation
    // get total chunks from file size and chunk size, add 1 if there's a remainder
    int total_chunks = file_size / chunk_size + (file_size % chunk_size != 0);
    // Allocate memory for chunk pointers, chunk sizes, and chunk checksums
    char **chunks = malloc(total_chunks * sizeof(char*));
    int *chunk_sizes = malloc(total_chunks * sizeof(int));
    uint32_t *chunk_checksums = malloc(total_chunks * sizeof(uint32_t));

    // Read chunks into memory, get checksum, and store in arrays
    for (int i = 0; i < total_chunks; i++) {
        // Quick check for last chunk edge case
        int temp_chunk_size;
        if (i == total_chunks - 1) {
            // Last chunk probably smaller so deserves special handling
            temp_chunk_size = file_size - (i * chunk_size);
        }
        else {
            // Otherwise just normal chunk size
            temp_chunk_size = chunk_size; 
        }
        // read chunk into memory
        chunks[i] = malloc(temp_chunk_size);
        fread(chunks[i], sizeof(char), temp_chunk_size, file);
        // store chunk size for later use
        chunk_sizes[i] = temp_chunk_size;

        // Get chunk checksum and store it
        chunk_checksums[i] = compute_chunk_checksum(chunks[i], temp_chunk_size);

        // Add chunk checksum to file checksum (simple aggregate)
        file_checksum += chunk_checksums[i]; // simple aggregate checksum
    }

    // Close because don't need it anymore, all necessary data is stored
    fclose(file);

    // create a chunked file to return with all the info for this file
    struct chunked_file chunkedFile;
    chunkedFile.filename = filename;
    chunkedFile.file_id = file_id;
    chunkedFile.chunks = chunks;
    chunkedFile.chunk_sizes = chunk_sizes;
    chunkedFile.total_chunks = total_chunks;
    chunkedFile.file_size = file_size;
    chunkedFile.chunk_checksums = chunk_checksums;
    chunkedFile.file_checksum = file_checksum;

    return chunkedFile;
}

uint32_t compute_chunk_checksum(char *givenChunk, int size) {
    uint32_t sum = 0;
    for (int i = 0; i < size; i++) {
        // just adds the numeric value of byte to the sum
        sum += (unsigned char)givenChunk[i];
    }
    return sum;
}

// helper for sending a chunk
void send_all_chunks(mcast_t *m, struct chunked_file *files, int file_count) {
    // send chunks from all files sequentially for now (can optimize later using round robin or something)
    for (int i = 0; i < file_count; i++) {
        for (int j = 0; j < files[i].total_chunks; j++) {
            //Create a data packet for this chunk
            int chunk_size = files[i].chunk_sizes[j];
            struct DataPacket dPac;
            dPac.seq_num = j;
            dPac.file_id = files[i].file_id;
            dPac.chunk_checksum = files[i].chunk_checksums[j];
            
            // Actually copy the chunk data into the packet's data field
            memcpy(dPac.data, files[i].chunks[j], chunk_size);
            // send data packet using multicast_send (need to implement this function in multicast.c)

            int packet_size = sizeof(struct DataPacket) + chunk_size;
            multicast_send(m, &dPac, packet_size);
        }
    }
}

// helper for sending a metadata packet
void send_metadata_packet(mcast_t *m, struct chunked_file file) {
    struct MetadataPacket meta;
    meta.file_size = file.file_size;
    meta.file_checksum = file.file_checksum;
    meta.total_chunks = file.total_chunks;
    meta.file_id = file.file_id;

    int packet_size = sizeof(struct MetadataPacket);
    // send metadata packet using multicast_send (need to implement this function in multicast.c)
    multicast_send(m, &meta, packet_size);
}


// Need to implement statistics related stuff as well. Doing after core functionality is working



int main(int argc, char *argv[]) {

    // Input parsing given assignment instructions/example
    // Simple error handling as well
    // Example:  ./sender -c 2048 train.csv descr.txt
    // Or just: ./sender train.csv descr.txt

    int chunk_size = 1024; // default chunk size
    int file_one_idx = 1; // index of first file argument in argv

    // Check for -c without a following chunk size
    if (strcmp(argv[1], "-c") == 0 && argc <= 2) {
        fprintf(stderr, "No chunk size given after -c\n");
        return 1;
    }   

    // Check for optional -c argument
    if (strcmp(argv[1], "-c") == 0) {
        chunk_size = atoi(argv[2]);
        file_one_idx = 3;
    }

    // Checking for any file arguments after optional -c or just in general
    if (argc <= file_one_idx) {
        fprintf(stderr, "Bad input! No files!\n");
        return 1;
    }

    int file_count = argc - file_one_idx;
    struct chunked_file *files = malloc(file_count * sizeof(struct chunked_file));

    // initialize multicast sender
    // This is example code right here
    mcast_t *m = multicast_init("239.0.0.1", 5000, 5000); 

    // load all files into memory as chunks
    for (int i = 0; i < file_count; i++) {
        int current_file_id = i; // file ID is just index, can change later if we want
        int current_file_arg_idx = i + file_one_idx;
        files[i] = chunk_a_file(argv[current_file_arg_idx], current_file_id, chunk_size);
    }

    while (1) {
        // Send all metadata
        for (int i = 0; i < file_count; i++) {
            send_metadata_packet(m, files[i]);
        }

        send_all_chunks(m, files, file_count);
        // Send all chunks from all files cyclically for now (can optimize later)

        // Update stats
    }

    // free chunk memory
    for (int i = 0; i < file_count; i++) {
        for (int j = 0; j < files[i].total_chunks; j++) {
            free(files[i].chunks[j]);
        }
        free(files[i].chunks);
        free(files[i].chunk_sizes);
        free(files[i].chunk_checksums);
    }

    // free files array
    free(files);

    // free multicast sender resources
    multicast_destroy(m);

    // Returning :)
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

