/*
    Raspberry Pi / 

    GPIO RAW NAND flasher
    (made out of "360-Clip based 8-bit NAND reader" by pharos)

    Copyright (C)	2016 littlebalup
					2019 skypiece

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>

#define DEBUG 1

#define PAGE_SIZE 2112 // (2K + 64)Byte
#define BLOCK_SIZE 135168 // 64 pages (128K + 4K)Byte
#define MAX_WAIT_READ_BUSY	1000000


/* For Raspberry B+ :*/
#define BCM2708_PERI_BASE	0x20000000
#define GPIO_BASE	 	(BCM2708_PERI_BASE + 0x200000)

/* For Raspberry 2B and 3B :*/
// #define BCM2736_PERI_BASE			0x3F000000
// #define GPIO_BASE					(BCM2736_PERI_BASE + 0x200000) /* GPIO controller */

#define GPIO_PULL_NONE			0
#define GPIO_PULL_DOWN			1
#define GPIO_PULL_UP			2

// IMPORTANT: BE VERY CAREFUL TO CONNECT VCC TO P1-01 (3.3V) AND *NOT* P1-02 (5V) !!
// IMPORTANT: MAY BE YOU NEED EXTERNAL 1.8V for modern NANDs

// GPIO pins have been chose to compitable Waveshare NandFlash Board and lost RPi SMI NAND driver
#define NAND_PIN_nWP	2
#define NAND_PIN_ALE	4
#define NAND_PIN_CLE	5
#define NAND_PIN_nRE	6
#define NAND_PIN_nWE	7
#define NAND_PIN_nRB1	17
#define NAND_PIN_nCE1	27
#define NAND_PIN_nRB2	3
#define NAND_PIN_nCE2	22

int NAND_IO_PINS[8] = { 8, 9, 10, 11, 12, 19, 14, 15 }; // 8 is NAND IO 0, etc.

volatile unsigned *gpio;

int read_id(unsigned char id[5]);
int read_pages(int first_page_number, int number_of_pages, char *outfile, int write_spare);
int write_pages(int first_page_number, int number_of_pages, char *infile);
int erase_blocks(int first_block_number, int number_of_blocks);

static inline void delay_us(long us) {
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = us * 1000,
	};
	nanosleep(&ts, NULL);
}

unsigned shortpause_delay = 50;
static inline void shortpause() {
	delay_us(shortpause_delay);
}

static inline void PULL_MODE_GPIO(int g, int m)
{
	if (m > GPIO_PULL_UP)
		return;
	*(gpio + 37) = m & 3;
	delay_us(5);
	*(gpio + 38) = 1 << g;
	delay_us(5);
	*(gpio + 37) = 0;
	*(gpio + 38) = 0;
}

static inline void INP_GPIO(int g) { gpio[g / 10] &= ~(7 << ((g % 10) * 3)); }
static inline void OUT_GPIO(int g) { INP_GPIO(g); gpio[g / 10] |= 1 << ((g % 10) * 3); }
static inline void GPIO_SET_1(int g) { gpio[7] = 1 << g; }
static inline void GPIO_SET_0(int g) { gpio[10] = 1 << g; }
static inline int GPIO_READ(int g) { return (gpio[13] >> g) & 1; }

int iomode = -1;
static inline void NAND_IO_INPUT(void) { if (iomode == 0) return; for (int i = 0; i < 8; i++) INP_GPIO(NAND_IO_PINS[i]); iomode = 0; }
static inline void NAND_IO_OUTPUT(void) { if (iomode == 1) return; for (int i = 0; i < 8; i++) OUT_GPIO(NAND_IO_PINS[i]); iomode = 1; }

static inline int NAND_IO_RAW_READ() {
	int data = 0;
	for (int i = 0; i < 8; i++)
		data |= GPIO_READ(NAND_IO_PINS[i]) << i;
	return data;
}
static inline void NAND_IO_RAW_SET(int data) {
	for (int i = 0; i < 8; i++) {
		if (data & (1 << i))
			GPIO_SET_1(NAND_IO_PINS[i]);
		else
			GPIO_SET_0(NAND_IO_PINS[i]);
	}
}

static inline int NAND_IO_READ() {
	NAND_IO_INPUT();
	GPIO_SET_0(NAND_PIN_nRE);
	shortpause();
	int data = NAND_IO_RAW_READ();
	GPIO_SET_1(NAND_PIN_nRE);
	shortpause();
	return data;
}

