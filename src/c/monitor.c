/*
 Copyright 2009-2012 Tom Parker

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
#define _GNU_SOURCE

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <confuse.h>

#include "monitor.h"
#include "config.h"
#include "crc.h"
#include "chargercontrol.h"
#include "chargeAlgorithm.h"
#include "canEventListener.h"
#include "buscontrol.h"
#include "soc.h"
#include "monitor_can.h"
#include "logger.h"
#include "console.h"
#include "util.h"

#define BAUDRATE B9600
#define _POSIX_SOURCE 1 /* POSIX compliant source */
#define FALSE 0
#define TRUE 1

#define CELL_ID_FILE "cells.txt"
#define DEBUG 0
#define HAS_KELVIN_CONNECTION 0
#define CHARGER_RELAY_PORT 7
#define SHUNT_MAX_CURRENT 150
#define CHARGE_CURRENT_OVERSAMPLING 5

#define ESCAPE_CHARACTER 0xff
#define START_OF_PACKET 0xfe
#define EVD5_BINSTATUS_LENGTH 20
#define EVD5_SUMMARY_3_LENGTH 13
#define EVD5_SUMMARY_4_LENGTH 11

void initData(struct config_t *config);
void sendCommand(struct status_t *cell, unsigned char command);
void getCellStates();
char getCellState(struct status_t *cell);
char _getCellState(struct status_t *status, int attempts);
void decodeBinStatus(unsigned char *buf, struct status_t *to);
void writeSlowly(int fd, unsigned char *s, int length);
crc_t writeCrc(unsigned char c, crc_t crc);
crc_t writeWithEscapeCrc(unsigned char c, crc_t crc);
void writeWithEscape(unsigned char c);
int readEnough(int fd, unsigned char *buf, int length);
unsigned char readPacket(struct status_t *cell, unsigned char *buf, unsigned char length, struct timeval *end);
unsigned short maxVoltageInAnyBattery();
unsigned short maxVoltage(struct battery_t *battery);
unsigned short maxVoltageCell(struct battery_t *battery);
unsigned short minVoltage(struct battery_t *battery);
unsigned short minVoltageCell(struct battery_t *battery);
unsigned short avgVoltage(struct battery_t *battery);
unsigned int totalVoltage(struct battery_t *battery);
unsigned char setShuntCurrent(struct config_t *config, struct battery_t *battery);
unsigned char setMinCurrent(struct status_t *cell, unsigned short minCurrent);
void dumpBuffer(unsigned char *buf, int length);
void findCells();
double asDouble(int s);
unsigned char turnOffAllShunts();
char isAnyCellShunting();
char isCellShunting(struct status_t *cell);
void flushInputBuffer();
unsigned char getCellVersion(struct status_t *cell);
void getSlaveVersions();

unsigned char shuntPause = 0;

int fd;

struct monitor_t data;

void decodeSummary3(unsigned char *buf, struct status_t *to) {
	if (!shuntPause) {
		to->iShunt = bufToShortLE(buf + 3);
	}
	if (!isCellShunting(to)) {
		to->vCell = bufToShortLE(buf + 5);
	}
	to->vShunt = bufToShortLE(buf + 7);
	to->temperature = bufToShortLE(buf + 9);
}

void decodeSummary4(unsigned char *buf, struct status_t *to) {
	if (!shuntPause) {
		to->iShunt = bufToShortLE(buf + 3);
	}
	if (!isCellShunting(to)) {
		to->vCell = bufToShortLE(buf + 5);
	}
	to->temperature = bufToShortLE(buf + 7);
}

