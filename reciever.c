// receiver.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "multicast.h"
#include "shared_structs.h"

// structs for tracking received files and chunks, can be optimized later
// keep a buffer for each file as a struct here somehow?

// helper for processing a received metadata packet

// helper for processing a received data packet


int main(int argc, char *argv[]) {

    multicast_setup_recv(m);
    if (multicast_check_receive(m) > 0) {
        multicast_receive(m, buffer, buffer_size);
    }

  while (1) {
        // recieving packets here
        // if metadata, update file tracking structs

        // if data, check checksum, update file buffer

        // somehow check if file is complete and if so write to disk
    }

    // free any allocated memory for file buffers, etc.

    return 0;
}

/*
B: Receiver Program (receiver.c)

- Join the multicast group and listen for chunks

- Reassembly:
  * Buffer chunks in RAM OR write to disk incrementally
  * Handle out-of-order delivery using sequence numbers

- Validation:
  * Verify chunk checksums
    - discard corrupted chunks

  * Detect and re-request missing chunks

- Validate final files

- Save files to disk
  (e.g., train.csv in received_files/)
  
  */