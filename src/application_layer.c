// Application layer protocol implementation

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "application_layer.h"
#include "application_layer/packet.h"
#include "byte_vector.h"
#include "link_layer.h"
#include "log.h"

int send_packet(LLConnection *connection, Packet *packet) {
    ByteVector *bv = bv_create();

    bv_push(bv, (uint8_t *)&packet->type, sizeof(packet->type));

    if (packet->information != NULL)
        bv_push(bv, packet->information->array, packet->information->length);

    int result = llwrite(connection, bv->array, bv->length);

    bv_destroy(bv);

    return result;
}

Packet *create_control_packet(enum packet_type packet_type, size_t file_size,
                              const char *file_name) {
    Packet *packet = (Packet *)malloc(sizeof(Packet));

    packet->type = packet_type;
    packet->information = NULL;

    if (packet_type == START) {
        packet->information = bv_create();

        enum control_packet_field_type field_type = FILE_SIZE;
        size_t file_size_length = sizeof(file_size);
        bv_push(packet->information, (uint8_t *)&field_type,
                sizeof(enum control_packet_field_type));
        bv_push(packet->information, (uint8_t *)&file_size_length,
                sizeof(size_t));
        bv_push(packet->information, (uint8_t *)&file_size, file_size_length);

        field_type = FILE_NAME;
        size_t file_name_length = strlen(file_name);
        bv_push(packet->information, (uint8_t *)&field_type,
                sizeof(enum control_packet_field_type));
        bv_push(packet->information, (uint8_t *)&file_name_length,
                sizeof(file_name_length));
        bv_push(packet->information, (uint8_t *)file_name, file_name_length);
    }

    return packet;
}

int send_control_packet(LLConnection *connection, enum packet_type packet_type,
                        size_t file_size, char *file_name) {
    Packet *packet = create_control_packet(packet_type, file_size, file_name);

    char *packet_type_name = packet_type == START ? "START" : "END";

    LOG("Sending %s control packet!\n", packet_type_name);

    int result = send_packet(connection, packet);

    packet_destroy(packet);

    if (result == -1)
        ERROR("Could not send %s packet\n", packet_type_name);
    else
        LOG("Control packet sent\n");
    return result;
}

/**
 * Fills in the packet header for the given packet
 */
void fill_data_packet_header(Packet *packet, uint16_t fragment_size,
                             uint8_t sequence_number) {
    if (packet == NULL)
        return;

    if (packet->information == NULL)
        packet->information = bv_create();

    bv_pushb(packet->information, (uint8_t)sequence_number);
    bv_pushb(packet->information, (uint8_t)((fragment_size & 0xFF00) >> 8));
    bv_pushb(packet->information, (uint8_t)(fragment_size & 0xFF));
}

int send_data_packet(LLConnection *connection, uint8_t *buf, size_t len) {
    // TODO: maybe make this a Packet attribute
    static uint8_t sequence_number = 0;

    Packet *packet = (Packet *)malloc(sizeof(Packet));

    packet->type = DATA;
    packet->information = bv_create();

    fill_data_packet_header(packet, len, sequence_number++);

    bv_push(packet->information, buf, len);

    int result = send_packet(connection, packet);

    packet_destroy(packet);

    if (result == -1)
        ERROR("Could not send DATA packet with length %lu\n", len);
    else
        LOG("Data packet sent\n");

    return result;
}

LLConnectionParams setupLLParams(const char *serial_port, const char *role,
                                 int baud_rate, int nTries, int timeout) {
    LLConnectionParams ll = {.baud_rate = baud_rate,
                             .n_retransmissions = nTries,
                             .timeout = timeout,
                             .role = strcmp(role, "rx") == 0 ? LL_RX : LL_TX};

    strncpy(ll.serial_port, serial_port, sizeof(ll.serial_port) - 1);

    return ll;
}

LLConnection *connect(LLConnectionParams ll) {
    LOG("Connecting to %s\n", ll.serial_port);

    LLConnection *connection = llopen(ll);

    if (connection == NULL) {
        ERROR("Serial connection on port %s not available, aborting\n",
              ll.serial_port);
        exit(-1);
    }

    if (ll.role == LL_TX) {
        LOG("Connection established\n");
    }

    return connection;
}