static inline void NAND_IO_SET(int data) {
	NAND_IO_OUTPUT();
	NAND_IO_RAW_SET(data);
	GPIO_SET_0(NAND_PIN_nWE);
	shortpause();
	GPIO_SET_1(NAND_PIN_nWE);
	shortpause();
}

static inline void SEND_CMD(int cmd) {
	GPIO_SET_1(NAND_PIN_CLE);
	NAND_IO_SET(cmd);
	GPIO_SET_0(NAND_PIN_CLE);
	shortpause();
}

static inline void SEND_ADDR(int addr)
{
	GPIO_SET_1(NAND_PIN_ALE);
	NAND_IO_SET(addr);
	GPIO_SET_0(NAND_PIN_ALE);
	shortpause();
}

void nand_setup_pulls()
{
	PULL_MODE_GPIO(NAND_PIN_nRB1, GPIO_PULL_UP);
	PULL_MODE_GPIO(NAND_PIN_nRB2, GPIO_PULL_UP);
	for (int i = 0; i < 8; i++)
		PULL_MODE_GPIO(NAND_IO_PINS[i], GPIO_PULL_UP);
}

void nand_enable()
{
	OUT_GPIO(NAND_PIN_nWP);
	OUT_GPIO(NAND_PIN_ALE);
	OUT_GPIO(NAND_PIN_CLE);
	OUT_GPIO(NAND_PIN_nRE);
	OUT_GPIO(NAND_PIN_nWE);
	OUT_GPIO(NAND_PIN_nCE1);
	OUT_GPIO(NAND_PIN_nCE2);
	INP_GPIO(NAND_PIN_nRB1);
	INP_GPIO(NAND_PIN_nRB2);

	GPIO_SET_1(NAND_PIN_nWP);
	GPIO_SET_0(NAND_PIN_ALE);
	GPIO_SET_0(NAND_PIN_CLE);
	GPIO_SET_1(NAND_PIN_nRE);
	GPIO_SET_1(NAND_PIN_nWE);
	GPIO_SET_1(NAND_PIN_nCE1);
	GPIO_SET_0(NAND_PIN_nCE2);

	NAND_IO_OUTPUT();
}

int wait_rb()
{
	/* Should be done within 3 milliseconds for all commands. */
	time_t timeout = time(0) + 3;

	while (time(0) < timeout) {
		if ( GPIO_READ(NAND_PIN_nRB2) )
			return 0;
		delay_us(1);
	}

	printf("wait_rb timed out\n");
	return -1;
}

void nand_reset()
{
	nand_enable();
	SEND_CMD(0xFF);
	wait_rb();
}

static uint32_t * initMapMem(int fd, uint32_t addr, uint32_t len)
{
    return (uint32_t *) mmap(0, len,
       PROT_READ | PROT_WRITE,
       MAP_SHARED | MAP_FIXED | MAP_LOCKED,
       fd, addr);
}