char _getCellSummary(struct status_t *status, int maxAttempts) {
	for (int attempt = 0; TRUE; attempt++) {
		if (attempt >= maxAttempts) {
			return 0;
			fprintf(stderr, "%d bus errors, exiting\n", attempt);
			chargercontrol_shutdown();
			exit(1);
		}
		if (attempt > 0) {
			fprintf(stderr, "no response from %d (id %d) in %s, resetting\n", status->cellIndex, status->cellId,
					status->battery->name);
			buscontrol_setBus(FALSE);
			buscontrol_setBus(TRUE);
		}
		unsigned char buf[EVD5_SUMMARY_3_LENGTH];
		struct timeval start, end;
		gettimeofday(&start, NULL);
		sendCommand(status, 's');
		if (status->version == 3) {
			if (!readPacket(status, buf, EVD5_SUMMARY_3_LENGTH, &end)) {
				continue;
			}
		} else if (status->version == 4) {
			if (!readPacket(status, buf, EVD5_SUMMARY_4_LENGTH, &end)) {
				continue;
			}
		}
		unsigned short recievedCellId =	bufToShortLE(buf + 1);
		if (status->cellId != recievedCellId) {
			fprintf(stderr, "\nSent message to %2d (id %2d) in %s but received response from 0x%x\n", status->cellIndex,
					status->cellId, status->battery->name, recievedCellId);
			dumpBuffer(buf, EVD5_SUMMARY_3_LENGTH);
			flushInputBuffer();
			continue;
		}
		if (status->version == 3) {
				decodeSummary3(buf, status);
		} else if (status->version == 4) {
			decodeSummary4(buf, status);
		}
		status->latency = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
		monitorCan_sendLatency(status->battery->batteryIndex, status->cellIndex, status->latency / 1000);
		break;
	}
	return 1;
}

char getCellSummary(struct status_t *cell) {
	// if we didn't get the cell version at startup, try again
	if (cell->version == (char) -1) {
		if (!getCellVersion(cell)) {
			return FALSE;
		}
	}
	char success = _getCellSummary(cell, 2);
	if (!success) {
		cell->errorCount++;
		monitorCan_sendError(cell->battery->batteryIndex, cell->cellIndex, cell->errorCount);
		fprintf(stderr, "bus errors talking to cell %d (id %d) in %s, exiting\n", cell->cellIndex, cell->cellId,
				cell->battery->name);
		chargercontrol_shutdown();
		return FALSE;
	}
	return TRUE;
}

/** turn shunting off on any cells without a kelvin connection and with a resistor shunt */
unsigned char turnOffNonKelvinResistorShunts() {
	unsigned char changed = 0;
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			if (!cell->isKelvinConnection && cell->isResistorShunt) {
				changed |= setMinCurrent(cell, 0);
			}
		}
	}
	return changed;
}

/** turn shunting off on any cells without a kelvin connection and with a transistor shunt */
unsigned char turnOffNonKelvinTransistorShunts() {
	unsigned char changed = 0;
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			if (!cell->isKelvinConnection && !cell->isResistorShunt) {
				changed |= setMinCurrent(cell, 0);
			}
		}
	}
	return changed;
}


