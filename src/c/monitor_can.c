/*
 Copyright 2011 Tom Parker

 This file is part of the Tumanako EVD5 BMS.

 The Tumanako EVD5 BMS is free software: you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public License as
 published by the Free Software Foundation, either version 3 of the License,
 or (at your option) any later version.

 The Tumanako EVD5 BMS is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU Lesser General Public License
 along with the Tumanako EVD5 BMS.  If not, see
 <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <libgen.h>
#include <time.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <net/if.h>

#include <linux/can.h>
#include <linux/if.h>
#include <linux/can/raw.h>

#include <pthread.h>

#include "util.h"
#include "monitor_can.h"

void monitorCan_sendChar2Shorts(const short frameId, const char c, const short s1, const short s2);
void monitorCan_send2Shorts(const short frameId, const short s1, const short s2);
char monitorCan_send(struct can_frame *frame);

/* CAN BUS socket */
int s;
static unsigned char error = 1;

static int getSocket() {
	if (error) {
		fprintf(stderr, "resetting can bus");
		system("./slcan.py");
		sleep(1);
		s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
		if (s == -1) {
			return -1;
		}

		struct ifreq ifr;
		strcpy(ifr.ifr_name, "slcan0");
		ioctl(s, SIOCGIFINDEX, &ifr);

		struct sockaddr_can addr;
		addr.can_family = AF_CAN;
		addr.can_ifindex = ifr.ifr_ifindex;

		error = bind(s, (struct sockaddr *) &addr, sizeof(addr));
		if (error) {
			return -1;
		}
	}
	return s;
}

void monitorCan_sendChar2ShortsChar(const short frameId, const char c, const short s1, const short s2, const char c2) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 6;
	charToBuf(c, frame.data);
	shortToBuf(s1, frame.data + 1);
	shortToBuf(s2, frame.data + 3);
	charToBuf(c2, frame.data + 5);
	monitorCan_send(&frame);
}

void monitorCan_sendCharShortChar(const short frameId, const char c, const short s1, const char c2) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 4;
	charToBuf(c, frame.data);
	shortToBuf(s1, frame.data + 1);
	charToBuf(c2, frame.data + 3);
	monitorCan_send(&frame);
}

void monitorCan_sendCharShortCharShort(const short frameId, const char c, const short s1, const char c2, const short s2) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 6;
	charToBuf(c, frame.data);
	shortToBuf(s1, frame.data + 1);
	charToBuf(c2, frame.data + 3);
	shortToBuf(s2, frame.data + 4);
	monitorCan_send(&frame);
}

void monitorCan_send3Char(const short frameId, const char c1, const char c2, const char c3) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 3;
	charToBuf(c1, frame.data);
	charToBuf(c2, frame.data + 1);
	charToBuf(c3, frame.data + 2);
	monitorCan_send(&frame);
}

void monitorCan_send3CharShort(const short frameId, const char c1, const char c2, const char c3, const __s16 s1) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 5;
	charToBuf(c1, frame.data);
	charToBuf(c2, frame.data + 1);
	charToBuf(c3, frame.data + 2);
	shortToBuf(s1, frame.data + 3);
	monitorCan_send(&frame);
}

/* Initialisation function, return 0 if successful */
int monitorCan_init() {
	getSocket();
	return error;
}

void montiorCan_sendCellVoltage(const unsigned char batteryIndex, const short cellIndex, const unsigned char isValid, const short vCell) {
	monitorCan_sendCharShortCharShort(0x3f0, batteryIndex, cellIndex, isValid, vCell);
}

void monitorCan_sendShuntCurrent(const unsigned char batteryIndex, const short cellIndex, const short iShunt) {
	monitorCan_sendChar2Shorts(0x3f1, batteryIndex, cellIndex, iShunt);
}

void monitorCan_sendMinCurrent(const unsigned char batteryIndex, const short cellIndex, const short minCurrent) {
	monitorCan_sendChar2Shorts(0x3f2, batteryIndex, cellIndex, minCurrent);
}

void monitorCan_sendTemperature(const unsigned char batteryIndex, const short cellIndex, const short temperature) {
	monitorCan_sendChar2Shorts(0x3f3, batteryIndex, cellIndex, temperature);
}

void monitorCan_sendHardware(const unsigned char batteryIndex, const short cellIndex,
		const unsigned char hasKelvinConnection, const unsigned char hasResistorShunt,
		const unsigned char hasTemperatureSensor, const unsigned short revision, const unsigned char isClean) {
	unsigned char value = 0;
	if (hasKelvinConnection) {
		value |= 0x1;
	}
	if (hasResistorShunt) {
		value |= 0x2;
	}
	if (hasTemperatureSensor) {
		value |= 0x4;
	}
	if (isClean) {
		value |= 0x8;
	}
	monitorCan_sendChar2ShortsChar(0x3f4, batteryIndex, cellIndex, revision, value);
}

void monitorCan_sendError(const unsigned char batteryIndex, const short cellIndex, const short errorCount) {
	monitorCan_sendChar2Shorts(0x3f5, batteryIndex, cellIndex, errorCount);
}

void monitorCan_sendLatency(const unsigned char batteryIndex, const short cellIndex, const unsigned char latency) {
	monitorCan_sendCharShortChar(0x3f6, batteryIndex, cellIndex, latency);
}

void monitorCan_sendChargerState(const unsigned char shutdown, const unsigned char state, const unsigned char reason,
		const __s16 shuntDelay) {
	monitorCan_send3CharShort(0x3f8, shutdown, state, reason, shuntDelay);
}

void monitorCan_sendMonitorState(const monitor_state_t state, __u16 delay, const __u8 loopsBeforeVoltage) {
	monitorCan_sendCharShortChar(0x3f9, state, delay, loopsBeforeVoltage);
}

void monitorCan_sendChar2Shorts(const short frameId, const char c, const short s1, const short s2) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 5;
	charToBuf(c, frame.data);
	shortToBuf(s1, frame.data + 1);
	shortToBuf(s2, frame.data + 3);
	monitorCan_send(&frame);
}

void monitorCan_send2Shorts(const short frameId, const short s1, const short s2) {
	struct can_frame frame;
	memset(&frame, 0, sizeof(struct can_frame)); /* init CAN frame, e.g. DLC = 0 */
	frame.can_id = frameId;
	frame.can_dlc = 4;
	shortToBuf(s1, frame.data);
	shortToBuf(s2, frame.data + 2);
	monitorCan_send(&frame);
}

/* Returns true if there is an error */
char monitorCan_send(struct can_frame *frame) {
//	fprintf(stderr, "\n");
//	fprint_long_canframe(stderr, frame, "\n", 0);
	int s = getSocket();
	if (error) {
		fprintf(stdout, "error getting socket");
		fprintf(stderr, "error getting socket");
		return 1;
	}
	int nbytes = write(s, frame, sizeof(struct can_frame));
	if (nbytes != sizeof(struct can_frame)) {
		printf("error writing can frame %d", nbytes);
		fprintf(stderr, "error writing can frame %d", nbytes);
		error = 1;
		return 1;
	}
	return 0;
}