int main(int argc, char **argv)
{ 
	int mem_fd;

	printf("Raspberry GPIO raw NAND flasher by pharos, littlebalup, skypiece\n\n");

	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC)) < 0) {
		perror("open /dev/mem, are you root?");
		return -1;
	}

	if (((gpio = (volatile unsigned *)mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, mem_fd, GPIO_BASE)) == MAP_FAILED)) {
		perror("registers mmap failed\n");
		close(mem_fd);
		return -1;
	}

	if (argc < 3) {
usage:
		GPIO_SET_1(NAND_PIN_nCE2);
		printf("usage: sudo %s <delay> <command> ...\n\n" \
		    " <delay> used to slow down operations (50 should work, increase if bad reads)\n\n" \
		    "Commands:\n" \
		    " read_id (no arguments)                        : read and decrypt chip ID\n" \
		    " read_full <page #> <# of pages> <output file> : read N pages including spare\n" \
		    " read_data <page #> <# of pages> <output file> : read N pages, discard spare\n" \
		    " write_full <page #> <# of pages> <input file> : write N pages, including spare\n" \
		    " write_data <page #> <# of pages> <input file> : write N pages, discard spare\n" \
		    " erase_blocks <block number> <# of blocks>     : erase N blocks\n\n" \
		    "Notes:\n" \
		    " This program assumes PAGE_SIZE == %d\n" \
		    " Run as root (sudo) required (for /dev/mem access)\n\n",
			argv[0], PAGE_SIZE);
		close(mem_fd);
		return -1;
	}

	shortpause_delay = atoi(argv[1]);

	struct sched_param sp = { .sched_priority = 99 };
	sched_setscheduler(getpid(), SCHED_RR, &sp);

	nand_setup_pulls();
	nand_reset();

	if (strcmp(argv[2], "read_id") == 0) {
		return read_id(NULL);
	}

	if (strcmp(argv[2], "read_full") == 0) {
		if (argc != 6) goto usage;
		if (atoi(argv[4]) <= 0) {
			printf("# of pages must be > 0\n");
			return -1;
		}
		return read_pages(atoi(argv[3]), atoi(argv[4]), argv[5], 1);
	}

	if (strcmp(argv[2], "read_data") == 0) {
		if (argc != 6) goto usage;
		if (atoi(argv[4]) <= 0) {
			printf("# of pages must be > 0\n");
			return -1;
		}
		return read_pages(atoi(argv[3]), atoi(argv[4]), argv[5], 0);
	}

	if (strcmp(argv[2], "write_full") == 0) {
		if (argc != 6) goto usage;
		if (atoi(argv[4]) <= 0) {
			printf("# of pages must be > 0\n");
			return -1;
		}
		return write_pages(atoi(argv[3]), atoi(argv[4]), argv[5]);
	}

	if (strcmp(argv[2], "erase_blocks") == 0) {
		if (argc != 5) goto usage;
		if (atoi(argv[4]) <= 0) {
			printf("# of blocks must be > 0\n");
			return -1;
		}
		return erase_blocks(atoi(argv[3]), atoi(argv[4]));
	}

	printf("unknown command '%s'\n", argv[2]);
	goto usage;
	return 0;
}

void error_msg(const char *msg)
{
	printf("%s\nBe sure to check wiring, and check that pressure is applied on clip (if used)\n", msg);
}