int main() {
	struct termios oldtio, newtio;

	struct config_t *config = getConfig();
	if (!config) {
		printf("error reading configuration file\n");
		return 1;
	}
	initData(config);

	canEventListener_init(config);

	if (chargercontrol_init()) {
		return 1;
	}

	if (buscontrol_init()) {
		return 1;
	}

	if (soc_init()) {
		return 1;
	}

	if (monitorCan_init()) {
		return 1;
	}

	if (logger_init(config)) {
		return 1;
	}

	console_init(config);
	chargeAlgorithm_init(config);

	buscontrol_setBus(TRUE);
	fd = open(config->serialPort, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror(config->serialPort);
		return -1;
	}

	tcgetattr(fd, &oldtio); /* save current port settings */

	bzero(&newtio, sizeof(newtio));
	newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;

	/* set input mode (non-canonical, no echo,...) */
	newtio.c_lflag = 0;

	newtio.c_cc[VTIME] = 1;
	newtio.c_cc[VMIN] = 10;

	tcflush(fd, TCIFLUSH);
	tcsetattr(fd, TCSANOW, &newtio);

	// send a byte to wake up the slaves
	writeWithEscape('a');

	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			sendCommand(cell, 'r');
		}
	}

	sleep(1);

	// send some bytes to wake up the slaves (they drop characters while flashing the light)
	writeWithEscape('a');

	// findCells();

	// clear the screen
	write(1, "\E[H\E[2J", 7);

	getSlaveVersions();
	turnOffAllShunts();
	sleep(1);

	time_t last = 0;
	for (int count = 0; TRUE; count++) {
		monitorCan_sendMonitorState(START, 0, count % 5);
		time_t t;
		time(&t);
		while (t < last + config->loopDelay) {
			monitorCan_sendMonitorState(SLEEPING, (last + config->loopDelay) - t, count % 5);
			sleep(1);
			time(&t);
		}
		last = t;
		if (config->loopDelay > 30) {
			monitorCan_sendMonitorState(WAKE_SLAVE, 0, count % 5);
			// if the slaves have gone to sleep, send some characters to wake them up
			writeWithEscape('a');
			// wait for slaves to wake up and take a measurement
			sleep(2);
		}

		// if necessary, turn off shunts and read the voltage
		shuntPause = turnOffNonKelvinResistorShunts();
		if (count % 5 == 0) {
			monitorCan_sendMonitorState(TURN_OFF_NON_KELVIN_TRANSISTOR, 0, count % 5);
			shuntPause = turnOffNonKelvinTransistorShunts() || shuntPause;
		}
		if (shuntPause) {
			// give cells time to read their real voltage
			monitorCan_sendMonitorState(WAIT_FOR_VOLTAGE_READING, 0, count % 5);
			sleep(2);
		}
		monitorCan_sendMonitorState(READ_VOLTAGE, 0, count % 5);
		getCellStates();

		// turn (back) on any shunts that are needed
		shuntPause = FALSE;
		unsigned char shuntValueChanged = FALSE;
		for (unsigned char i = 0; i < data.batteryCount; i++) {
			monitorCan_sendMonitorState(TURN_ON_SHUNTS, i, count % 5);
			shuntValueChanged |= setShuntCurrent(config, &data.batteries[i]);
		}
		// if we turned on any shunts, read the shunt current
		if (shuntValueChanged) {
			// give cells a chance re-read
			monitorCan_sendMonitorState(WAIT_FOR_SHUNT_CURRENT, 0, count % 5);
			sleep(2);
			// read the current
			monitorCan_sendMonitorState(READ_CURRENT, 0, count % 5);
			getCellStates();
		}
	}
	tcsetattr(fd, TCSANOW, &oldtio);
}

void getCellStates() {
	// move to the top of the screen
	write(1, "\E[H", 3);
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			char success = getCellSummary(cell);
			cell->isDataCurrent = success;
			if (!success) {
				continue;
			}
			montiorCan_sendCellVoltage(i, j, !isCellShunting(cell), cell->vCell);
			if (!shuntPause) {
				monitorCan_sendShuntCurrent(i, j, cell->iShunt);
				monitorCan_sendMinCurrent(i, j, cell->minCurrent);
			}
			if (cell->hasTemperatureSensor) {
				monitorCan_sendTemperature(i, j, cell->temperature);
			}
		}
	}
}

char getCellState(struct status_t *cell) {
	// if we didn't get the cell version at startup, try again
	if (cell->version == (char) -1) {
		if (!getCellVersion(cell)) {
			return FALSE;
		}
	}
	char success = _getCellState(cell, 2);
	if (!success) {
		cell->errorCount++;
		monitorCan_sendError(cell->battery->batteryIndex, cell->cellIndex, cell->errorCount);
		fprintf(stderr, "bus errors talking to cell %d (id %d) in %s, exiting\n", cell->cellIndex, cell->cellId,
				cell->battery->name);
		chargercontrol_shutdown();
		return FALSE;
	}
	return TRUE;
}

