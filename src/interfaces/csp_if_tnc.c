/*
Cubesat Space Protocol - A small network-layer protocol designed for Cubesats
Copyright (C) 2012 GomSpace ApS (http://www.gomspace.com)
Copyright (C) 2012 AAUSAT3 Project (http://aausat3.space.aau.dk) 

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <csp/csp.h>
#include <csp/csp_endian.h>
#include <csp/csp_platform.h>
#include <csp/csp_interface.h>
#include <csp/interfaces/csp_if_tnc.h>
#include <csp/arch/csp_semaphore.h>
#include <csp/csp_crc32.h>

#define TNC_MTU				256

#define FEND  					0xC0
#define FESC  					0xDB
#define TFEND 					0xDC
#define TFESC 					0xDD

#define TNC_DATA				0x00
#define TNC_SET_HARDWARE		0x06
#define TNC_RETURN				0xFF

static int tnc_lock_init = 0;
static csp_bin_sem_handle_t tnc_lock;

uint8_t test_byte = 0x00;

uint8_t ax25_dest_src_bytes[] = {
		0x96, 0x92, 0x9E, 0x9E, 0x6E, 0xB2, 0x60,
		0x96, 0x92, 0x9E, 0x9E, 0x6E, 0xB2, 0x61
	};
uint8_t channel_select = 0x00;

/* Send a CSP packet over the TNC RS232 protocol */
static int csp_tnc_tx(csp_iface_t * interface, csp_packet_t * packet, uint32_t timeout) {

	if (interface == NULL || interface->driver == NULL)
		return CSP_ERR_DRIVER;

	/* Add CRC32 checksum */
	//csp_crc32_append(packet, false);
	// csp_crc32_append(packet, true);

	/* Save the outgoing id in the buffer */
	packet->id.ext = csp_hton32(packet->id.ext);
	packet->length += sizeof(packet->id.ext);

	/* Lock */
	csp_bin_sem_wait(&tnc_lock, 1000);

	/* Transmit data */
	//printf("csp_tnc_tx: transmitting bytes: ");
	csp_tnc_handle_t * driver = interface->driver;
	driver->tnc_putc(FEND); //printf("%02x ", FEND); 
	driver->tnc_putc(channel_select); //printf("%02x ", 0x10); // TNC_DATA needs to be 0x10

	// Write AX.25 destination and source to tnc
	
	for (int i = 0; i < 14; i++) {
		driver->tnc_putc(ax25_dest_src_bytes[i]); //printf("%02x ", ax25_dest_src_bytes[i]);
	}

	// Write AX.25 ctrl bits and protocol ID to tnc
	driver->tnc_putc(0x03); //printf("%02x ", 0x03); // AX.25 control bits (fixed)
	driver->tnc_putc(0xf0); //printf("%02x ", 0xf0); // AX.25 protocol id (fixed)

	for (unsigned int i = 0; i < packet->length; i++) {
		if (((unsigned char *) &packet->id.ext)[i] == FEND) {
			((unsigned char *) &packet->id.ext)[i] = TFEND;
			driver->tnc_putc(FESC); //printf("%02x ", FESC);
		} else if (((unsigned char *) &packet->id.ext)[i] == FESC) {
			((unsigned char *) &packet->id.ext)[i] = TFESC;
			driver->tnc_putc(FESC); //printf("%02x ", FESC);
		}
		driver->tnc_putc(((unsigned char *) &packet->id.ext)[i]); //printf("%02x ", ((unsigned char *) &packet->id.ext)[i]);
	}
	driver->tnc_putc(FEND); //printf("%02x ", FEND);
	//printf("\n");

	/* Free data */
	csp_buffer_free(packet);

	/* Unlock */
	csp_bin_sem_post(&tnc_lock);

	return CSP_ERR_NONE;
}

/**
 * When a frame is received, decode the tnc-stuff
 * and eventually send it directly to the CSP new packet function.
 */