void print_id(unsigned char id[5])
{
	unsigned int i, bit, page_size, ras_size, orga, plane_number;
	unsigned long block_size, plane_size, nand_size, nandras_size;
	char maker[16], device[16], serial_access[20];
	unsigned *thirdbits = (unsigned*)malloc(sizeof(unsigned) * 8);
	unsigned *fourthbits = (unsigned*)malloc(sizeof(unsigned) * 8);
	unsigned *fifthbits = (unsigned*)malloc(sizeof(unsigned) * 8);

	printf("Raw ID data: ");
	for (i = 0; i < 5; i++)
		printf("0x%02X ", id[i]);
	printf("\n");

 	switch(id[0]) {
 		case 0xEC: {
 			strcpy(maker, "Samsung");
 			switch(id[1]) {
 				case 0xA1: strcpy(device, "K9F1G08R0A"); break;
 				case 0xD5: strcpy(device, "K9GAG08U0M"); break;
 				case 0xF1: strcpy(device, "K9F1G08U0A/B"); break;
 				default: strcpy(device, "unknown");
 			}
 			break;
 		}
 		case 0xAD: {
 			strcpy(maker, "Hynix");
 			switch(id[1]) {
 				case 0x73: strcpy(device, "HY27US08281A"); break;
 				case 0xD7: strcpy(device, "H27UBG8T2A"); break;
 				case 0xDA: strcpy(device, "HY27UF082G2B"); break;
 				case 0xDC: strcpy(device, "H27U4G8F2D"); break;
 				default: strcpy(device, "unknown");
 			}
 			break;
 		}
 		case 0x2C: {
 			strcpy(maker, "Micron");
 			switch(id[1]) {
 				default: strcpy(device, "unknown");
 			}
 			break;
 		}
 		default: strcpy(maker, "unknown"); strcpy(device, "unknown");
 	}

/* all sizes in bytes */
	for(bit = 0; bit < 8; ++bit)
		thirdbits[bit] = (id[2] >> bit) & 1;

	for(bit = 0; bit < 8; ++bit)
		fourthbits[bit] = (id[3] >> bit) & 1;
	switch(fourthbits[1] * 10 + fourthbits[0]) {
		case 00: page_size = 1024; break;
		case 01: page_size = 2048; break;
		case 10: page_size = 4096; break;
		case 11: page_size = 8192; break;
	}
	switch(fourthbits[5] * 10 + fourthbits[4]) {
		case 00: block_size = 64 * 1024; break;
		case 01: block_size = 128 * 1024; break;
		case 10: block_size = 256 * 1024; break;
		case 11: block_size = 521 * 1024; break;
	}
	switch(fourthbits[2]) {
		case 0: ras_size = 8; break; // for 512 bytes
		case 1: ras_size = 16; break; // for 512 bytes
	}
	switch(fourthbits[6]) {
		case 0: orga = 8; break; // bits
		case 1: orga = 16; break; // bits
	}
	switch(fourthbits[7] * 10 + fourthbits[3]) {
		case 00: strcpy(serial_access, "50ns/30ns minimum"); break;
		case 10: strcpy(serial_access, "25ns minimum"); break;
		case 01: strcpy(serial_access, "unknown (reserved)"); break;
		case 11: strcpy(serial_access, "unknown (reserved)"); break;
	}

	for(bit = 0; bit < 8; ++bit)
		fifthbits[bit] = (id[4] >> bit) & 1;
	switch(fifthbits[3] * 10 + fifthbits[2]) {
		case 00: plane_number = 1; break;
		case 01: plane_number = 2; break;
		case 10: plane_number = 4; break;
		case 11: plane_number = 8; break;
	}
	switch(fifthbits[6] * 100 + fifthbits[5] * 10 + fifthbits[4]) {
		case 000: plane_size = 64 / 8 * 1024 * 1024; break; // 64 megabits
		case 001: plane_size = 128 / 8 * 1024 * 1024; break; // 128 megabits
		case 010: plane_size = 256 / 8 * 1024 * 1024; break; // 256 megabits
		case 011: plane_size = 512 / 8 * 1024 * 1024; break; // 512 megabits
		case 100: plane_size = 1024 / 8 * 1024 * 1024; break; // 1 gigabit
		case 101: plane_size = 2048 / 8 * 1024 * 1024; break; // 2 gigabits
		case 110: plane_size = 4096 / 8 * 1024 * 1024; break; // 4 gigabits
		case 111: plane_size = 8192 / 8 * 1024 * 1024; break; // 8 gigabits
	}

	nand_size = plane_number * plane_size;
	nandras_size = nand_size + ras_size * nand_size / 512;

	printf("\n");
	printf("NAND manufacturer:  %s (0x%02X)\n", maker, id[0]);
	printf("NAND model:         %s (0x%02X)\n", device, id[1]);
	printf("\n");

	printf("              I/O|7|6|5|4|3|2|1|0|\n");
	printf("3rd ID data:     |");
	for(bit = 8; bit--;)
        printf("%u|", thirdbits[bit]);
    printf(" (0x%02X)\n", id[2]);
	printf("4th ID data:     |");
	for(bit = 8; bit--;)
        printf("%u|", fourthbits[bit]);
    printf(" (0x%02X)\n", id[3]);
	printf("5th ID data:     |");
	for(bit = 8; bit--;)
        printf("%u|", fifthbits[bit]);
    printf(" (0x%02X)\n", id[4]);

	printf("\n");
	printf("Page size:          %d bytes\n", page_size);
	printf("Block size:         %lu bytes\n", block_size);
	printf("RAS (/512 bytes):   %d bytes\n", ras_size);
	// printf("RAS (per page):  %d bytes\n", ras_size * page_size / 512);
	// printf("RAS (per block): %d bytes\n", ras_size * block_size / 512);
	printf("Organisation:       %d bit\n", orga);
	printf("Serial access:      %s\n", serial_access);
	printf("Number of planes:   %d\n", plane_number);
	printf("Plane size:         %lu bytes\n", plane_size);
	printf("\n");
	printf("NAND size:          %lu MB\n", nand_size / (1024 * 1024));
	printf("NAND size + RAS:    %lu MB\n", nandras_size / (1024 * 1024));
	printf("Number of blocks:   %lu\n", nand_size / block_size);
	printf("Number of pages:    %lu\n", nand_size / page_size);
}