char _getCellState(struct status_t *status, int maxAttempts) {
	for (int attempt = 0; TRUE; attempt++) {
		if (attempt >= maxAttempts) {
			return 0;
			fprintf(stderr, "%d bus errors, exiting\n", attempt);
			chargercontrol_shutdown();
			exit(1);
		}
		if (attempt > 0) {
			fprintf(stderr, "no response from %d (id %d) in %s, resetting\n", status->cellIndex, status->cellId,
					status->battery->name);
			buscontrol_setBus(FALSE);
			buscontrol_setBus(TRUE);
		}
		unsigned char buf[EVD5_BINSTATUS_LENGTH];
		struct timeval start, end;
		gettimeofday(&start, NULL);
		sendCommand(status, '/');
		if (!readPacket(status, buf, EVD5_BINSTATUS_LENGTH, &end)) {
			continue;
		}
		unsigned short recievedCellId =	bufToShortLE(buf + 1);
		if (status->cellId != recievedCellId) {
			fprintf(stderr, "\nSent message to %2d (id %2d) in %s but received response from 0x%x\n", status->cellIndex,
					status->cellId, status->battery->name, recievedCellId);
			dumpBuffer(buf, EVD5_BINSTATUS_LENGTH);
			flushInputBuffer();
			continue;
		}
		decodeBinStatus(buf, status);
		status->latency = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
		break;
	}
	return 1;
}

unsigned char readPacket(struct status_t *cell, unsigned char *buf, unsigned char length, struct timeval *end) {
	unsigned char actualLength = 0;
	unsigned char escape = FALSE;
	while (actualLength != length) {
		if (!readEnough(fd, buf + actualLength, 1)) {
			fprintf(stderr, "read %d, expected %d from cell %d (id %2d) in %s\n", actualLength, length,
					cell->cellIndex, cell->cellId, cell->battery->name);
			dumpBuffer(buf, actualLength);
			return 0;
		}
		unsigned char read = buf[actualLength];
		if (read == 0xff && !escape) {
			escape = TRUE;
			continue;
		}
		if (actualLength == 0) {
			if (read == 0xfe && !escape) {
				actualLength++;
			}
			continue;
		}
		escape = FALSE;
		actualLength++;
	}
	gettimeofday(end, NULL);
	crc_t actualCrc = crc_init();
	actualCrc = crc_update(actualCrc, buf, length - 2);
	actualCrc = crc_finalize(actualCrc);
	unsigned short *receivedCrc = (unsigned short *) (buf + length - sizeof(crc_t));
	if (actualCrc != *receivedCrc) {
		fprintf(stderr, "\nSent message to %2d (id %2d) in %s, received CRC 0x%04x calculated 0x%04x\n", cell->cellIndex,
				cell->cellId, cell->battery->name, *receivedCrc, actualCrc);
		dumpBuffer(buf, actualLength);
		return 0;
	}
	return actualLength;
}

void decodeBinStatus(unsigned char *buf, struct status_t *to) {
	if (!shuntPause) {
		to->iShunt = bufToShortLE(buf + 3);
	}
	if (!isCellShunting(to)) {
		to->vCell = bufToShortLE(buf + 5);
	}
	to->vShunt = bufToShortLE(buf + 7);
	to->temperature = bufToShortLE(buf + 9);
	to->minCurrent = bufToShortLE(buf + 11);
	to->gainPot = buf[13];
	to->vShuntPot = buf[14];
	to->hasRx = buf[15];
	to->softwareAddressing = buf[16];
	to->automatic = buf[17];
}

/** turn shunting off on any cells that are shunting */
unsigned char turnOffAllShunts() {
	unsigned char changed = 0;
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			changed |= setMinCurrent(cell, 0);
		}
	}
	return changed;
}

unsigned short getMaxTemperature(struct battery_t *battery) {
	unsigned short result = 0;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		struct status_t *cell = battery->cells + i;
		if (cell->hasTemperatureSensor) {
			if (cell->temperature > result) {
				result = cell->temperature;
			}
		}
	}
	return result;
}

void setShuntForcedOnForShuntingCells() {
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			if (cell->targetShuntCurrent != 0) {
				cell->isShuntForcedOn = TRUE;
			}
		}
	}

}