int init_transmission(LLConnection *connection, char *filename) {
    struct stat st;

    if (stat(filename, &st) != 0) {
        perror("Could not determine size of file to transmit");
        return -1;
    }

    if (send_control_packet(connection, START, st.st_size, filename) == -1) {
        ERROR("Error sending START packet for file: %s\n", filename);
        return -1;
    }

    LOG("Successfully sent START packet!\n");

    return 1;
}

#define MAX_PAYLOAD_SIZE (1000 - 3 - sizeof(DATA))

void applicationLayer(const char *serial_port, const char *role, int baud_rate,
                      int n_tries, int timeout, const char *filename) {

    LLConnectionParams ll =
        setupLLParams(serial_port, role, baud_rate, n_tries, timeout);

    LLConnection *connection = connect(ll);

    uint8_t packet_data[MAX_PAYLOAD_SIZE];
    if (ll.role == LL_RX) {

        uint8_t *packet_ptr = NULL;
        int fd = -1;
        uint8_t *file_name;
        size_t file_size;

        while (true) {
            int bytes_read = llread(connection, packet_data);

            if (bytes_read == -1) {
                ERROR("Invalid read!\n");
                break;
            }

            packet_ptr = packet_data;

            LOG("Processing packet\n");

            enum packet_type packet_type = *(enum packet_type *)packet_ptr;
            packet_ptr += sizeof(enum packet_type);

            // TODO: this should be refactored

            if (packet_type == END)
                break;
            else if (packet_type == START) {

                {
                    packet_ptr += sizeof(enum control_packet_field_type);

                    // size_t filesize_length = *(size_t *)packet_ptr;
                    packet_ptr += sizeof(size_t);

                    file_size = *(size_t *)packet_ptr;
                    packet_ptr += sizeof(size_t);
                }

                {
                    packet_ptr += sizeof(enum control_packet_field_type);

                    size_t filename_length = *(size_t *)packet_ptr;
                    packet_ptr += sizeof(size_t);

                    file_name = calloc(filename_length + strlen("_test") + 1,
                                       sizeof(uint8_t));
                    sprintf(file_name, "%.*s_test", filename_length,
                            packet_ptr);
                    packet_ptr += filename_length;
                }

                LOG("Opening file descriptor for file: %s\n", file_name);

                fd = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);

                if (fd == -1) {
                    perror("opening RX fd");
                    exit(-1);
                }

            } else if (packet_type == DATA) {
                uint8_t sequence_number = *(uint8_t *)packet_ptr;
                packet_ptr += sizeof(uint8_t);

                uint8_t fragment_size_h = *(uint8_t *)packet_ptr;
                packet_ptr += sizeof(uint8_t);
                uint8_t fragment_size_l = *(uint8_t *)packet_ptr;
                packet_ptr += sizeof(uint8_t);

                uint16_t fragment_size =
                    (fragment_size_h << 8) | fragment_size_l;

                LOG("Writing %d bytes to %s\n", fragment_size, file_name);

                ssize_t bytes_written = write(fd, packet_ptr, fragment_size);
                static size_t total_bytes_written = 0;

                if (bytes_written == -1) {
                    perror("writing to RX fd");
                    exit(-1);
                } else if (bytes_written != 0) {
                    total_bytes_written += bytes_written;

                    INFO("Written %lf%% of the file\n",
                         (double)(total_bytes_written * 100.0 / file_size));
                }
            }
        }

        if (fd != -1)
            close(fd);
    } else {

        // TODO: Use ByteVector

        int fd = open(filename, O_RDWR | O_NOCTTY);
        if (fd == -1) {
            perror("Error opening file in transmitter!");
            exit(-1);
        }

        if (init_transmission(connection, filename) == -1) {
            exit(-1);
        }

        while (true) {
            int bytes_read = read(fd, packet_data, MAX_PAYLOAD_SIZE);

            if (bytes_read == -1) {
                perror("Error reading file fragment, aborting");
                break;
            } else if (bytes_read == 0) {
                // reached end of file, send END packet

                if (send_control_packet(connection, END, 0, "") == -1) {
                    ERROR("Error sending END control packet\n");
                }

                break;
            } else {
                if (send_data_packet(connection, packet_data, bytes_read) ==
                    -1) {
                    ERROR("Error sending DATA packet\n");
                    break;
                };
            }
        }

        close(fd);
    }

    llclose(connection, false);
}