int read_id(unsigned char id[5])
{
	int i;
	unsigned char buf[5];

	SEND_CMD(0x90); // Read ID byte 1
	SEND_ADDR(0x00); // Read ID byte 2

	NAND_IO_INPUT();
	for (i = 0; i < 5; i++)
		buf[i] = NAND_IO_READ(); //
	if (id != NULL)
		memcpy(id, buf, 5);
	else
		print_id(buf);
	if (buf[0] == buf[1] && buf[1] == buf[2] && buf[2] == buf[3] && buf[3] == buf[4]) {
		error_msg("all five ID bytes are identical, this is not normal");
		return -1;
	}
	return 0;
}

static inline int page_to_address(int page, int address_byte_index)
{
	switch(address_byte_index) {
	case 2:
		return page & 0xff;
	case 3:
		return (page >>  8) & 0xff;
	case 4:
		return (page >> 16) & 0xff;
	default:
		return 0;
	}
}

int read_status()
{
	SEND_CMD(0x70);
	return NAND_IO_READ() & 1; // I/O0=0 success , I/O0=1 error
}

int send_read_command(int page, unsigned char data[PAGE_SIZE])
{
	int i;

	SEND_CMD(0x00);

	GPIO_SET_1(NAND_PIN_ALE);
	for (i = 0; i < 5; i++) {
		NAND_IO_SET(page_to_address(page, i));
	}
	GPIO_SET_0(NAND_PIN_ALE);
	shortpause();

	SEND_CMD(0x30);

	NAND_IO_INPUT();
	wait_rb();

	for (i = 0; i < PAGE_SIZE; i++)
		data[i] = NAND_IO_READ();

	return 0;
}

int send_write_command(int page, const unsigned char data[PAGE_SIZE])
{
	int i;

	SEND_CMD(0x80);

	GPIO_SET_1(NAND_PIN_ALE);
	for (i = 0; i < 5; i++)
		NAND_IO_SET(page_to_address(page, i));
	GPIO_SET_0(NAND_PIN_ALE);
	shortpause();

	for (i = 0; i < PAGE_SIZE; i++)
		NAND_IO_SET(data[i]);

	SEND_CMD(0x10);

	wait_rb();
	return read_status();
}

int send_eraseblock_command(int block)
{
	int i;

	SEND_CMD(0x60);

	GPIO_SET_1(NAND_PIN_ALE);
	for (i = 2; i < 5; i++)
		NAND_IO_SET(page_to_address(block, i));
	GPIO_SET_0(NAND_PIN_ALE);
	shortpause();


	SEND_CMD(0xD0);

	while (GPIO_READ(NAND_PIN_nRB2) == 0) {
		// printf("Busy\n");
		shortpause();
	}

	return read_status();
}

int read_id_check(const unsigned char id[5]) {
	unsigned char id2[5];

	for (int i = 0; i < 100; i++) {
		if (!read_id(id2) && !memcmp(id, id2, sizeof(id2)))
			return 0;
		printf("\nNAND ID has changed! retrying");
	}

	printf("\nNAND connection appears to be unstable, check your wiring!");
	return -1;
}

int read_pages(int first_page_number, int number_of_pages, char *outfile, int write_spare)
{
	unsigned char id[5], buf[PAGE_SIZE * 2];
	FILE *badlog, *outf;

	if (!(outf = fopen(outfile, "wb"))) {
		perror("fopen output file");
		return -1;
	}

	if (!(badlog = fopen("bad.log", "w+"))) {
		perror("fopen bad.log");
		return -1;
	}

	if (GPIO_READ(NAND_PIN_nRB2) == 0) {
		error_msg("NAND_PIN_nRB2 should be 1 (pulled up), but reads as 0. make sure the NAND is powered on");
		return -1;
	}

	if (read_id(id) < 0)
		return -1;

	print_id(id);
	printf("if this ID is incorrect, press Ctrl-C NOW to abort (3s timeout)\n");
	sleep(3);


	printf("\nStart reading...\n");
	clock_t start = clock();

	for (int page = first_page_number; page < (first_page_number + number_of_pages); page++) {
		int page_nbr = page - first_page_number + 1;
		int percent = (100 * page_nbr) / number_of_pages;

		printf("Reading page n° %d in block n° %d (page %d of %d), %d%%\r",
			page, page / 64, page_nbr, number_of_pages, percent);

		int retry_count = 5;
		for (; retry_count > 0; retry_count--) {
			if (read_id_check(id)) continue;
			send_read_command(page, &buf[0]);

			if (read_id_check(id)) continue;
			send_read_command(page, &buf[PAGE_SIZE]);

			if (!memcmp(&buf[0], &buf[PAGE_SIZE], PAGE_SIZE))
				break;

			printf("Page failed to read correctly! retrying\n");
		}
		if (!retry_count) {
			printf("Too many retries. Perhaps bad block?\n");
			fprintf(badlog, "Page %d seems to be bad\n", page);
			continue;
		}

		int res = write_spare
			? fwrite(buf, PAGE_SIZE, 1, outf)
			: fwrite(buf, 512 * (PAGE_SIZE / 512), 1, outf);
		if (res != 1) {
			perror("fwrite");
			return -1;
		}
	}

	fclose(badlog);
	fclose(outf);

	printf("\n\nReading done in %f seconds\n", (float)(clock() - start) / CLOCKS_PER_SEC);
}