void clearShuntForcedOn() {
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			cell->isShuntForcedOn = FALSE;
		}
	}
}

unsigned char setShuntCurrent(struct config_t *config, struct battery_t *battery) {
	if (soc_getCurrent() < -3) {
		return FALSE;
	}
	unsigned short maxTemperature = getMaxTemperature(battery);
	unsigned short maxShuntCurrent;
	if (maxTemperature < 3000) {
		maxShuntCurrent = 450;
	} else if (maxTemperature < 4000) {
		maxShuntCurrent = 400;
	} else if (maxTemperature < 5000) {
		maxShuntCurrent = 300;
	} else {
		maxShuntCurrent = 150;
	}
	unsigned short min = minVoltage(battery);
	unsigned char changed = FALSE;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		struct status_t *cell = battery->cells + i;
		unsigned short target;
		if (cell->vCell < config->minVoltageSocRelevant) {
			target = 0;
		} else if (cell->vCell < min + config->voltageDeadband) {
			target = 0;
		} else {
			target = maxShuntCurrent;
		}
		if (target < config->minShuntCurrent) {
			target = config->minShuntCurrent;
		}
		changed |= setMinCurrent(cell, target);
	}
	return changed;
}

unsigned char setMinCurrent(struct status_t *cell, unsigned short minCurrent) {
	if (cell->version == (char) -1) {
		return FALSE;
	}
	if (cell->minCurrent == minCurrent && cell->targetShuntCurrent == minCurrent) {
		return FALSE;
	}
	cell->targetShuntCurrent = minCurrent;
	for (int i = 0; i < 20; i++) {
		if (cell->minCurrent == minCurrent) {
			return TRUE;
		}
		if (minCurrent != 0 && (minCurrent < 150 || minCurrent > 450)) {
			chargercontrol_shutdown();
			fprintf(stderr, "internal error, %d cannot be honoured by cell", minCurrent);
			exit(1);
		}
		char cmd = 0x30 + minCurrent / 50;
		sendCommand(cell, cmd);
		getCellState(cell);
	}
	// couldn't get to desired current after 20 attempts???
	chargercontrol_shutdown();
	fprintf(stderr, "%2d (id %2d) in %s trying to get to %d but had %d\n", cell->cellIndex, cell->cellId,
			cell->battery->name, minCurrent, cell->minCurrent);
	return FALSE;
}

unsigned short minVoltage(struct battery_t *battery) {
	unsigned short result = 0xffff;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		if (battery->cells[i].vCell < result) {
			result = battery->cells[i].vCell;
		}
	}
	return result;
}

unsigned short minVoltageCell(struct battery_t *battery) {
	unsigned short min = 0xffff;
	unsigned short result = 0;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		if (battery->cells[i].vCell < min) {
			min = battery->cells[i].vCell;
			result = i;
		}
	}
	return result;
}

unsigned short maxVoltageInAnyBattery() {
	unsigned short result = 0;
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		unsigned short max = maxVoltage(&data.batteries[i]);
		if (result < max) {
			result = max;
		}
	}
	return result;
}

unsigned short maxVoltage(struct battery_t *battery) {
	unsigned short result = 0;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		if (battery->cells[i].vCell > result) {
			result = battery->cells[i].vCell;
		}
	}
	return result;
}

unsigned short maxVoltageCell(struct battery_t *battery) {
	unsigned short max = 0;
	unsigned short result = 0;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		if (battery->cells[i].vCell > max) {
			max = battery->cells[i].vCell;
			result = i;
		}
	}
	return result;
}

unsigned int totalVoltage(struct battery_t *battery) {
	int result = 0;
	for (unsigned short i = 0; i < battery->cellCount; i++) {
		result += battery->cells[i].vCell;
	}
	return result;
}

unsigned short avgVoltage(struct battery_t *battery) {
	return totalVoltage(battery) / (battery->cellCount);
}

/**
 * @return true if any cell is shunting
 */
