#ifndef ESP32_SOCKETS_H
#define ESP32_SOCKETS_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

/**
 * @brief Set destination address for socket connection
 * 
 * @param dest_addr Pointer to sockaddr_in structure to store destination
 * @param addr IP address or hostname string
 * @param port Port number
 * @return 0 on success, -1 on failure
 */
int socket_set_dest_addr(struct sockaddr_in *dest_addr, char *addr, uint16_t port);

/**
 * @brief Close socket connection
 * 
 * @return 0 on success
 */
int esp32_socket_exit(void);

/**
 * @brief Set socket file descriptor
 * 
 * @param sock Pointer to socket file descriptor
 * @return 0 on success
 */
int socket_set_sock(int *sock);

/**
 * @brief Initialize socket subsystem
 * 
 * @return 0 on success
 */
int esp32_socket_init(void);

/**
 * @brief Connect to server using socket
 * 
 * @param addr Server address string
 * @param port Server port
 * @return 0 on success, error code on failure
 */
int esp32_socket_connect(char *addr, uint16_t port);

/**
 * @brief Send data over socket
 * 
 * @param tx_buffer Data buffer to send
 * @param size Size of data to send
 * @return Number of bytes sent, -1 on error
 */
int esp32_socket_send_data(char *tx_buffer, uint16_t size);

/**
 * @brief Receive data from socket
 * 
 * @param buf_recv Buffer to store received data
 * @param size Maximum size of buffer
 * @return Number of bytes received, -1 on error
 */
int esp32_socket_recv_data(char *buf_recv, uint16_t size);

#endif // ESP32_SOCKETS_H