int write_pages(int first_page_number, int number_of_pages, char *infile)
{
	int page, block_no, page_nbr, percent, retry_count;
	unsigned char buf[PAGE_SIZE], id[5], id2[5];;

	if (read_id(id) < 0)
		return -1;

	print_id(id);
	printf("if this ID is incorrect, press Ctrl-C NOW to abort (3s timeout)\n");
	sleep(3);

	printf("\nStart writing...\n");
	clock_t start = clock();


	FILE *f = fopen(infile, "rb");
	if (f == NULL) {
		perror("fopen input file");
		return -1;
	}

	for (retry_count = 0, page = first_page_number; page < first_page_number + number_of_pages; page++) {

	retry_all:

		if (retry_count == 0) {
			page_nbr = page - first_page_number + 1;
			percent = (100 * page_nbr) / number_of_pages;
			block_no = page / 64;
			printf("Writing page n° %d in block n° %d (page %d of %d), %d%%\r", page, block_no, page_nbr, number_of_pages, percent);
			fflush(stdout);
		}

		fseek(f, page * PAGE_SIZE, SEEK_SET);
		fread(buf, PAGE_SIZE, 1, f);

	retry:
		read_id(id2);
		if (memcmp(id, id2, 5) != 0) {
			printf("\nNAND ID has changed! retrying");
			goto retry;
		}

		if (send_write_command(page, buf)) {
			if (retry_count == 0) printf("\n");
			if (retry_count < 5) {
				printf("Failed to write page correctly! retrying\n");
				retry_count++;
				goto retry_all;
			}
			printf("Too many retries. Perhaps bad block?\n");
		}
		retry_count = 0;
	}

	fcloseall();
	clock_t end = clock();
	printf("\nWrite done in %f seconds\n", (float)(end - start) / CLOCKS_PER_SEC);
}

int erase_blocks(int first_block_number, int number_of_blocks)
{
	int block, block_no, block_nbr, percent, i, n, retry_count;
	unsigned char id[5], id2[5];

	if (read_id(id) < 0)
		return -1;
	print_id(id);
	printf("if this ID is incorrect, press Ctrl-C NOW to abort (3s timeout)\n");
	sleep(3);

	printf("\nStart erasing...\n");
	clock_t start = clock();

	for (retry_count = 0, block = first_block_number; block < (first_block_number + number_of_blocks); block++) {

	  retry_all:
			
		block_nbr = block - first_block_number + 1;
		percent = (100 * block_nbr) / number_of_blocks;

		if (retry_count == 0) {
			printf("Erasing block n° %d at adress 0x%02X (block %d of %d), %d%%\r", block, block * BLOCK_SIZE, block_nbr, number_of_blocks, percent);
			fflush(stdout);
		}

	  retry:
		read_id(id2);
		if (memcmp(id, id2, 5) != 0) {
			printf("\nNAND ID has changed! retrying");
			goto retry;
		}

		send_eraseblock_command(block * 64); // 64 = pages per block

		if (read_status()) {
			if (retry_count == 0) printf("\n");
			if (retry_count < 5) {
				printf("Failed to erase block correctly! retrying\n");
				retry_count++;
				goto retry_all;
			}
			printf("Too many retries. Perhaps bad block?\n");
		}
		retry_count = 0;
	}

	clock_t end = clock();
	printf("\nErasing done in %f seconds\n", (float)(end - start) / CLOCKS_PER_SEC);

}