char isAnyCellShunting() {
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			if (battery->cells[j].targetShuntCurrent > 0 ||
					battery->cells[j].minCurrent > 0) {
				return 1;
			}
		}
	}
	return 0;
}

/**
 * Returns true if the passed cell or an adjacent cell is shunting current. This works
 * even when cells are out of order because each board has it's own end connections and we are careful
 * not to reorder cells within a board.
 */
char isCellShunting(struct status_t *cell) {
	if (HAS_KELVIN_CONNECTION) {
		return 0;
	}
	if (cell->targetShuntCurrent != 0) {
		return 1;
	}
	if (cell->cellIndex != 0 && (cell - 1)->targetShuntCurrent != 0) {
		return 1;
	}
	if (cell->cellIndex + 1 < cell->battery->cellCount && (cell + 1)->targetShuntCurrent != 0) {
		return 1;
	}
	return 0;
}

void sendCommand(struct status_t *cell, unsigned char command) {
	// we're sending "SXXZCC"
	crc_t crc = crc_init();
	crc = writeCrc(START_OF_PACKET, crc);
	crc = writeWithEscapeCrc(cell->cellId & 0x00FF, crc);
	crc = writeWithEscapeCrc((cell->cellId & 0xFF00) >> 8, crc);
	crc = writeWithEscapeCrc(command, crc);
	crc = crc_finalize(crc);
	writeWithEscape(crc & 0x00FF);
	writeWithEscape((crc & 0xFF00) >> 8);
}

crc_t writeCrc(unsigned char c, crc_t crc) {
	writeSlowly(fd, &c, 1);
	return crc_update(crc, &c, 1);
}

crc_t writeWithEscapeCrc(unsigned char c, crc_t crc) {
	if (c == START_OF_PACKET || c == ESCAPE_CHARACTER) {
		unsigned char ff = 0xff;
		writeSlowly(fd, &ff, 1);
	}
	writeSlowly(fd, &c, 1);
	return crc_update(crc, &c, 1);
}

void writeWithEscape(unsigned char c) {
	if (c == START_OF_PACKET || c == ESCAPE_CHARACTER) {
		unsigned char ff = 0xff;
		writeSlowly(fd, &ff, 1);
	}
	writeSlowly(fd, &c, 1);
}

void writeSlowly(int fd, unsigned char *s, int length) {
	//printf("%s\n", s);
	for (int i = 0; i < length; i++) {
		write(fd, s + i, 1);
		//usleep(100000);
	}
}

int readEnough(int fd, unsigned char *buf, int length) {
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	tv.tv_sec = 0;
	tv.tv_usec = 200000;

	int actual = 0;
	for (int i = 0; i < 5; i++) {
		int retval = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (retval == -1) {
			fflush(NULL);
			return actual;
		}
		if (!retval) {
			fflush(NULL);
			continue;
		}
		actual += read(fd, buf + actual, length - actual);
		if (DEBUG) {
			fprintf(stderr, "read %d expecting %d: ", actual, length);
			for (int j = 0; j < actual; j++) {
				fprintf(stderr, "%02x ", buf[j]);
			}
			fprintf(stderr, "\n");
		}
		if (actual >= length) {
			break;
		}
	}
	return actual;
}

/** read all the data in the input buffers, used instead of a start of message byte to re-sync */
void flushInputBuffer() {
	unsigned char buf[255];
	int length = readEnough(fd, buf, 255);
	do {
		length = readEnough(fd, buf, 255);
		fprintf(stderr, "read %d more\n", length);
		dumpBuffer(buf, length);
	} while (length > 0);
}

void dumpBuffer(unsigned char *buf, int length) {
	if (DEBUG) {
		for (int i = 0; i < length; i++) {
			fprintf(stderr, "%02d %02x\n", i, buf[i]);
		}
	}
}

double asDouble(int s) {
	return ((double) s) / 1000;
}

