/*
 * Mirics MSi001 tuner driver
 *
 * Copyright (C) 2012 by Eric Wild <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#define c(v) ((v<0)?printf("err! %s %s:%i\n", #v, __FILE__, __LINE__):0)
#define MHZ(x)	((x)*1000*1000)
#define KHZ(x)	((x)*1000)

enum mode{
	AM_MODE1=0,
	AM_MODE2=1,
	VHF_MODE=2,
	B3_MODE=3,
	B45_MODE=4,
	BL_MODE=5
};

enum xtal{
	XTAL19_2Mz=0,
	XTAL22M=1,
	XTAL24_576M=2,
	XTAL26M=3,
	XTAL38_4M=4
};

enum am_mixgainred{
	r1_mixbu_p0_0 = 0,
	r1_mixbu_p0_6 = 1,
	r1_mixbu_p0_12 = 2,
	r1_mixbu_p0_18 = 3,
	r1_mixbu_p1_24 = 3
};

struct state{
	enum mode m;
	enum xtal x;
	double freq_hz;
	uint32_t minus_bbgain;
	enum am_mixgainred am_mixgainred;
	uint32_t mixl;
	uint32_t lnagr;
	uint32_t reg[6];
};

struct r0_modes_{
	char* bits;
	unsigned char value;
	char* name;
	unsigned char lodiv;
};

struct iffreqs_{
	uint32_t xtalfreq;
	uint32_t fref1;
	uint32_t fif1;
};

int msi001_init(void *dev, uint32_t freq);

//######
#define R0_FIL_MODE_SH 12
#define R0_FIL_BW_SH 14
#define R0_XTAL_SEL_SH 17
#define R0_IF_LPMODE_SH 20
#define R0_VCO_LPMODE_SH 23

#define FIL_MODE_450K_IF 0x2
#define FIL_MODE_ZERO_IF 0x3

//######
#define R2_INT_SH 16

//######
#define R1_MIXBU_SH 10
#define R1_MIXL_SH 12
#define R1_LNAGR_SH 13
#define R1_DCCAL_SH 14

#define R1_DCCAL 0x05 // continuous, no speedup
//######