void csp_tnc_rx(csp_iface_t * interface, uint8_t * buf, int len, void * pxTaskWoken) {
	// printf("Debug#1\n");
	/* Driver handle */
	csp_tnc_handle_t * driver = interface->driver;

	while (len--) {

		/* Input */
		unsigned char inputbyte = *buf++;
		// printf("Debug#2\n");
		/* If packet was too long */
		if (driver->rx_length > interface->mtu) {
			csp_log_warn("TNC RX overflow");
			interface->rx_error++;
			driver->rx_mode = TNC_MODE_NOT_STARTED;
			driver->rx_length = 0;
		}
		// printf("Debug#3\n");
		switch (driver->rx_mode) {
			
		case TNC_MODE_NOT_STARTED:

			/* Send normal chars back to usart driver */
			if (inputbyte != FEND) {
				if (driver->tnc_discard != NULL)
					driver->tnc_discard(inputbyte, pxTaskWoken);
				break;
			}
			// printf("Debug#4\n");
			/* Try to allocate new buffer */
			if (driver->rx_packet == NULL) {
				if (pxTaskWoken == NULL) {
					driver->rx_packet = csp_buffer_get(interface->mtu);
				} else {
					driver->rx_packet = csp_buffer_get_isr(interface->mtu);
				}
			}	
			// printf("Debug#5\n");
			/* If no more memory, skip frame */
			if (driver->rx_packet == NULL) {
				driver->rx_mode = TNC_MODE_SKIP_FRAME;
				break;
			}
			// printf("Debug#6\n");
			/* Start transfer */
			driver->rx_length = 0;
			driver->rx_mode = TNC_MODE_STARTED;
			driver->rx_first = 1;
			break;

		case TNC_MODE_STARTED:

			/* Escape char */
			if (inputbyte == FESC) {
				driver->rx_mode = TNC_MODE_ESCAPED;
				break;
			}
			// printf("Debug#7\n");
			/* End Char */
			if (inputbyte == FEND) {

				/* Accept message */
				if (driver->rx_length > 0) {

					int ax25_len = 16;

					//printf("csp_if_tnc: Message accepted\n");
					// printf("Debug#8\n");
					/* Check for valid length */
					printf("Expected Length: %d Got Length is %d \n",CSP_HEADER_LENGTH + sizeof(uint32_t)+ax25_len,driver->rx_length);
					if (driver->rx_length < CSP_HEADER_LENGTH + sizeof(uint32_t)+ax25_len) {
						printf("csp_if_tnc: Invalid length! Expected %d but got %d\n", CSP_HEADER_LENGTH + 4+ax25_len, driver->rx_length);
						csp_log_warn("TNC short frame skipped, len: %u", driver->rx_length);
						interface->rx_error++;
						driver->rx_mode = TNC_MODE_NOT_STARTED;
						break;
					}

					/* Count received frame */
					interface->frame++;

					/* The CSP packet length is without the header */
					driver->rx_packet->length = driver->rx_length - CSP_HEADER_LENGTH;

					/* Convert the packet from network to host order */
					driver->rx_packet->id.ext = csp_ntoh32(driver->rx_packet->id.ext);

					/* Validate CRC */
					// if (csp_crc32_verify(driver->rx_packet, false) != CSP_ERR_NONE) {
					// 	printf("csp_if_tnc: Invalid CRC!\n");
					// 	csp_log_warn("TNC invalid crc frame skipped, len: %u", driver->rx_packet->length);
					// 	interface->rx_error++;
					// 	driver->rx_mode = TNC_MODE_NOT_STARTED;
					// 	break;
					// }
					// printf("Debug#9\n");
					// test_byte = 0x60;
					// printf("Current Test Byte = %02x\n",test_byte);
					//Print Packet Data -- Testing Only
					 printf("Packet Data (starting from the ID): \n");
					 for (int i = 0; i < driver->rx_packet->length + sizeof(driver->rx_packet->id) - sizeof(uint32_t); i++){
					 	// if(i<=0x60){
						// 	 ((uint8_t*)&(driver->rx_packet->id))[i] = test_byte;
						//  }
						 printf("%02x ", ((uint8_t*)&(driver->rx_packet->id))[i]);
					 }
					 printf("\n ASCII:\n");
					// Print in ASCII
					 for (int i = 0; i < driver->rx_packet->length + sizeof(driver->rx_packet->id) - sizeof(uint32_t); i++){
					 	printf("%c ", ((uint8_t*)&(driver->rx_packet->id))[i]);
					 }
					 printf("\n");

					//  test_byte ++;

					// REMOVE AX25 STUFF HERE
					// At this point, the first c0 has been removed. You must now remove the AX25 bytes
					// from the data section and send the resulting packet to csp_qfifo_write (as done
					// in the code below)
					// 
					// printf("Debug#10\n");
					// Kiss(2): c0 10
					// AX25(16): 7 callsign, 7 callsign, 2 parameter
					
					//int chk_sm_len = 4;
					// Save new header bytes as int (id.ext type = 32)
					uint32_t new_header = 0;
					new_header |= ((uint8_t*)&(driver->rx_packet->data))[15]; new_header = new_header << 8;
					new_header |= ((uint8_t*)&(driver->rx_packet->data))[14]; new_header = new_header << 8;
					new_header |= ((uint8_t*)&(driver->rx_packet->data))[13]; new_header = new_header << 8;
					new_header |= ((uint8_t*)&(driver->rx_packet->data))[12]; 
					//printf("Saved header bytes: %x\n", new_header);
					// printf("Debug#11\n");
					// Save new data buffer 
					int new_len = (driver->rx_packet->length) - ax25_len;
					//printf("csp_if_tnc:data length :%d\n",new_len);
					uint8_t* data_buffer = (uint8_t*)malloc(new_len);
					for (int i = 0; i < new_len; i++) {
						data_buffer[i] = ((uint8_t*)&(driver->rx_packet->data))[i + ax25_len];
					}
					// printf("Debug#12\n");
					// Verify bytes of data buffer: 
					// printf("New data bytes: ");
					// for (int i = 0; i < new_len; i++) {
					// 	printf("%02x ", data_buffer[i]);
					// }
					// printf("\n");

					// Copy new data back to the rx_packet
					driver->rx_packet->id.ext = csp_ntoh32(new_header);
					driver->rx_packet->length = new_len;
					memcpy(driver->rx_packet->data, data_buffer, new_len);
					free(data_buffer);
					driver->rx_packet->length--;
					// printf("Debug#13\n");
					// printf("csp_if_tnc: new packet header: ");
					// for (int i = 0; i < 4; i++) {
					// 	printf("%02x ", ((uint8_t*)(&driver->rx_packet->id))[i]);
					// }
					// printf("\n");

					// printf("csp_if_tnc: new packet data: ");
					// for (int i = 0; i < driver->rx_packet->length; i++) {
					// 	printf("%02x ", driver->rx_packet->data[i]);
					// }
					// printf("\n");
					// printf("Debug#14\n");
					/* Send back into CSP, notice calling from task so last argument must be NULL! */
					csp_qfifo_write(driver->rx_packet, interface, pxTaskWoken);
					driver->rx_packet = NULL;
					driver->rx_mode = TNC_MODE_NOT_STARTED;
					// printf("Debug#15\n");
					break;

				}

				/* Break after the end char */
				break;
			}
			// printf("Debug#16\n");
			/* Skip the first char after FEND which is TNC_DATA (0x00) */
			if (driver->rx_first) {
				driver->rx_first = 0;
				break;
			}

			/* Valid data char */
			((char *) &driver->rx_packet->id.ext)[driver->rx_length++] = inputbyte;

			break;

		case TNC_MODE_ESCAPED:

			/* Escaped escape char */
			if (inputbyte == TFESC)
				((char *) &driver->rx_packet->id.ext)[driver->rx_length++] = FESC;
			// printf("Debug#17\n");
			/* Escaped fend char */
			if (inputbyte == TFEND)
				((char *) &driver->rx_packet->id.ext)[driver->rx_length++] = FEND;
			printf("Debug#18\n");
			/* Go back to started mode */
			driver->rx_mode = TNC_MODE_STARTED;
			break;

		case TNC_MODE_SKIP_FRAME:
			// printf("Debug#19\n");
			/* Just wait for end char */
			if (inputbyte == FEND)
				driver->rx_mode = TNC_MODE_NOT_STARTED;

			break;

		}

	}

}

void csp_tnc_init(csp_iface_t * csp_iface, csp_tnc_handle_t * csp_tnc_handle, csp_tnc_putc_f tnc_putc_f, csp_tnc_discard_f tnc_discard_f, const char * name) {

	/* Init lock only once */
	if (tnc_lock_init == 0) {
		csp_bin_sem_create(&tnc_lock);
		tnc_lock_init = 1;
	}

	/* Register device handle as member of interface */
	csp_iface->driver = csp_tnc_handle;
	csp_tnc_handle->tnc_discard = tnc_discard_f;
	csp_tnc_handle->tnc_putc = tnc_putc_f;
	csp_tnc_handle->rx_packet = NULL;
	csp_tnc_handle->rx_mode = TNC_MODE_NOT_STARTED;

	/* Setop other mandatories */
	csp_iface->mtu = TNC_MTU;
	csp_iface->nexthop = csp_tnc_tx;
	csp_iface->name = name;

	/* Regsiter interface */
	csp_iflist_add(csp_iface);

}