unsigned char _getCellVersion(struct status_t *cell) {
	cell->version = 3;
	sendCommand(cell, '?');
	unsigned char buf[16];
	struct timeval end;
	if (!readPacket(cell, buf, 17, &end)) {
		return FALSE;
	}
	short cellId = bufToShortLE(buf + 1);
	if (cell->cellId != cellId) {
		fprintf(stderr, "sent getVersion to %4d, got reply from %4d\n", cell->cellId, cellId);
		return 0;
	}
	cell->version = buf[3];
	cell->isKelvinConnection = buf[4];
	cell->isResistorShunt = buf[5];
	cell->isHardSwitchedShunt = buf[6];
	cell->hasTemperatureSensor = buf[7];
	cell->revision = bufToShortLE(buf + 8);
	cell->isClean = buf[10];
	cell->whenProgrammed = bufToLongLE(buf + 11);
	return 1;
}

/**
 * Obtain version information about the specified cell and store it in the passed cell structure
 * @return true if version information was successfully obtained
 */
unsigned char getCellVersion(struct status_t *cell) {
	for (int i = 0; i < 3; i++) {
		if (_getCellVersion(cell)) {
			return TRUE;
		}
		cell->errorCount++;
		monitorCan_sendError(cell->battery->batteryIndex, cell->cellIndex, cell->errorCount);
	}
	fprintf(stderr, "error getting version for cell %d (id %d)\n", cell->cellIndex, cell->cellId);
	cell->version = -1;
	return FALSE;
}

/** Interrogate cells and discover their version number */
void getSlaveVersions() {
	for (unsigned char i = 0; i < data.batteryCount; i++) {
		struct battery_t *battery = data.batteries + i;
		for (unsigned short j = 0; j < battery->cellCount; j++) {
			struct status_t *cell = battery->cells + j;
			getCellVersion(cell);
			monitorCan_sendHardware(i, j, cell->isKelvinConnection, cell->isResistorShunt, cell->isHardSwitchedShunt,
					cell->revision, cell->isClean);
		}
	}
}

void findCells() {
	struct status_t status;
	struct battery_t battery;
	battery.name = "findCells";
	status.battery = &battery;
	for (unsigned short i = 0; i < 255; i++) {
		status.cellId = i;
		if (getCellVersion(&status)) {
			printf("found cell at %d\n", i);
		} else {
			printf("found nothing at %d\n", i);
		}
	}
}

void initData(struct config_t *config) {
	data.batteryCount = config->batteryCount;
	data.batteries = malloc(sizeof(struct battery_t) * data.batteryCount);
	for (unsigned char j = 0; j < data.batteryCount; j++) {
		struct battery_t *battery = data.batteries + j;
		battery->batteryIndex = j;
		battery->name = config->batteries[j].name;
		battery->cellCount = config->batteries[j].cellCount;
		battery->cells = calloc(sizeof(struct status_t), battery->cellCount);
		for (unsigned short k = 0; k < battery->cellCount; k++) {
			struct status_t *cell = battery->cells + k;
			cell->cellIndex = k;
			cell->cellId = config->batteries[j].cellIds[k];
			cell->battery = battery;
			cell->errorCount = 0;
			cell->isDataCurrent = FALSE;
			cell->minCurrent = 999;
			cell->isShuntForcedOn = FALSE;
		}
	}
}

const char *monitor_getStateString(monitor_state_t state) {
	switch (state) {
	case START :
		return "Start";
	case SLEEPING :
		return "Sleeping";
	case WAKE_SLAVE :
		return "Waking Slaves";
	case TURN_OFF_NON_KELVIN_TRANSISTOR :
		return "Turn off shunts";
	case WAIT_FOR_VOLTAGE_READING :
		return "Wait for valid data";
	case READ_VOLTAGE :
		return "Reading voltage";
	case TURN_ON_SHUNTS :
		return "Turn on shunts";
	case WAIT_FOR_SHUNT_CURRENT :
		return "Wait for data";
	case READ_CURRENT :
		return "Reading current";
	}
	return "unknown";
}
